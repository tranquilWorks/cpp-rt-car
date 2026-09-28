#pragma once
// Copyable teaching implementation. Lifecycle/registry calls are control-thread
// only and require submit/poll/cancel callers to have joined. See the guide.
#include "profile.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>

namespace backend_kit {

class Backend {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    static_assert(std::atomic<std::int32_t>::is_always_lock_free);
    static_assert(std::atomic<Fault>::is_always_lock_free);
    struct Buffer { void* data{}; std::uint64_t bytes{}, token{}; std::uint32_t flags{}; };
    std::array<Buffer, 2> buffers_{};
    // free -> writing -> pending -> retiring -> free. Each CAS tries once.
    enum : unsigned { free, writing, pending, retiring };
    std::atomic<unsigned> slot_{free};
    rt::HalV2Submission command_{};
    std::chrono::steady_clock::time_point accepted_{};
    std::atomic<Fault> fault_{Fault::none};
    bool owned_ = false, initialized_ = false;
    std::uint64_t limit_ = 0, next_token_ = 0, generation_ = 0, resets_ = 0;
    std::atomic<unsigned> health_{static_cast<unsigned>(rt::HalV2HealthState::shutdown)};
    std::atomic<std::int32_t> last_{0};
    std::atomic<std::uint64_t> submissions_{0}, completions_{0}, rejected_{0},
        timeouts_{0}, errors_{0}, losses_{0}, canceled_{0};
    bool take(Fault f) noexcept {
        return fault_.compare_exchange_strong(f, Fault::none, std::memory_order_relaxed);
    }
    Buffer* lookup(std::uint64_t token) noexcept {
        for (auto& b : buffers_) if (token != 0 && b.token == token) return &b;
        return nullptr;
    }
    static Backend& self(void* p) noexcept { return *static_cast<Backend*>(p); }
    static Status capabilities(void* p, rt::HalV2Capabilities* out) noexcept {
        if (!p || !out) return Status::invalid_argument;
        *out = {};
        out->max_in_flight = 1; out->max_registered_buffers = 2; out->max_buffer_bytes = 256;
        out->supports_cancel = 1; out->supports_reset = 1; out->deterministic_mock = 1;
        constexpr char name[] = "authoring.copy";
        std::copy_n(name, sizeof(name), out->backend_id.begin());
        out->control_storage_bytes = sizeof(Backend);
        return Status::ok;
    }
    static Status initialize(void* p, const rt::HalV2InitializeConfig* c) noexcept {
        if (!p || !c || c->struct_size < sizeof(*c) || c->api_version != rt::hal_v2_api_version ||
            !zero(c->reserved) || c->requested_in_flight != 1 ||
            c->requested_registered_buffers == 0 || c->requested_registered_buffers > 2)
            return Status::invalid_argument;
        auto& b = self(p);
        if (b.owned_) return Status::invalid_state;
        if (b.generation_ == std::numeric_limits<std::uint64_t>::max()) return Status::resource_exhausted;
        b.owned_ = true; // Partial-start failure still owns this setup resource.
        if (b.take(Fault::initialize)) return Status::error;
        b.initialized_ = true; b.limit_ = c->requested_registered_buffers; ++b.generation_;
        b.health_.store(static_cast<unsigned>(rt::HalV2HealthState::healthy));
        b.last_.store(0);
        return Status::ok;
    }
    static Status register_buffer(void* p, const rt::HalV2BufferRegistration* r,
                                  std::uint64_t* token) noexcept {
        if (token) *token = 0;
        if (!p || !r || !token || r->struct_size < sizeof(*r) || !zero(r->reserved) ||
            !identifier(r->name) || !r->data || r->bytes == 0 || r->bytes > 256 ||
            r->flags == 0 || (r->flags & ~std::uint32_t{15}) != 0)
            return Status::invalid_argument;
        auto& b = self(p);
        if (!b.initialized_) return Status::invalid_state;
        if (b.take(Fault::registration)) return Status::error;
        if (b.next_token_ == std::numeric_limits<std::uint64_t>::max()) return Status::resource_exhausted;
        for (std::size_t i = 0; i < b.limit_; ++i) if (b.buffers_[i].token == 0) {
            *token = ++b.next_token_;
            b.buffers_[i] = {r->data, r->bytes, *token, r->flags};
            return Status::ok;
        }
        return Status::resource_exhausted;
    }
    static Status unregister_buffer(void* p, std::uint64_t token) noexcept {
        if (!p || token == 0) return Status::invalid_argument;
        auto& b = self(p);
        auto* buffer = b.lookup(token);
        if (!buffer) return Status::invalid_argument;
        // Conservatively retain every registration while any command is owned.
        if (b.slot_.load(std::memory_order_acquire) != free) return Status::invalid_state;
        if (b.take(Fault::unregister)) return Status::error;
        *buffer = {};
        return Status::ok;
    }
    static Status submit(void* p, const rt::HalV2Submission* s) noexcept {
        if (!p || !s || s->struct_size < sizeof(*s) || s->api_version != rt::hal_v2_api_version ||
            !zero(s->reserved) || s->submission_id == 0 || s->timeout_ns == 0 || s->flags != 0 ||
            s->opcode != copy_opcode || s->payload_size != 0 || s->buffer_count != 2)
            return Status::invalid_argument;
        auto& b = self(p);
        if (!b.initialized_) return Status::invalid_state;
        if (b.health_.load(std::memory_order_acquire) != static_cast<unsigned>(rt::HalV2HealthState::healthy))
            return Status::reset_required;
        for (std::size_t i = 0; i < 2; ++i) {
            const auto& r = s->buffers[i]; const auto* buffer = b.lookup(r.buffer_token);
            const auto access = i == 0 ? RTFW_DEVICE_ACCESS_READ : RTFW_DEVICE_ACCESS_WRITE;
            const auto flag = i == 0 ? RTFW_DEVICE_BUFFER_DEVICE_READ : RTFW_DEVICE_BUFFER_DEVICE_WRITE;
            if (!buffer || r.reserved0 != 0 || r.access != access || !(buffer->flags & flag) ||
                r.bytes == 0 || r.offset > buffer->bytes || r.bytes > buffer->bytes - r.offset)
                return Status::invalid_argument;
        }
        if (s->buffers[0].bytes != s->buffers[1].bytes) return Status::invalid_argument;
        unsigned expected = free;
        if (!b.slot_.compare_exchange_strong(expected, writing, std::memory_order_acquire)) {
            b.rejected_.fetch_add(1); return Status::queue_full;
        }
        b.command_ = *s; b.accepted_ = std::chrono::steady_clock::now();
        b.submissions_.fetch_add(1);
        b.slot_.store(pending, std::memory_order_release);
        return Status::ok;
    }
    static Status poll(void* p, rt::HalV2Completion* out, std::uint64_t capacity,
                       std::uint64_t* count) noexcept {
        if (count) *count = 0;
        if (!p || !count || !out || capacity == 0) return Status::invalid_argument;
        auto& b = self(p);
        if (!b.initialized_) return Status::invalid_state;
        unsigned expected = pending;
        if (!b.slot_.compare_exchange_strong(expected, retiring, std::memory_order_acquire)) return Status::ok;
        Status result = Status::ok;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - b.accepted_).count();
        if (b.take(Fault::completion_loss)) result = Status::lost;
        else if (b.take(Fault::completion_error)) result = Status::error;
        else if (b.take(Fault::completion_timeout) ||
                 (elapsed >= 0 && static_cast<std::uint64_t>(elapsed) >= b.command_.timeout_ns))
            result = Status::timeout;
        if (result == Status::ok) {
            const auto& src = b.command_.buffers[0]; const auto& dst = b.command_.buffers[1];
            std::memmove(static_cast<std::byte*>(b.lookup(dst.buffer_token)->data) + dst.offset,
                         static_cast<const std::byte*>(b.lookup(src.buffer_token)->data) + src.offset,
                         static_cast<std::size_t>(src.bytes));
        } else {
            if (result == Status::lost) b.losses_.fetch_add(1);
            else if (result == Status::timeout) b.timeouts_.fetch_add(1);
            else b.errors_.fetch_add(1);
            b.health_.store(static_cast<unsigned>(result == Status::lost ?
                rt::HalV2HealthState::lost : rt::HalV2HealthState::reset_required), std::memory_order_release);
        }
        b.last_.store(static_cast<std::int32_t>(result));
        *out = {}; out->submission_id = b.command_.submission_id; out->status = static_cast<std::int32_t>(result);
        out->value = result == Status::ok ? b.command_.buffers[0].bytes : 0;
        *count = 1; b.completions_.fetch_add(1);
        b.slot_.store(free, std::memory_order_release);
        return Status::ok;
    }
    static Status cancel(void* p, std::uint64_t id) noexcept {
        if (!p || id == 0) return Status::invalid_argument;
        auto& b = self(p);
        if (!b.initialized_) return Status::invalid_state;
        unsigned expected = pending;
        if (!b.slot_.compare_exchange_strong(expected, retiring, std::memory_order_acquire)) return Status::invalid_state;
        if (b.command_.submission_id != id) {
            b.slot_.store(pending, std::memory_order_release); return Status::invalid_argument;
        }
        b.canceled_.fetch_add(1); b.last_.store(static_cast<std::int32_t>(Status::canceled));
        b.slot_.store(free, std::memory_order_release); // Successful cancel retires; no completion follows.
        return Status::ok;
    }
    static Status health(void* p, rt::HalV2Health* out) noexcept {
        if (!p || !out) return Status::invalid_argument;
        auto& b = self(p); *out = {};
        out->state = b.health_.load(); out->last_status = b.last_.load();
        out->generation = b.generation_; out->resets = b.resets_;
        out->submissions = b.submissions_.load(); out->completions = b.completions_.load();
        out->queue_rejections = b.rejected_.load(); out->timeouts = b.timeouts_.load();
        out->errors = b.errors_.load(); out->losses = b.losses_.load(); out->cancellations = b.canceled_.load();
        out->outstanding = b.slot_.load() == free ? 0 : 1;
        return Status::ok;
    }
    static Status reset(void* p) noexcept {
        if (!p) return Status::invalid_argument;
        auto& b = self(p);
        if (!b.initialized_ || b.slot_.load() != free) return Status::invalid_state;
        if (b.generation_ == std::numeric_limits<std::uint64_t>::max()) return Status::resource_exhausted;
        ++b.generation_; ++b.resets_; b.last_.store(0);
        b.health_.store(static_cast<unsigned>(rt::HalV2HealthState::healthy));
        return Status::ok;
    }
    static Status shutdown(void* p) noexcept {
        if (!p) return Status::invalid_argument;
        auto& b = self(p);
        if (!b.owned_) return Status::invalid_state;
        if (b.slot_.load() != free || b.registered() != 0) return Status::invalid_state;
        if (b.take(Fault::shutdown)) return Status::error;
        b.owned_ = false; b.initialized_ = false; b.limit_ = 0;
        b.health_.store(static_cast<unsigned>(rt::HalV2HealthState::shutdown));
        return Status::ok;
    }
public:
    void inject(Fault f) noexcept { fault_.store(f); }
    // Fixture inspection: control thread only, after execution callers joined.
    bool owns_setup() const noexcept { return owned_; }
    std::size_t registered() const noexcept {
        return static_cast<std::size_t>(std::count_if(buffers_.begin(), buffers_.end(),
            [](const Buffer& b) { return b.token != 0; }));
    }
    rt::HalV2BackendApi api() noexcept {
        rt::HalV2BackendApi a; a.instance = this;
        a.get_capabilities = capabilities; a.initialize = initialize;
        a.register_buffer = register_buffer; a.unregister_buffer = unregister_buffer;
        a.submit = submit; a.poll = poll; a.cancel = cancel; a.get_health = health;
        a.reset = reset; a.shutdown = shutdown; return a;
    }
};
} // namespace backend_kit
