#include "provider.hpp"
#include <string>

namespace example {
Provider::Provider(void* user, Transform transform, Cleanup cleanup) noexcept
    : user_(user), transform_(transform ? transform : transform_default), cleanup_(cleanup) {}

b::ProviderV1 Provider::table() noexcept {
    b::ProviderV1 result;
    result.id = "example.transform";
    result.case_count = 2;
    result.user = this;
    result.describe = describe;
    result.invoke = invoke;
    return result;
}

b::Status Provider::describe(void*, std::size_t index, b::Descriptor& out) {
    if (index >= 2) return b::Status::not_found;
    const std::uint64_t n = index == 0 ? 64 : 1024;
    out = {};
    out.case_id = index == 0 ? "transform-64" : "transform-1024";
    out.subsystem = "third-party-example";
    out.implementation = "checked-integer-transform-v1";
    out.configuration = "host-owned-fixed-storage";
    out.workload_kind = "fill-transform-validate";
    out.workload_sha256 = b::sha256("example.transform.v1.elements-" + std::to_string(n));
    out.parameters = {{"elements", n, 64, 1024}};
    out.counters = {{"elements", "count", n, n}, {"written_bytes", "bytes", n * 8, n * 8}};
    return b::Status::ok;
}

b::Status Provider::prepare(std::string_view id, bool available) noexcept {
    if (prepared_) return b::Status::busy;
    if (id != "transform-64" && id != "transform-1024") return b::Status::not_found;
    // Retain static identifiers, never a borrowed caller string.
    selected_ = id == "transform-64" ? "transform-64" : "transform-1024";
    count_ = id == "transform-64" ? 64 : 1024;
    next_ = completed_ = 0;
    failed_ = false;
    available_ = available;
    prepared_ = true;
    return b::Status::ok;
}

bool Provider::transform_default(void*, const std::uint64_t* input, std::uint64_t* output, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) output[i] = 3 * input[i] + 7;
    return true;
}

b::Status Provider::invoke(void* user, std::string_view id, std::uint64_t ordinal, b::Observation& out) {
    auto& self = *static_cast<Provider*>(user);
    if (!self.prepared_ || id != self.selected_) return b::Status::invalid;
    if (self.failed_) return b::Status::provider_error;
    if (!self.available_) return b::Status::not_run;
    if (ordinal != self.next_ || ordinal >= 7) {
        self.failed_ = true;
        return b::Status::provider_error;
    }
    self.failed_ = true;  // Any exception/error poisons this session until finish.
    for (std::size_t i = 0; i < self.count_; ++i) {
        self.input_[i] = i + ordinal;
        self.output_[i] = b::max_integer;
    }
    if (!self.transform_(self.user_, self.input_.data(), self.output_.data(), self.count_))
        return b::Status::provider_error;
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < self.count_; ++i) {
        if (self.output_[i] != 3 * (i + ordinal) + 7) return b::Status::invariant_failed;
        sum += self.output_[i];
    }
    const auto n = static_cast<std::uint64_t>(self.count_);
    if (sum != n * (3 * ordinal + 7) + 3 * n * (n - 1) / 2) return b::Status::invariant_failed;
    out.counters = {n, n * 8};
    out.checksum = sum;
    out.correct = true;
    ++self.next_;
    ++self.completed_;
    self.failed_ = false;
    return b::Status::ok;
}

b::Status Provider::finish() noexcept {
    if (!prepared_) return b::Status::invalid;
    failed_ = true;  // No more invokes once cleanup begins, including retry.
    try {
        if (cleanup_ && !cleanup_(user_)) return b::Status::provider_error;
    } catch (...) {
        return b::Status::provider_error;
    }
    prepared_ = false;
    // Cleanup success releases ownership even after an invocation failed.
    return b::Status::ok;
}

b::Status run_to_directory(Provider& provider, std::string_view id, const b::ClockV1& clock,
                          const b::Identity& identity, const std::filesystem::path& output, bool available) {
    auto status = b::check_destination(output);
    if (status != b::Status::ok) return status;
    if (b::validate(identity) != b::Status::ok) return b::Status::invalid;
    b::Runner runner;
    b::ProviderHandle handle;
    status = runner.register_provider(provider.table(), handle);
    if (status != b::Status::ok) return status;
    status = provider.prepare(id, available);
    if (status != b::Status::ok) return status;
    try {
        const auto result = runner.run("example.transform", id, clock, identity);
        if (provider.finish() != b::Status::ok) return b::Status::provider_error;
        if (result.status != b::Status::ok && result.status != b::Status::not_run) return result.status;
        status = b::publish(result, output);
        return status == b::Status::ok ? result.status : status;
    } catch (...) {
        // A host allocation/serialization failure must also settle ownership.
        if (provider.owns_session()) static_cast<void>(provider.finish());
        return b::Status::provider_error;
    }
}
}  // namespace example
