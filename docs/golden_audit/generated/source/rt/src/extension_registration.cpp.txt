#include "extension_registration.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace {

using Transaction = rt::detail::ExtensionRegistrationTransaction;

template <std::size_t N>
bool reserved_zero(const std::uint64_t (&values)[N]) noexcept {
    return std::all_of(
        std::begin(values),
        std::end(values),
        [](std::uint64_t value) { return value == 0; });
}

bool valid_c_status(rtfw_status status) noexcept {
    return status <= RTFW_STATUS_OK && status >= RTFW_STATUS_INCOMPATIBLE_ABI;
}

bool valid_handle(
    const rtfw_extension_handle_v1& handle,
    const Transaction& transaction,
    std::uint32_t expected_kind) noexcept {
    if (handle.owner != transaction.owner ||
        handle.generation != transaction.generation ||
        handle.kind != expected_kind) {
        return false;
    }
    switch (expected_kind) {
    case RTFW_EXTENSION_HANDLE_PHASE:
        return handle.slot < transaction.phase_count;
    case RTFW_EXTENSION_HANDLE_BACKEND:
        return handle.slot < transaction.backend_count;
    case RTFW_EXTENSION_HANDLE_SERVICE:
        return handle.slot < transaction.service_count;
    case RTFW_EXTENSION_HANDLE_RESOURCE:
        return handle.slot < transaction.resource_count;
    default:
        return false;
    }
}

rtfw_status poison(Transaction& transaction, rt::Status status) noexcept {
    if (transaction.failure == rt::Status::ok) {
        transaction.failure = status;
    }
    return static_cast<rtfw_status>(status);
}

void zero_handle(rtfw_extension_handle_v1* handle) noexcept {
    if (handle) {
        *handle = {};
    }
}

rtfw_extension_handle_v1 make_handle(
    const Transaction& transaction,
    std::uint32_t kind,
    std::size_t slot) noexcept {
    return {
        transaction.owner,
        kind,
        static_cast<std::uint32_t>(slot),
        transaction.generation,
    };
}

bool valid_device_api(const rtfw_device_backend_api& api) noexcept {
    return api.struct_size >= sizeof(rtfw_device_backend_api) &&
           api.abi_version == RTFW_DEVICE_ABI_VERSION && api.instance &&
           api.get_capabilities && api.initialize && api.register_buffer &&
           api.unregister_buffer && api.submit && api.poll && api.cancel &&
           api.get_health && api.reset && api.shutdown &&
           reserved_zero(api.reserved);
}

bool valid_service_api(const rtfw_extension_service_api_v1& api) noexcept {
    return api.struct_size >= RTFW_EXTENSION_SERVICE_API_V1_REQUIRED_SIZE &&
           api.abi_version == RTFW_EXTENSION_ABI_VERSION && api.instance &&
           api.initialize && api.request_stop && api.quiesce && api.shutdown &&
           reserved_zero(api.reserved);
}

rtfw_status RTFW_EXTENSION_CALL stage_phase(
    void* opaque,
    const rtfw_extension_phase_v1* input,
    rtfw_extension_handle_v1* output) {
    zero_handle(output);
    if (!opaque) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    auto& transaction = *static_cast<Transaction*>(opaque);
    if (!input || !output ||
        input->struct_size < RTFW_EXTENSION_PHASE_V1_REQUIRED_SIZE ||
        input->abi_version != RTFW_EXTENSION_ABI_VERSION ||
        !rt::detail::valid_extension_identifier(
            input->name, RTFW_EXTENSION_IDENTIFIER_CAPACITY) ||
        !input->callback || !reserved_zero(input->reserved)) {
        return poison(transaction, rt::Status::invalid_argument);
    }
    if (transaction.phase_count >= transaction.phase_limit) {
        return poison(transaction, rt::Status::capacity_exceeded);
    }
    auto copy = *input;
    copy.struct_size = RTFW_EXTENSION_PHASE_V1_REQUIRED_SIZE;
    const auto slot = transaction.phase_count++;
    transaction.phases[slot] = copy;
    *output = make_handle(
        transaction, RTFW_EXTENSION_HANDLE_PHASE, slot);
    return RTFW_STATUS_OK;
}

rtfw_status RTFW_EXTENSION_CALL stage_backend(
    void* opaque,
    const rtfw_extension_backend_v1* input,
    rtfw_extension_handle_v1* output) {
    zero_handle(output);
    if (!opaque) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    auto& transaction = *static_cast<Transaction*>(opaque);
    if (!input || !output ||
        input->struct_size < RTFW_EXTENSION_BACKEND_V1_REQUIRED_SIZE ||
        input->abi_version != RTFW_EXTENSION_ABI_VERSION ||
        !rt::detail::valid_extension_identifier(
            input->name, RTFW_EXTENSION_IDENTIFIER_CAPACITY) ||
        !valid_device_api(input->api) || !reserved_zero(input->reserved)) {
        return poison(transaction, rt::Status::invalid_argument);
    }
    if (transaction.backend_count >= transaction.backend_limit) {
        return poison(transaction, rt::Status::capacity_exceeded);
    }
    auto copy = *input;
    copy.struct_size = RTFW_EXTENSION_BACKEND_V1_REQUIRED_SIZE;
    copy.api.struct_size = sizeof(rtfw_device_backend_api);
    const auto slot = transaction.backend_count++;
    transaction.backends[slot] = copy;
    *output = make_handle(
        transaction, RTFW_EXTENSION_HANDLE_BACKEND, slot);
    return RTFW_STATUS_OK;
}

rtfw_status RTFW_EXTENSION_CALL stage_service(
    void* opaque,
    const rtfw_extension_service_v1* input,
    rtfw_extension_handle_v1* output) {
    zero_handle(output);
    if (!opaque) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    auto& transaction = *static_cast<Transaction*>(opaque);
    if (!input || !output ||
        input->struct_size < RTFW_EXTENSION_SERVICE_V1_REQUIRED_SIZE ||
        input->abi_version != RTFW_EXTENSION_ABI_VERSION ||
        !rt::detail::valid_extension_identifier(
            input->name, RTFW_EXTENSION_IDENTIFIER_CAPACITY) ||
        !rt::detail::valid_extension_identifier(
            input->interface_name, RTFW_EXTENSION_IDENTIFIER_CAPACITY) ||
        input->interface_version == 0 || input->reserved0 != 0 ||
        !valid_service_api(input->api) || !reserved_zero(input->reserved)) {
        return poison(transaction, rt::Status::invalid_argument);
    }
    if (transaction.service_count >= RTFW_EXTENSION_SERVICE_CAPACITY) {
        return poison(transaction, rt::Status::capacity_exceeded);
    }
    auto copy = *input;
    copy.struct_size = RTFW_EXTENSION_SERVICE_V1_REQUIRED_SIZE;
    copy.api.struct_size = RTFW_EXTENSION_SERVICE_API_V1_REQUIRED_SIZE;
    const auto slot = transaction.service_count++;
    transaction.services[slot] = copy;
    *output = make_handle(
        transaction, RTFW_EXTENSION_HANDLE_SERVICE, slot);
    return RTFW_STATUS_OK;
}

rtfw_status RTFW_EXTENSION_CALL stage_resource(
    void* opaque,
    const rtfw_extension_resource_v1* input,
    rtfw_extension_handle_v1* output) {
    zero_handle(output);
    if (!opaque) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    auto& transaction = *static_cast<Transaction*>(opaque);
    if (!input || !output ||
        input->struct_size < RTFW_EXTENSION_RESOURCE_V1_REQUIRED_SIZE ||
        input->abi_version != RTFW_EXTENSION_ABI_VERSION ||
        !rt::detail::valid_extension_identifier(
            input->name, RTFW_EXTENSION_IDENTIFIER_CAPACITY) ||
        !reserved_zero(input->reserved)) {
        return poison(transaction, rt::Status::invalid_argument);
    }
    if (transaction.resource_count >= RTFW_EXTENSION_RESOURCE_CAPACITY) {
        return poison(transaction, rt::Status::capacity_exceeded);
    }
    auto copy = *input;
    copy.struct_size = RTFW_EXTENSION_RESOURCE_V1_REQUIRED_SIZE;
    const auto slot = transaction.resource_count++;
    transaction.resources[slot] = copy;
    *output = make_handle(
        transaction, RTFW_EXTENSION_HANDLE_RESOURCE, slot);
    return RTFW_STATUS_OK;
}

rtfw_status RTFW_EXTENSION_CALL stage_relationship(
    void* opaque,
    const rtfw_extension_relationship_v1* input) {
    if (!opaque) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    auto& transaction = *static_cast<Transaction*>(opaque);
    if (!input ||
        input->struct_size < RTFW_EXTENSION_RELATIONSHIP_V1_REQUIRED_SIZE ||
        input->abi_version != RTFW_EXTENSION_ABI_VERSION ||
        !reserved_zero(input->reserved)) {
        return poison(transaction, rt::Status::invalid_argument);
    }
    if (transaction.relationship_count >=
        RTFW_EXTENSION_RELATIONSHIP_CAPACITY) {
        return poison(transaction, rt::Status::capacity_exceeded);
    }
    bool valid = false;
    switch (input->kind) {
    case RTFW_EXTENSION_RELATIONSHIP_PHASE_DEPENDENCY:
        valid = input->access == RTFW_EXTENSION_RESOURCE_ACCESS_NONE &&
            valid_handle(
                input->first, transaction, RTFW_EXTENSION_HANDLE_PHASE) &&
            valid_handle(
                input->second, transaction, RTFW_EXTENSION_HANDLE_PHASE) &&
            input->first.slot != input->second.slot;
        break;
    case RTFW_EXTENSION_RELATIONSHIP_PHASE_RESOURCE:
        valid = (input->access == RTFW_EXTENSION_RESOURCE_ACCESS_READ ||
                 input->access == RTFW_EXTENSION_RESOURCE_ACCESS_WRITE) &&
            valid_handle(
                input->first, transaction, RTFW_EXTENSION_HANDLE_PHASE) &&
            valid_handle(
                input->second, transaction, RTFW_EXTENSION_HANDLE_RESOURCE);
        break;
    case RTFW_EXTENSION_RELATIONSHIP_SERVICE_BACKEND:
        valid = input->access == RTFW_EXTENSION_RESOURCE_ACCESS_NONE &&
            valid_handle(
                input->first, transaction, RTFW_EXTENSION_HANDLE_SERVICE) &&
            valid_handle(
                input->second, transaction, RTFW_EXTENSION_HANDLE_BACKEND);
        break;
    default:
        break;
    }
    if (!valid) {
        return poison(transaction, rt::Status::invalid_argument);
    }
    for (std::size_t index = 0;
         index < transaction.relationship_count; ++index) {
        const auto& existing = transaction.relationships[index];
        if (existing.kind == input->kind && existing.access == input->access &&
            existing.first.owner == input->first.owner &&
            existing.first.kind == input->first.kind &&
            existing.first.slot == input->first.slot &&
            existing.first.generation == input->first.generation &&
            existing.second.owner == input->second.owner &&
            existing.second.kind == input->second.kind &&
            existing.second.slot == input->second.slot &&
            existing.second.generation == input->second.generation) {
            return poison(transaction, rt::Status::invalid_argument);
        }
    }
    auto copy = *input;
    copy.struct_size = RTFW_EXTENSION_RELATIONSHIP_V1_REQUIRED_SIZE;
    transaction.relationships[transaction.relationship_count++] = copy;
    return RTFW_STATUS_OK;
}

class ActiveCall {
public:
    ActiveCall(
        std::atomic<bool>& admission,
        std::atomic<std::uint32_t>& count) noexcept
        : admission_(admission), count_(count) {
        if (!admission_.load(std::memory_order_acquire)) {
            return;
        }
        count_.fetch_add(1, std::memory_order_acq_rel);
        active_ = admission_.load(std::memory_order_acquire);
        if (!active_) {
            count_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    ~ActiveCall() {
        if (active_) {
            count_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    std::atomic<bool>& admission_;
    std::atomic<std::uint32_t>& count_;
    bool active_ = false;
};

} // namespace

namespace rt::detail {

bool valid_extension_identifier(
    const char* value,
    std::size_t capacity) noexcept {
    if (!value || capacity == 0) {
        return false;
    }
    std::size_t length = 0;
    while (length < capacity && value[length] != '\0') {
        const unsigned char c = static_cast<unsigned char>(value[length]);
        const bool valid = (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == ':' || c == '/' || c == '@' ||
            c == '-';
        if (!valid) {
            return false;
        }
        ++length;
    }
    if (length == 0 || length == capacity) {
        return false;
    }
    for (std::size_t index = length + 1; index < capacity; ++index) {
        if (value[index] != '\0') {
            return false;
        }
    }
    return true;
}

Status status_from_extension(rtfw_status status) noexcept {
    return valid_c_status(status)
        ? static_cast<Status>(status)
        : Status::internal_error;
}

Status invoke_extension_entry(
    rtfw_extension_entry_fn_v1 entry,
    std::uint32_t owner,
    std::uint32_t generation,
    std::size_t phase_capacity,
    std::size_t backend_capacity,
    ExtensionRegistrationTransaction& transaction) noexcept {
    transaction = {};
    transaction.owner = owner;
    transaction.generation = generation;
    transaction.phase_limit = std::min<std::size_t>(
        phase_capacity, RTFW_EXTENSION_PHASE_CAPACITY);
    transaction.backend_limit = std::min<std::size_t>(
        backend_capacity, RTFW_EXTENSION_BACKEND_CAPACITY);
    transaction.descriptor.struct_size =
        RTFW_EXTENSION_DESCRIPTOR_V1_REQUIRED_SIZE;

    rtfw_extension_host_api_v1 host{};
    host.struct_size = RTFW_EXTENSION_HOST_API_V1_REQUIRED_SIZE;
    host.current_abi_version = RTFW_EXTENSION_ABI_VERSION;
    host.min_compatible_abi_version =
        RTFW_EXTENSION_ABI_MIN_COMPATIBLE_VERSION;
    host.context = &transaction;
    host.phase_capacity = transaction.phase_limit;
    host.backend_capacity = transaction.backend_limit;
    host.service_capacity = RTFW_EXTENSION_SERVICE_CAPACITY;
    host.resource_capacity = RTFW_EXTENSION_RESOURCE_CAPACITY;
    host.relationship_capacity = RTFW_EXTENSION_RELATIONSHIP_CAPACITY;
    host.stage_phase = &stage_phase;
    host.stage_backend = &stage_backend;
    host.stage_service = &stage_service;
    host.stage_resource = &stage_resource;
    host.stage_relationship = &stage_relationship;

    if (!entry) {
        return Status::invalid_argument;
    }
    rtfw_status result = RTFW_STATUS_INTERNAL_ERROR;
    try {
        result = entry(&host, &transaction.descriptor);
    } catch (...) {
        transaction.failure = Status::callback_failed;
        return transaction.failure;
    }
    if (transaction.failure != Status::ok) {
        return transaction.failure;
    }
    if (!valid_c_status(result) || result != RTFW_STATUS_OK) {
        return result == RTFW_STATUS_OK
            ? Status::internal_error
            : status_from_extension(result);
    }
    const auto& descriptor = transaction.descriptor;
    const auto lower = std::max(
        host.min_compatible_abi_version,
        descriptor.min_compatible_abi_version);
    const auto upper = std::min(
        host.current_abi_version,
        descriptor.current_abi_version);
    if (descriptor.struct_size < RTFW_EXTENSION_DESCRIPTOR_V1_REQUIRED_SIZE ||
        descriptor.current_abi_version == 0 ||
        descriptor.min_compatible_abi_version == 0 ||
        descriptor.min_compatible_abi_version >
            descriptor.current_abi_version ||
        descriptor.reserved0 != 0 ||
        !valid_extension_identifier(
            descriptor.name, RTFW_EXTENSION_IDENTIFIER_CAPACITY) ||
        !valid_extension_identifier(
            descriptor.version, RTFW_EXTENSION_IDENTIFIER_CAPACITY) ||
        !reserved_zero(descriptor.reserved) ||
        descriptor.phase_count != transaction.phase_count ||
        descriptor.backend_count != transaction.backend_count ||
        descriptor.service_count != transaction.service_count ||
        descriptor.resource_count != transaction.resource_count ||
        descriptor.relationship_count != transaction.relationship_count) {
        return Status::invalid_argument;
    }
    if (lower > upper || upper != RTFW_EXTENSION_ABI_VERSION) {
        return Status::incompatible_abi;
    }
    return Status::ok;
}

CallbackResult ExtensionPhaseOwner::invoke(
    void* opaque,
    const CallbackContext& context) noexcept {
    auto& self = *static_cast<ExtensionPhaseOwner*>(opaque);
    ActiveCall call(self.admission, self.active_calls);
    if (!call.active() || !self.callback) {
        return CallbackResult::error;
    }
    rtfw_callback_context c_context{};
    c_context.struct_size = sizeof(c_context);
    c_context.numerical_mode =
        context.numerics.mode() == NumericalMode::fused_multiply_add
        ? RTFW_NUMERICAL_FUSED_MULTIPLY_ADD
        : RTFW_NUMERICAL_PRECISE;
    c_context.degradation_level = context.degradation_level;
    c_context.frame_index = context.frame.frame_index;
    c_context.delta_ns = context.frame.delta.count();
    c_context.has_deadline = context.frame.deadline_ns ? 1u : 0u;
    c_context.deadline_ns = context.frame.deadline_ns.value_or(0);
    c_context.scratch = context.scratch.data();
    c_context.scratch_bytes = context.scratch.size();
    c_context.tasks =
        reinterpret_cast<const rtfw_task_context*>(&context.tasks);
    try {
        return self.callback(self.user_data, &c_context) == RTFW_CALLBACK_OK
            ? CallbackResult::ok
            : CallbackResult::error;
    } catch (...) {
        return CallbackResult::error;
    }
}

void ExtensionPhaseOwner::close() noexcept {
    admission.store(false, std::memory_order_release);
}

void ExtensionPhaseOwner::open() noexcept {
    admission.store(true, std::memory_order_release);
}

void ExtensionPhaseOwner::clear() noexcept {
    callback = nullptr;
    user_data = nullptr;
}

void ExtensionBackendOwner::initialize_from(
    const rtfw_device_backend_api& api) noexcept {
    borrowed = api;
    borrowed.struct_size = sizeof(borrowed);
    forwarding = {};
    forwarding.struct_size = sizeof(forwarding);
    forwarding.abi_version = RTFW_DEVICE_ABI_VERSION;
    forwarding.instance = this;
    forwarding.get_capabilities = &get_capabilities;
    forwarding.initialize = &initialize;
    forwarding.register_buffer = &register_buffer;
    forwarding.unregister_buffer = &unregister_buffer;
    forwarding.submit = &submit;
    forwarding.poll = &poll;
    forwarding.cancel = &cancel;
    forwarding.get_health = &get_health;
    forwarding.reset = &reset;
    forwarding.shutdown = &shutdown;
    admission.store(true, std::memory_order_release);
    released.store(true, std::memory_order_release);
}

void ExtensionBackendOwner::close() noexcept {
    admission.store(false, std::memory_order_release);
}

void ExtensionBackendOwner::open() noexcept {
    admission.store(true, std::memory_order_release);
}

void ExtensionBackendOwner::clear() noexcept {
    borrowed = {};
    forwarding = {};
}

#define RTFW_FORWARD_BACKEND_BODY(member, ...)                                  \
    auto& forward_self = *static_cast<ExtensionBackendOwner*>(opaque);          \
    forward_self.active_calls.fetch_add(1, std::memory_order_acq_rel);          \
    rtfw_device_status result = RTFW_DEVICE_STATUS_INTERNAL_ERROR;              \
    try {                                                                        \
        if (forward_self.borrowed.member) {                                      \
            result = forward_self.borrowed.member(                               \
                forward_self.borrowed.instance, __VA_ARGS__);                    \
        }                                                                        \
    } catch (...) {                                                              \
        result = RTFW_DEVICE_STATUS_INTERNAL_ERROR;                              \
    }                                                                            \
    forward_self.active_calls.fetch_sub(1, std::memory_order_acq_rel);           \
    return result

rtfw_device_status ExtensionBackendOwner::get_capabilities(
    void* opaque, rtfw_device_capabilities* capabilities) noexcept {
    RTFW_FORWARD_BACKEND_BODY(get_capabilities, capabilities);
}

rtfw_device_status ExtensionBackendOwner::initialize(
    void* opaque, const rtfw_device_init_config* config) noexcept {
    auto& self = *static_cast<ExtensionBackendOwner*>(opaque);
    if (!self.admission.load(std::memory_order_acquire)) {
        return RTFW_DEVICE_STATUS_INVALID_STATE;
    }
    self.released.store(false, std::memory_order_release);
    RTFW_FORWARD_BACKEND_BODY(initialize, config);
}

rtfw_device_status ExtensionBackendOwner::register_buffer(
    void* opaque,
    const rtfw_device_buffer_registration* registration,
    uint64_t* token) noexcept {
    auto& self = *static_cast<ExtensionBackendOwner*>(opaque);
    if (!self.admission.load(std::memory_order_acquire)) {
        return RTFW_DEVICE_STATUS_INVALID_STATE;
    }
    RTFW_FORWARD_BACKEND_BODY(register_buffer, registration, token);
}

rtfw_device_status ExtensionBackendOwner::unregister_buffer(
    void* opaque, uint64_t token) noexcept {
    RTFW_FORWARD_BACKEND_BODY(unregister_buffer, token);
}

rtfw_device_status ExtensionBackendOwner::submit(
    void* opaque, const rtfw_device_submission* submission) noexcept {
    auto& self = *static_cast<ExtensionBackendOwner*>(opaque);
    if (!self.admission.load(std::memory_order_acquire)) {
        return RTFW_DEVICE_STATUS_INVALID_STATE;
    }
    RTFW_FORWARD_BACKEND_BODY(submit, submission);
}

rtfw_device_status ExtensionBackendOwner::poll(
    void* opaque,
    rtfw_device_completion* completions,
    uint64_t capacity,
    uint64_t* count) noexcept {
    RTFW_FORWARD_BACKEND_BODY(poll, completions, capacity, count);
}

rtfw_device_status ExtensionBackendOwner::cancel(
    void* opaque, uint64_t submission) noexcept {
    RTFW_FORWARD_BACKEND_BODY(cancel, submission);
}

rtfw_device_status ExtensionBackendOwner::get_health(
    void* opaque, rtfw_device_health* health) noexcept {
    RTFW_FORWARD_BACKEND_BODY(get_health, health);
}

rtfw_device_status ExtensionBackendOwner::reset(void* opaque) noexcept {
    auto& self = *static_cast<ExtensionBackendOwner*>(opaque);
    self.active_calls.fetch_add(1, std::memory_order_acq_rel);
    rtfw_device_status result = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    try {
        if (self.borrowed.reset) {
            result = self.borrowed.reset(self.borrowed.instance);
        }
    } catch (...) {
        result = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    }
    self.active_calls.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

rtfw_device_status ExtensionBackendOwner::shutdown(void* opaque) noexcept {
    auto& self = *static_cast<ExtensionBackendOwner*>(opaque);
    self.active_calls.fetch_add(1, std::memory_order_acq_rel);
    rtfw_device_status result = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    try {
        if (self.borrowed.shutdown) {
            result = self.borrowed.shutdown(self.borrowed.instance);
        }
    } catch (...) {
        result = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    }
    if (result == RTFW_DEVICE_STATUS_OK ||
        result == RTFW_DEVICE_STATUS_INVALID_STATE) {
        self.released.store(true, std::memory_order_release);
    }
    self.active_calls.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

#undef RTFW_FORWARD_BACKEND_BODY

void ExtensionServiceOwner::initialize_from(
    const rtfw_extension_service_api_v1& source) noexcept {
    api = source;
    api.struct_size = RTFW_EXTENSION_SERVICE_API_V1_REQUIRED_SIZE;
    admission.store(true, std::memory_order_release);
    initialized.store(false, std::memory_order_release);
    stop_requested.store(false, std::memory_order_release);
    quiesced.store(false, std::memory_order_release);
    released.store(true, std::memory_order_release);
}

void ExtensionServiceOwner::open() noexcept {
    admission.store(true, std::memory_order_release);
}
Status ExtensionServiceOwner::call_initialize() noexcept {
    if (initialized.load(std::memory_order_acquire)) {
        return Status::ok;
    }
    stop_requested.store(false, std::memory_order_release);
    quiesced.store(false, std::memory_order_release);
    released.store(false, std::memory_order_release);
    active_calls.fetch_add(1, std::memory_order_acq_rel);
    rtfw_status result = RTFW_STATUS_INTERNAL_ERROR;
    try {
        result = api.initialize(api.instance);
    } catch (...) {
        result = RTFW_STATUS_CALLBACK_FAILED;
    }
    active_calls.fetch_sub(1, std::memory_order_acq_rel);
    const auto status = status_from_extension(result);
    if (status == Status::ok) {
        initialized.store(true, std::memory_order_release);
    }
    return status;
}

Status ExtensionServiceOwner::call_request_stop() noexcept {
    if (released.load(std::memory_order_acquire)) {
        return Status::ok;
    }
    if (stop_requested.load(std::memory_order_acquire)) {
        return Status::ok;
    }
    active_calls.fetch_add(1, std::memory_order_acq_rel);
    rtfw_status result = RTFW_STATUS_INTERNAL_ERROR;
    try {
        result = api.request_stop(api.instance);
    } catch (...) {
        result = RTFW_STATUS_CALLBACK_FAILED;
    }
    active_calls.fetch_sub(1, std::memory_order_acq_rel);
    const auto status = status_from_extension(result);
    if (status == Status::ok) {
        stop_requested.store(true, std::memory_order_release);
    }
    return status;
}

Status ExtensionServiceOwner::call_quiesce() noexcept {
    if (quiesced.load(std::memory_order_acquire) ||
        released.load(std::memory_order_acquire)) {
        return Status::ok;
    }
    active_calls.fetch_add(1, std::memory_order_acq_rel);
    rtfw_status result = RTFW_STATUS_INTERNAL_ERROR;
    try {
        result = api.quiesce(api.instance);
    } catch (...) {
        result = RTFW_STATUS_CALLBACK_FAILED;
    }
    active_calls.fetch_sub(1, std::memory_order_acq_rel);
    const auto status = status_from_extension(result);
    if (status == Status::ok) {
        quiesced.store(true, std::memory_order_release);
    }
    return status;
}

Status ExtensionServiceOwner::call_shutdown() noexcept {
    if (released.load(std::memory_order_acquire)) {
        return Status::ok;
    }
    active_calls.fetch_add(1, std::memory_order_acq_rel);
    rtfw_status result = RTFW_STATUS_INTERNAL_ERROR;
    try {
        result = api.shutdown(api.instance);
    } catch (...) {
        result = RTFW_STATUS_CALLBACK_FAILED;
    }
    active_calls.fetch_sub(1, std::memory_order_acq_rel);
    const auto status = status_from_extension(result);
    if (status == Status::ok) {
        released.store(true, std::memory_order_release);
        initialized.store(false, std::memory_order_release);
    }
    return status;
}

Status ExtensionServiceOwner::call_status(
    rtfw_extension_service_status_v1& output) noexcept {
    const auto initial = output;
    ActiveCall call(admission, active_calls);
    if (!call.active() || !api.status) {
        return Status::invalid_state;
    }
    rtfw_extension_service_status_v1 candidate{};
    candidate.struct_size = RTFW_EXTENSION_SERVICE_STATUS_V1_REQUIRED_SIZE;
    rtfw_status result = RTFW_STATUS_INTERNAL_ERROR;
    try {
        result = api.status(api.instance, &candidate);
    } catch (...) {
        result = RTFW_STATUS_CALLBACK_FAILED;
    }
    const auto status = status_from_extension(result);
    if (status != Status::ok ||
        candidate.struct_size < RTFW_EXTENSION_SERVICE_STATUS_V1_REQUIRED_SIZE ||
        candidate.abi_version != RTFW_EXTENSION_ABI_VERSION ||
        candidate.healthy > 1 || candidate.reserved0 != 0 ||
        candidate.reserved1 != 0 || !valid_c_status(candidate.status) ||
        !reserved_zero(candidate.reserved)) {
        output = initial;
        return status == Status::ok ? Status::invalid_argument : status;
    }
    output = candidate;
    return Status::ok;
}

void ExtensionServiceOwner::close() noexcept {
    admission.store(false, std::memory_order_release);
}

void ExtensionServiceOwner::clear() noexcept {
    api = {};
}

void ExtensionRegistrationRecord::close_admission() noexcept {
    for (std::size_t index = 0; index < phase_count; ++index) {
        phases[index].close();
    }
    for (std::size_t index = 0; index < backend_count; ++index) {
        backends[index].close();
    }
    for (std::size_t index = 0; index < service_count; ++index) {
        services[index].close();
    }
}

void ExtensionRegistrationRecord::open_admission() noexcept {
    for (std::size_t index = 0; index < phase_count; ++index) {
        phases[index].open();
    }
    for (std::size_t index = 0; index < backend_count; ++index) {
        backends[index].open();
    }
    for (std::size_t index = 0; index < service_count; ++index) {
        services[index].open();
    }
}

bool ExtensionRegistrationRecord::callbacks_quiescent() const noexcept {
    for (std::size_t index = 0; index < phase_count; ++index) {
        if (phases[index].active_calls.load(std::memory_order_acquire) != 0) {
            return false;
        }
    }
    for (std::size_t index = 0; index < backend_count; ++index) {
        if (backends[index].active_calls.load(std::memory_order_acquire) != 0) {
            return false;
        }
    }
    for (std::size_t index = 0; index < service_count; ++index) {
        if (services[index].active_calls.load(std::memory_order_acquire) != 0) {
            return false;
        }
    }
    return true;
}

bool ExtensionRegistrationRecord::backends_released() const noexcept {
    for (std::size_t index = 0; index < backend_count; ++index) {
        if (!backends[index].released.load(std::memory_order_acquire)) {
            return false;
        }
    }
    return true;
}

bool ExtensionRegistrationRecord::services_released() const noexcept {
    for (std::size_t index = 0; index < service_count; ++index) {
        if (!services[index].released.load(std::memory_order_acquire)) {
            return false;
        }
    }
    return true;
}

void ExtensionRegistrationRecord::clear_borrowed() noexcept {
    for (std::size_t index = 0; index < phase_count; ++index) {
        phases[index].clear();
        phase_descriptors[index].callback = nullptr;
        phase_descriptors[index].user_data = nullptr;
    }
    for (std::size_t index = 0; index < backend_count; ++index) {
        backends[index].clear();
        backend_descriptors[index].api = {};
    }
    for (std::size_t index = 0; index < service_count; ++index) {
        services[index].clear();
        service_descriptors[index].api = {};
    }
}

} // namespace rt::detail
