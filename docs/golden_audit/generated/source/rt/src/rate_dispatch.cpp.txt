#include "rate_dispatch.hpp"

#include "command_batch.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <new>
#include <tuple>

namespace {

bool checked_add(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    output = left + right;
    return true;
}

bool checked_multiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {
    if (left != 0 &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    output = left * right;
    return true;
}

bool valid_action(rt::RateLateAction action) noexcept {
    return action == rt::RateLateAction::skip ||
        action == rt::RateLateAction::bounded_catch_up ||
        action == rt::RateLateAction::hold ||
        action == rt::RateLateAction::degrade ||
        action == rt::RateLateAction::fail;
}

bool ceil_div(
    std::uint64_t value,
    std::uint64_t divisor,
    std::uint64_t& output) noexcept {
    if (divisor == 0) {
        return false;
    }
    output = value / divisor;
    if (value % divisor != 0) {
        if (output == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        ++output;
    }
    return true;
}

} // namespace

namespace rt::detail {

Status compile_device_rate_plan(
    std::uint32_t graph_owner,
    std::uint32_t runtime_outstanding_capacity,
    std::uint32_t runtime_completion_batch_capacity,
    const CompiledRatePlan& rate_plan,
    std::span<const DeviceRateBindingSpec> bindings,
    std::span<const DeviceRateBackendSource> backends,
    std::span<const DeviceRateBufferSource> buffers,
    std::span<const DeviceRateTimelineSource> timelines,
    std::span<const DeviceRatePhaseSource> phases,
    CompiledDeviceRatePlan& output,
    DeviceRateDiagnostic& diagnostic) noexcept {
    diagnostic = {};
    if (bindings.empty()) {
        output = {};
        return Status::ok;
    }
    if (rate_plan.supercycle_ns == 0 || runtime_outstanding_capacity == 0 ||
        runtime_completion_batch_capacity == 0 || phases.size() != bindings.size()) {
        diagnostic = {Status::invalid_config,
                      "device-rate admission requires a finite plan and positive Runtime capacities"};
        return diagnostic.status;
    }

    const auto fail = [&](Status status, const char* message,
                          PhaseHandle phase = {},
                          std::size_t reference = invalid_reference_release_index) {
        diagnostic = {status, message, reference, phase};
        return status;
    };
    const auto role_matches = [](DeviceRatePayloadRole role,
                                 std::uint32_t access) {
        return (role == DeviceRatePayloadRole::input &&
                access == RTFW_DEVICE_ACCESS_READ) ||
            (role == DeviceRatePayloadRole::output &&
             access == RTFW_DEVICE_ACCESS_WRITE) ||
            (role == DeviceRatePayloadRole::input_output &&
             access == RTFW_DEVICE_ACCESS_READ_WRITE);
    };

    try {
        CompiledDeviceRatePlan candidate;
        candidate.phases.reserve(bindings.size());
        candidate.admission_phases.reserve(bindings.size());
        std::vector<std::size_t> plan_phase_by_registration(
            rate_plan.bindings.size(), invalid_reference_release_index);
        std::vector<std::size_t> backend_source_by_index(
            backends.size(), invalid_reference_release_index);
        std::vector<std::size_t> buffer_source_by_index(
            buffers.size(), invalid_reference_release_index);
        std::vector<std::size_t> timeline_source_by_index(
            timelines.size(), invalid_reference_release_index);
        std::vector<std::size_t> phase_source_by_index(
            rate_plan.bindings.size(), invalid_reference_release_index);
        std::vector<bool> backend_used(backends.size(), false);
        std::vector<std::size_t> binding_order(bindings.size());
        std::vector<std::size_t> compiled_index_by_binding(
            bindings.size(), invalid_reference_release_index);
        std::vector<std::size_t> compiled_index_by_phase(
            rate_plan.bindings.size(), invalid_reference_release_index);
        for (const auto& compiled_binding : rate_plan.bindings) {
            if (compiled_binding.phase.valid() &&
                compiled_binding.phase.owner() == graph_owner &&
                compiled_binding.phase.index() <
                    compiled_index_by_phase.size()) {
                compiled_index_by_phase[compiled_binding.phase.index()] =
                    compiled_binding.compiled_phase_index;
            }
        }
        for (std::size_t index = 0; index < binding_order.size(); ++index) {
            binding_order[index] = index;
            if (bindings[index].phase.valid() &&
                bindings[index].phase.owner() == graph_owner &&
                bindings[index].phase.index() < compiled_index_by_phase.size()) {
                compiled_index_by_binding[index] =
                    compiled_index_by_phase[bindings[index].phase.index()];
            }
        }
        std::sort(binding_order.begin(), binding_order.end(),
                  [&](std::size_t left, std::size_t right) {
            return std::tuple{compiled_index_by_binding[left], left} <
                std::tuple{compiled_index_by_binding[right], right};
        });

        for (std::size_t index = 0; index < backends.size(); ++index) {
            if (!backends[index].backend.valid() ||
                backends[index].backend.owner() != graph_owner ||
                backends[index].backend.index() >= backends.size() ||
                backend_source_by_index[backends[index].backend.index()] !=
                    invalid_reference_release_index) {
                return fail(Status::invalid_handle,
                            "device-rate backend inventory contains an invalid, foreign, or duplicate handle");
            }
            backend_source_by_index[backends[index].backend.index()] = index;
        }
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            if (!buffers[index].buffer.valid() ||
                buffers[index].buffer.owner() != graph_owner ||
                buffers[index].buffer.index() >= buffer_source_by_index.size() ||
                buffer_source_by_index[buffers[index].buffer.index()] !=
                    invalid_reference_release_index) {
                return fail(Status::invalid_handle,
                            "device-rate buffer inventory contains an invalid, foreign, or duplicate handle");
            }
            buffer_source_by_index[buffers[index].buffer.index()] = index;
        }
        for (std::size_t index = 0; index < timelines.size(); ++index) {
            if (!timelines[index].timeline.valid() ||
                timelines[index].timeline.owner() != graph_owner ||
                timelines[index].timeline.index() >=
                    timeline_source_by_index.size() ||
                timeline_source_by_index[timelines[index].timeline.index()] !=
                    invalid_reference_release_index) {
                return fail(Status::invalid_handle,
                            "device-rate timeline inventory contains an invalid, foreign, or duplicate handle");
            }
            timeline_source_by_index[timelines[index].timeline.index()] = index;
        }
        for (std::size_t index = 0; index < phases.size(); ++index) {
            if (!phases[index].phase.valid() ||
                phases[index].phase.owner() != graph_owner ||
                phases[index].phase.index() >= phase_source_by_index.size() ||
                phase_source_by_index[phases[index].phase.index()] !=
                    invalid_reference_release_index) {
                return fail(Status::invalid_handle,
                            "device-rate phase inventory contains an invalid, foreign, or duplicate handle");
            }
            phase_source_by_index[phases[index].phase.index()] = index;
        }

        const auto find_buffer = [&](DeviceBufferHandle handle)
            -> const DeviceRateBufferSource* {
            if (!handle.valid() || handle.owner() != graph_owner ||
                handle.index() >= buffer_source_by_index.size() ||
                buffer_source_by_index[handle.index()] ==
                    invalid_reference_release_index) {
                return nullptr;
            }
            return &buffers[buffer_source_by_index[handle.index()]];
        };
        const auto find_timeline = [&](DeviceTimelineHandle handle)
            -> const DeviceRateTimelineSource* {
            if (!handle.valid() || handle.owner() != graph_owner ||
                handle.index() >= timeline_source_by_index.size() ||
                timeline_source_by_index[handle.index()] ==
                    invalid_reference_release_index) {
                return nullptr;
            }
            return &timelines[timeline_source_by_index[handle.index()]];
        };

        for (const auto binding_index : binding_order) {
            const auto& binding = bindings[binding_index];
            const auto phase_source_index =
                binding.phase.valid() &&
                    binding.phase.owner() == graph_owner &&
                    binding.phase.index() < phase_source_by_index.size()
                ? phase_source_by_index[binding.phase.index()]
                : invalid_reference_release_index;
            const auto* phase_source =
                phase_source_index < phases.size()
                ? &phases[phase_source_index]
                : nullptr;
            const auto compiled_index =
                compiled_index_by_binding[binding_index];
            const auto* compiled_binding =
                compiled_index < rate_plan.bindings.size()
                ? &rate_plan.bindings[compiled_index]
                : nullptr;
            if (!binding.phase.valid() ||
                binding.phase.owner() != graph_owner ||
                binding.phase.index() >= rate_plan.bindings.size() ||
                !binding.domain.valid() ||
                binding.domain.owner() != graph_owner ||
                binding.domain.index() >= rate_plan.domains.size() ||
                binding.completion_budget_ns == 0 ||
                binding.maximum_in_flight == 0 ||
                !valid_simulation_policy(binding.simulation) || !phase_source ||
                !phase_source->declaration || !compiled_binding ||
                compiled_binding->domain != binding.domain ||
                compiled_binding->phase_kind != RatePhaseKind::device ||
                plan_phase_by_registration[binding.phase.index()] !=
                    invalid_reference_release_index) {
              return fail(Status::invalid_config,
                          "device-rate binding has invalid ownership, policy, "
                          "or rate identity",
                          binding.phase);
            }
            const auto backend_index = phase_source->backend.index();
            if (!phase_source->backend.valid() ||
                phase_source->backend.owner() != graph_owner ||
                backend_index >= backend_source_by_index.size() ||
                backend_source_by_index[backend_index] ==
                    invalid_reference_release_index) {
                return fail(Status::invalid_handle,
                            "device-rate phase references an invalid or foreign backend",
                            binding.phase);
            }
            const auto& backend =
                backends[backend_source_by_index[backend_index]];
            if (binding.simulation && !backend.deterministic_mock) {
              return fail(
                  Status::invalid_config,
                  "simulation timing requires a deterministic mock backend",
                  binding.phase);
            }
            const auto& declaration = *phase_source->declaration;
            if (!backend.completion_timestamp_domain_valid ||
                backend.completion_timestamp_domain_identity == 0 ||
                !validate_batch_declaration(declaration) ||
                declaration.command_count >
                    backend.capabilities.max_commands_per_batch ||
                declaration.wait_count > backend.capabilities.max_wait_points ||
                declaration.signal_count >
                    backend.capabilities.max_signal_points ||
                binding.maximum_in_flight >
                    backend.capabilities.max_in_flight_batches) {
                return fail(Status::invalid_config,
                            "device-rate phase exceeds command, timeline, timestamp, or in-flight capability",
                            binding.phase);
            }

            CompiledDeviceRatePhase compiled;
            compiled.phase = binding.phase;
            compiled.domain = binding.domain;
            compiled.backend = phase_source->backend;
            compiled.compiled_phase_index = compiled_binding->compiled_phase_index;
            compiled.completion_budget_ns = binding.completion_budget_ns;
            compiled.simulation = binding.simulation;
            compiled.maximum_in_flight = binding.maximum_in_flight;
            compiled.command_count = declaration.command_count;
            compiled.wait_count = declaration.wait_count;
            compiled.signal_count = declaration.signal_count;
            compiled.first_command_index = candidate.commands.size();
            compiled.first_payload_reference_index =
                candidate.payload_references.size();
            compiled.first_timeline_reference_index =
                candidate.timeline_references.size();
            compiled.completion_timestamp_domain_identity =
                backend.completion_timestamp_domain_identity;

            std::size_t role_index = 0;
            const auto add_reference = [&](const HalV2BufferReference& reference,
                                           std::uint32_t command_index,
                                           std::uint32_t command_reference_index,
                                           DeviceRateReferenceKind kind) -> Status {
                if (role_index >= binding.payload_roles.size()) {
                    return Status::invalid_config;
                }
                const DeviceBufferHandle buffer{reference.buffer_token};
                const auto* source = find_buffer(buffer);
                const auto role = binding.payload_roles[role_index++];
                const auto required_access =
                    reference.access == RTFW_DEVICE_ACCESS_READ
                    ? RTFW_DEVICE_BUFFER_DEVICE_READ
                    : reference.access == RTFW_DEVICE_ACCESS_WRITE
                    ? RTFW_DEVICE_BUFFER_DEVICE_WRITE
                    : reference.access == RTFW_DEVICE_ACCESS_READ_WRITE
                    ? RTFW_DEVICE_BUFFER_DEVICE_READ |
                          RTFW_DEVICE_BUFFER_DEVICE_WRITE
                    : 0u;
                if (!source || source->backend != phase_source->backend ||
                    required_access == 0 ||
                    (required_access & ~source->access) != 0 ||
                    !role_matches(role, reference.access) ||
                    reference.bytes == 0 || reference.offset > source->bytes ||
                    reference.bytes > source->bytes - reference.offset ||
                    source->byte_granularity == 0 ||
                    source->offset_granularity == 0 ||
                    reference.bytes % source->byte_granularity != 0 ||
                    reference.offset % source->offset_granularity != 0) {
                    return Status::invalid_config;
                }
                candidate.payload_references.push_back({
                    binding.phase,
                    phase_source->backend,
                    buffer,
                    command_index,
                    command_reference_index,
                    kind,
                    role,
                    reference.access,
                    reference.offset,
                    reference.bytes,
                });
                return Status::ok;
            };

            for (std::uint32_t command_index = 0;
                 command_index < declaration.command_count; ++command_index) {
                const auto& command = declaration.commands[command_index];
                CompiledDeviceRateCommand compiled_command;
                compiled_command.phase = binding.phase;
                compiled_command.command_index = command_index;
                compiled_command.kind =
                    static_cast<HalV2CommandKind>(command.kind);
                compiled_command.operation =
                    static_cast<HalV2MemoryOperation>(command.operation);
                compiled_command.opcode = command.opcode;
                compiled_command.flags = command.flags;
                compiled_command.first_payload_reference_index =
                    candidate.payload_references.size();
                Status reference_status = Status::ok;
                if (compiled_command.kind == HalV2CommandKind::dispatch) {
                    for (std::uint32_t reference_index = 0;
                         reference_index < command.buffer_count;
                         ++reference_index) {
                        reference_status = add_reference(
                            command.buffers[reference_index], command_index,
                            reference_index,
                            DeviceRateReferenceKind::dispatch_buffer);
                        if (reference_status != Status::ok) {
                            break;
                        }
                    }
                } else if (compiled_command.kind == HalV2CommandKind::copy) {
                    reference_status = add_reference(
                        command.source, command_index, 0,
                        DeviceRateReferenceKind::copy_source);
                    if (reference_status == Status::ok) {
                        reference_status = add_reference(
                            command.destination, command_index, 1,
                            DeviceRateReferenceKind::copy_destination);
                    }
                } else {
                    reference_status = add_reference(
                        command.target, command_index, 0,
                        DeviceRateReferenceKind::synchronization_target);
                }
                if (reference_status != Status::ok) {
                    return fail(Status::invalid_config,
                                "device-rate payload roles do not exactly match the copied declaration",
                                binding.phase);
                }
                compiled_command.payload_reference_count =
                    candidate.payload_references.size() -
                    compiled_command.first_payload_reference_index;
                candidate.commands.push_back(compiled_command);
            }
            if (role_index != binding.payload_roles.size()) {
                return fail(Status::invalid_config,
                            "device-rate payload-role inventory has extra entries",
                            binding.phase);
            }
            compiled.payload_reference_count =
                candidate.payload_references.size() -
                compiled.first_payload_reference_index;

            for (std::uint32_t index = 0; index < declaration.wait_count;
                 ++index) {
                const DeviceTimelineHandle timeline{
                    declaration.waits[index].timeline_handle};
                const auto* source = find_timeline(timeline);
                if (!source || source->backend != phase_source->backend ||
                    std::any_of(
                        declaration.waits.begin(),
                        declaration.waits.begin() + index,
                        [&](const auto& prior) {
                            return prior.timeline_handle == timeline.value;
                        })) {
                    return fail(Status::invalid_config,
                                "device-rate wait timeline is foreign or duplicated",
                                binding.phase);
                }
                candidate.timeline_references.push_back({
                    binding.phase, phase_source->backend, timeline,
                    DeviceRateTimelineRole::wait, index});
            }
            for (std::uint32_t index = 0; index < declaration.signal_count;
                 ++index) {
                const DeviceTimelineHandle timeline{
                    declaration.signals[index].timeline_handle};
                const auto* source = find_timeline(timeline);
                if (!source || source->backend != phase_source->backend ||
                    std::any_of(
                        declaration.signals.begin(),
                        declaration.signals.begin() + index,
                        [&](const auto& prior) {
                            return prior.timeline_handle == timeline.value;
                        })) {
                    return fail(Status::invalid_config,
                                "device-rate signal timeline is foreign or duplicated",
                                binding.phase);
                }
                candidate.timeline_references.push_back({
                    binding.phase, phase_source->backend, timeline,
                    DeviceRateTimelineRole::signal, index});
            }
            compiled.timeline_reference_count =
                candidate.timeline_references.size() -
                compiled.first_timeline_reference_index;
            plan_phase_by_registration[binding.phase.index()] =
                candidate.phases.size();
            backend_used[backend_source_by_index[backend_index]] = true;
            candidate.phases.push_back(compiled);
            candidate.admission_phases.push_back({
                binding.phase, phase_source->backend,
                binding.completion_budget_ns, binding.maximum_in_flight, 0});
        }

        std::vector<std::size_t> signal_owner(timelines.size(),
                                               invalid_reference_release_index);
        const auto timeline_ordinal = [&](DeviceTimelineHandle handle) {
            if (!handle.valid() || handle.owner() != graph_owner ||
                handle.index() >= timeline_source_by_index.size()) {
                return invalid_reference_release_index;
            }
            return timeline_source_by_index[handle.index()];
        };
        for (std::size_t phase_index = 0;
             phase_index < candidate.phases.size(); ++phase_index) {
            const auto& phase = candidate.phases[phase_index];
            for (std::size_t index = phase.first_timeline_reference_index;
                 index < phase.first_timeline_reference_index +
                     phase.timeline_reference_count; ++index) {
                const auto& reference = candidate.timeline_references[index];
                if (reference.role != DeviceRateTimelineRole::signal) {
                    continue;
                }
                const auto ordinal = timeline_ordinal(reference.timeline);
                if (ordinal == invalid_reference_release_index ||
                    signal_owner[ordinal] != invalid_reference_release_index) {
                    return fail(Status::invalid_config,
                                "device-rate timeline progress has multiple signal owners",
                                phase.phase);
                }
                signal_owner[ordinal] = phase_index;
            }
        }
        std::vector<std::vector<std::size_t>> wait_graph(candidate.phases.size());
        std::vector<std::size_t> indegree(candidate.phases.size(), 0);
        for (std::size_t phase_index = 0;
             phase_index < candidate.phases.size(); ++phase_index) {
            const auto& phase = candidate.phases[phase_index];
            for (std::size_t index = phase.first_timeline_reference_index;
                 index < phase.first_timeline_reference_index +
                     phase.timeline_reference_count; ++index) {
                const auto& reference = candidate.timeline_references[index];
                if (reference.role != DeviceRateTimelineRole::wait) {
                    continue;
                }
                const auto ordinal = timeline_ordinal(reference.timeline);
                const auto owner = ordinal == invalid_reference_release_index
                    ? invalid_reference_release_index
                    : signal_owner[ordinal];
                if (owner == invalid_reference_release_index) {
                    if (ordinal == invalid_reference_release_index ||
                        timelines[ordinal].initial_value == 0) {
                        return fail(Status::invalid_config,
                                    "device-rate wait cannot be proven available",
                                    phase.phase);
                    }
                    continue;
                }
                if (owner == phase_index) {
                    return fail(Status::invalid_config,
                                "device-rate wait graph contains a cycle",
                                phase.phase);
                }
                if (std::find(wait_graph[owner].begin(), wait_graph[owner].end(),
                              phase_index) == wait_graph[owner].end()) {
                    wait_graph[owner].push_back(phase_index);
                    ++indegree[phase_index];
                }
            }
        }
        std::vector<std::size_t> ready;
        for (std::size_t index = 0; index < indegree.size(); ++index) {
            if (indegree[index] == 0) {
                ready.push_back(index);
            }
        }
        std::size_t visited = 0;
        while (!ready.empty()) {
            const auto current = ready.back();
            ready.pop_back();
            ++visited;
            for (const auto next : wait_graph[current]) {
                if (--indegree[next] == 0) {
                    ready.push_back(next);
                }
            }
        }
        if (visited != candidate.phases.size()) {
            return fail(Status::invalid_config,
                        "device-rate wait graph contains a cycle");
        }

        for (std::size_t index = 0; index < backends.size(); ++index) {
            if (!backend_used[index]) {
                continue;
            }
            candidate.admission_backends.push_back({
                backends[index].backend,
                backends[index].capabilities.max_in_flight_batches,
                std::min<std::uint32_t>(
                    runtime_completion_batch_capacity,
                    backends[index].capabilities.completion_batch_capacity),
                0,
                0,
            });
        }
        std::vector<std::size_t> admission_backend_by_source(
            backends.size(), invalid_reference_release_index);
        for (std::size_t index = 0;
             index < candidate.admission_backends.size(); ++index) {
            const auto backend = candidate.admission_backends[index].backend;
            admission_backend_by_source[
                backend_source_by_index[backend.index()]] = index;
        }

        struct Event {
            std::uint64_t time = 0;
            bool release = false;
            std::size_t interval = 0;
        };
        std::vector<Event> events;
        std::vector<std::pair<std::uint64_t, std::size_t>> completions;
        events.reserve(rate_plan.releases.size() * 2);
        completions.reserve(rate_plan.releases.size());
        const auto supercycle = rate_plan.supercycle_ns;
        for (std::size_t reference_index = 0;
             reference_index < rate_plan.releases.size(); ++reference_index) {
            const auto& release = rate_plan.releases[reference_index];
            if (release.phase_kind != RatePhaseKind::device) {
                continue;
            }
            if (release.phase.index() >= plan_phase_by_registration.size() ||
                plan_phase_by_registration[release.phase.index()] ==
                    invalid_reference_release_index) {
                return fail(Status::invalid_config,
                            "active device reference has no admitted command-batch policy",
                            release.phase, reference_index);
            }
            const auto phase_index =
                plan_phase_by_registration[release.phase.index()];
            const auto& phase = candidate.phases[phase_index];
            std::uint64_t budget_deadline = 0;
            if (!checked_add(release.release_time_ns,
                             phase.completion_budget_ns, budget_deadline)) {
                return fail(Status::capacity_exceeded,
                            "device-rate completion-budget arithmetic overflowed",
                            release.phase, reference_index);
            }
            const auto completion =
                std::min(release.deadline_time_ns, budget_deadline);
            if (completion <= release.release_time_ns ||
                completion - release.release_time_ns > supercycle) {
                return fail(Status::invalid_config,
                            "device-rate completion cannot drain within one repeating horizon",
                            release.phase, reference_index);
            }
            const auto interval_index = candidate.admission_intervals.size();
            const auto carry = completion > supercycle;
            const auto phase_limit = phase.maximum_in_flight;
            const auto backend_source =
                backend_source_by_index[phase.backend.index()];
            const auto& backend_report = candidate.admission_backends[
                admission_backend_by_source[backend_source]];
            const auto backend_limit = backend_report.maximum_in_flight;
            auto constraint = DeviceRateAdmissionConstraint::phase_in_flight;
            auto constraint_limit = phase_limit;
            if (backend_limit < constraint_limit) {
                constraint = DeviceRateAdmissionConstraint::backend_in_flight;
                constraint_limit = backend_limit;
            }
            if (runtime_outstanding_capacity < constraint_limit) {
                constraint = DeviceRateAdmissionConstraint::runtime_outstanding;
                constraint_limit = runtime_outstanding_capacity;
            }
            candidate.admission_intervals.push_back({
                reference_index, release.phase, release.domain, phase.backend,
                release.release_time_ns, completion, release.substep_ordinal,
                release.optional, carry, constraint, constraint_limit, 0});
            if (release.optional) {
                continue;
            }
            events.push_back({release.release_time_ns, true, interval_index});
            if (carry) {
                events.push_back({completion - supercycle, false,
                                  interval_index});
            } else {
                events.push_back({completion, false, interval_index});
            }
            completions.push_back({completion % supercycle, interval_index});
        }

        std::sort(events.begin(), events.end(), [](const auto& left,
                                                   const auto& right) {
            return std::tie(left.time, left.release, left.interval) <
                std::tie(right.time, right.release, right.interval);
        });
        std::vector<std::uint32_t> phase_demand(candidate.phases.size(), 0);
        std::vector<std::uint32_t> backend_demand(backends.size(), 0);
        std::uint32_t global_demand = 0;
        const auto phase_index_for = [&](PhaseHandle phase) {
            return plan_phase_by_registration[phase.index()];
        };
        const auto backend_index_for = [&](DeviceBackendHandle backend) {
            return backend_source_by_index[backend.index()];
        };
        for (std::size_t interval_index = 0;
             interval_index < candidate.admission_intervals.size();
             ++interval_index) {
            const auto& interval = candidate.admission_intervals[interval_index];
            if (interval.optional || !interval.carries_across_supercycle) {
                continue;
            }
            const auto phase_index = phase_index_for(interval.phase);
            const auto backend_index = backend_index_for(interval.backend);
            ++phase_demand[phase_index];
            ++backend_demand[backend_index];
            ++global_demand;
            if (phase_demand[phase_index] >
                    candidate.phases[phase_index].maximum_in_flight ||
                backend_demand[backend_index] >
                    backends[backend_index].capabilities.max_in_flight_batches ||
                global_demand > runtime_outstanding_capacity) {
                return fail(Status::invalid_config,
                            "device-rate cyclic carry exceeds an in-flight capacity",
                            interval.phase, interval.reference_index);
            }
        }
        const auto initial_phase_demand = phase_demand;
        const auto initial_backend_demand = backend_demand;
        const auto initial_global_demand = global_demand;
        for (std::size_t index = 0; index < phase_demand.size(); ++index) {
            candidate.admission_phases[index].peak_in_flight =
                phase_demand[index];
        }
        for (auto& report : candidate.admission_backends) {
            report.peak_in_flight =
                backend_demand[backend_index_for(report.backend)];
        }
        candidate.report.peak_global_in_flight = global_demand;
        for (std::size_t event_index = 0; event_index < events.size();) {
            const auto time = events[event_index].time;
            std::size_t cursor = event_index;
            while (cursor < events.size() && events[cursor].time == time &&
                   !events[cursor].release) {
                const auto& interval =
                    candidate.admission_intervals[events[cursor].interval];
                --phase_demand[phase_index_for(interval.phase)];
                --backend_demand[backend_index_for(interval.backend)];
                --global_demand;
                ++cursor;
            }
            while (cursor < events.size() && events[cursor].time == time &&
                   events[cursor].release) {
                auto& interval =
                    candidate.admission_intervals[events[cursor].interval];
                const auto phase_index = phase_index_for(interval.phase);
                const auto backend_index = backend_index_for(interval.backend);
                ++phase_demand[phase_index];
                ++backend_demand[backend_index];
                ++global_demand;
                interval.demand_at_release = phase_demand[phase_index];
                auto& phase_report = candidate.admission_phases[phase_index];
                phase_report.peak_in_flight = std::max(
                    phase_report.peak_in_flight, phase_demand[phase_index]);
                auto& backend_report = candidate.admission_backends[
                    admission_backend_by_source[backend_index]];
                backend_report.peak_in_flight = std::max(
                    backend_report.peak_in_flight,
                    backend_demand[backend_index]);
                candidate.report.peak_global_in_flight = std::max(
                    candidate.report.peak_global_in_flight, global_demand);
                if (phase_demand[phase_index] >
                        candidate.phases[phase_index].maximum_in_flight ||
                    backend_demand[backend_index] >
                        backends[backend_index].capabilities.max_in_flight_batches ||
                    global_demand > runtime_outstanding_capacity) {
                    return fail(Status::invalid_config,
                                "device-rate release exceeds an in-flight capacity",
                                interval.phase, interval.reference_index);
                }
                ++cursor;
            }
            event_index = cursor;
        }
        if (global_demand != initial_global_demand ||
            phase_demand != initial_phase_demand ||
            backend_demand != initial_backend_demand) {
            return fail(Status::invalid_config,
                        "device-rate admission is not cyclically balanced");
        }

        std::sort(completions.begin(), completions.end(),
                  [&](const auto& left, const auto& right) {
            const auto& left_interval = candidate.admission_intervals[left.second];
            const auto& right_interval = candidate.admission_intervals[right.second];
            return std::tie(left.first, left_interval.backend.value,
                            left_interval.reference_index) <
                std::tie(right.first, right_interval.backend.value,
                         right_interval.reference_index);
        });
        for (std::size_t index = 0; index < completions.size();) {
            const auto& first = candidate.admission_intervals[
                completions[index].second];
            std::size_t end = index + 1;
            while (end < completions.size() &&
                   completions[end].first == completions[index].first &&
                   candidate.admission_intervals[completions[end].second].backend ==
                       first.backend) {
                ++end;
            }
            const auto count = static_cast<std::uint32_t>(end - index);
            auto& backend_report = candidate.admission_backends[
                admission_backend_by_source[
                    backend_source_by_index[first.backend.index()]]];
            backend_report.peak_completions =
                std::max(backend_report.peak_completions, count);
            if (count > backend_report.completion_batch_capacity) {
                return fail(Status::invalid_config,
                            "device-rate poll boundary exceeds completion-batch capacity",
                            first.phase, first.reference_index);
            }
            index = end;
        }

        candidate.report.supercycle_ns = supercycle;
        candidate.report.runtime_outstanding_capacity =
            runtime_outstanding_capacity;
        candidate.report.runtime_completion_batch_capacity =
            runtime_completion_batch_capacity;
        candidate.report.backend_count = candidate.admission_backends.size();
        candidate.report.phase_count = candidate.admission_phases.size();
        candidate.report.interval_count =
            candidate.admission_intervals.size();
        output = std::move(candidate);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return fail(Status::internal_error, nullptr);
    }
}

Status compile_rate_dispatch(
    std::uint32_t graph_owner,
    DeterminismTier determinism_tier,
    const RateExecutionPolicy& policy,
    std::span<const GraphDependency> dependencies,
    const CompiledRatePlan& rate_plan,
    std::span<const CrossRateChannelSpec> channels,
    const CompiledDeviceRatePlan* device_rate_plan,
    CompiledRateDispatchPlan& output,
    RateDispatchDiagnostic& diagnostic) noexcept {
    diagnostic = {};
    if (policy.maximum_dispatch_records_per_step == 0 ||
        policy.maximum_dispatch_records_per_step >
            reference_release_capacity ||
        policy.host_policy_version == 0 ||
        policy.consecutive_late_threshold == 0 ||
        policy.consecutive_on_time_threshold == 0 ||
        policy.consecutive_late_threshold > rate_policy_threshold_limit ||
        policy.consecutive_on_time_threshold > rate_policy_threshold_limit ||
        policy.rate_telemetry_capacity > rate_telemetry_capacity_limit) {
        diagnostic = {
            Status::invalid_config,
            "active rate execution requires valid positive policy fields and bounded telemetry"};
        return diagnostic.status;
    }
    if (determinism_tier != DeterminismTier::unspecified) {
        diagnostic = {
            Status::invalid_config,
            "active M16-03 rate execution supports only D0 unspecified"};
        return diagnostic.status;
    }
    if (rate_plan.domains.empty() || rate_plan.releases.empty() ||
        rate_plan.supercycle_ns == 0 ||
        rate_plan.domains.size() > rate_domain_capacity) {
        diagnostic = {
            Status::invalid_config,
            "active rate execution requires a finite explicit rate plan"};
        return diagnostic.status;
    }

    try {
        std::array<bool, rate_domain_capacity> producer_domains{};
        std::array<bool, rate_domain_capacity> cpu_domains{};
        for (const auto& binding : rate_plan.bindings) {
            if (binding.phase_kind == RatePhaseKind::cpu &&
                binding.domain.valid() &&
                binding.domain.index() < rate_plan.domains.size()) {
                cpu_domains[binding.domain.index()] = true;
            }
        }
        for (const auto& channel : channels) {
            if (!channel.producer.valid() ||
                channel.producer.owner() != graph_owner ||
                channel.producer.index() >= rate_plan.bindings.size() ||
                !channel.consumer.valid() ||
                channel.consumer.owner() != graph_owner ||
                channel.consumer.index() >= rate_plan.bindings.size()) {
                diagnostic = {
                    Status::invalid_handle,
                    "active cross-rate endpoint is invalid"};
                return diagnostic.status;
            }
            const auto producer_binding = std::find_if(
                rate_plan.bindings.begin(),
                rate_plan.bindings.end(),
                [&](const CompiledRateBinding& candidate) {
                    return candidate.phase == channel.producer;
                });
            const auto consumer_binding = std::find_if(
                rate_plan.bindings.begin(),
                rate_plan.bindings.end(),
                [&](const CompiledRateBinding& candidate) {
                    return candidate.phase == channel.consumer;
                });
            if (producer_binding == rate_plan.bindings.end() ||
                consumer_binding == rate_plan.bindings.end() ||
                !producer_binding->domain.valid() ||
                !consumer_binding->domain.valid() ||
                producer_binding->domain.index() >= rate_plan.domains.size() ||
                consumer_binding->domain.index() >= rate_plan.domains.size()) {
                diagnostic = {
                    Status::invalid_handle,
                    "active cross-rate endpoint has no valid domain"};
                return diagnostic.status;
            }
            if (rate_plan.domains[producer_binding->domain.index()].optional ||
                rate_plan.domains[consumer_binding->domain.index()].optional) {
                diagnostic = {
                    Status::invalid_config,
                    "optional domains cannot produce or consume active cross-rate channels"};
                return diagnostic.status;
            }
            producer_domains[producer_binding->domain.index()] = true;
        }

        for (std::size_t index = 0;
             index < rate_plan.domains.size();
             ++index) {
            const auto& domain = rate_plan.domains[index];
            if (domain.period_ns == 0 ||
                domain.relative_deadline_ns == 0 ||
                (cpu_domains[index] &&
                 domain.relative_deadline_ns > domain.period_ns) ||
                !valid_action(domain.late_action) ||
                (domain.late_action == RateLateAction::bounded_catch_up &&
                 domain.bounded_catch_up_limit == 0) ||
                (domain.late_action != RateLateAction::bounded_catch_up &&
                 domain.bounded_catch_up_limit != 0) ||
                (domain.late_action == RateLateAction::skip &&
                 producer_domains[index])) {
                diagnostic = {
                    Status::invalid_config,
                    "active rate domain has invalid deadline, budget, optionality, late action, or catch-up bound"};
                return diagnostic.status;
            }
        }
        for (const auto& binding : rate_plan.bindings) {
            if (binding.phase_kind == RatePhaseKind::device &&
                (!device_rate_plan ||
                 std::none_of(
                     device_rate_plan->phases.begin(),
                     device_rate_plan->phases.end(),
                     [&](const auto& phase) {
                         return phase.phase == binding.phase &&
                             phase.domain == binding.domain;
                     }))) {
                diagnostic = {
                    Status::invalid_config,
                    "active device rate execution requires an admitted HAL-v2 command-batch phase"};
                return diagnostic.status;
            }
        }

        std::vector<std::size_t> domain_by_phase(
            rate_plan.bindings.size(),
            std::numeric_limits<std::size_t>::max());
        for (const auto& binding : rate_plan.bindings) {
            if (!binding.phase.valid() ||
                binding.phase.owner() != graph_owner ||
                binding.phase.index() >= domain_by_phase.size() ||
                !binding.domain.valid() ||
                binding.domain.owner() != graph_owner ||
                binding.domain.index() >= rate_plan.domains.size()) {
                diagnostic = {
                    Status::invalid_handle,
                    "active rate binding is invalid"};
                return diagnostic.status;
            }
            domain_by_phase[binding.phase.index()] = binding.domain.index();
        }
        for (const auto& dependency : dependencies) {
            if (!dependency.prerequisite.valid() ||
                !dependency.dependent.valid() ||
                dependency.prerequisite.index() >= domain_by_phase.size() ||
                dependency.dependent.index() >= domain_by_phase.size() ||
                domain_by_phase[dependency.prerequisite.index()] !=
                    domain_by_phase[dependency.dependent.index()]) {
                diagnostic = {
                    Status::invalid_config,
                    "active rate execution rejects ordinary cross-domain graph dependencies; use a cross-rate channel"};
                return diagnostic.status;
            }
        }

        CompiledRateDispatchPlan candidate;
        candidate.policy = policy;
        candidate.admission.reserve(rate_plan.releases.size());
        candidate.groups.reserve(rate_plan.releases.size());
        candidate.optional_shed_order.reserve(rate_plan.domains.size());
        candidate.device_phase_by_reference.assign(
            rate_plan.releases.size(),
            std::numeric_limits<std::size_t>::max());
        candidate.producer_channel_offsets.assign(
            rate_plan.bindings.size() + 1, 0);
        const auto has_device_consumer = std::any_of(
            channels.begin(), channels.end(),
            [](const CrossRateChannelSpec& channel) {
                return channel.consumer_device.valid();
            });
        if (has_device_consumer) {
            candidate.consumer_channel_offsets.assign(
                rate_plan.bindings.size() + 1, 0);
        }
        for (const auto& channel : channels) {
            ++candidate.producer_channel_offsets[
                channel.producer.index() + 1];
            if (has_device_consumer && channel.consumer_device.valid()) {
                ++candidate.consumer_channel_offsets[
                    channel.consumer.index() + 1];
            }
        }
        for (std::size_t index = 1;
             index < candidate.producer_channel_offsets.size(); ++index) {
            candidate.producer_channel_offsets[index] +=
                candidate.producer_channel_offsets[index - 1];
            if (has_device_consumer) {
                candidate.consumer_channel_offsets[index] +=
                    candidate.consumer_channel_offsets[index - 1];
            }
        }
        candidate.producer_channel_indices.resize(channels.size());
        if (has_device_consumer) {
            candidate.consumer_channel_indices.resize(
                static_cast<std::size_t>(std::count_if(
                    channels.begin(), channels.end(),
                    [](const CrossRateChannelSpec& channel) {
                        return channel.consumer_device.valid();
                    })));
        }
        auto next_producer_channel = candidate.producer_channel_offsets;
        auto next_consumer_channel = candidate.consumer_channel_offsets;
        for (std::size_t channel_index = 0;
             channel_index < channels.size(); ++channel_index) {
            const auto phase_index = channels[channel_index].producer.index();
            candidate.producer_channel_indices[
                next_producer_channel[phase_index]++] = channel_index;
            if (has_device_consumer &&
                channels[channel_index].consumer_device.valid()) {
                const auto consumer_phase_index =
                    channels[channel_index].consumer.index();
                candidate.consumer_channel_indices[
                    next_consumer_channel[consumer_phase_index]++] =
                        channel_index;
            }
        }
        for (std::size_t index = 0; index < rate_plan.domains.size(); ++index) {
            if (rate_plan.domains[index].optional) {
                candidate.optional_shed_order.push_back(index);
            }
        }
        std::sort(
            candidate.optional_shed_order.begin(),
            candidate.optional_shed_order.end(),
            [&](std::size_t left, std::size_t right) {
                const auto left_criticality = static_cast<std::uint8_t>(
                    rate_plan.domains[left].criticality);
                const auto right_criticality = static_cast<std::uint8_t>(
                    rate_plan.domains[right].criticality);
                return left_criticality != right_criticality
                    ? left_criticality < right_criticality
                    : left > right;
            });

        std::uint64_t previous_finish = 0;
        for (std::size_t index = 0;
             index < rate_plan.releases.size();
             ++index) {
            const auto& release = rate_plan.releases[index];
            if ((release.phase_kind != RatePhaseKind::cpu &&
                 release.phase_kind != RatePhaseKind::device) ||
                release.relative_deadline_ns == 0 ||
                release.domain_registration_index >=
                    rate_plan.domains.size()) {
                diagnostic = {
                    Status::invalid_config,
                    "active reference record is ambiguous or unsupported",
                    index};
                return diagnostic.status;
            }
            if (release.phase_kind == RatePhaseKind::cpu &&
                release.budget_wcet_ns == 0) {
                diagnostic = {
                    Status::invalid_config,
                    "active CPU reference record requires a declared budget",
                    index};
                return diagnostic.status;
            }
            if (release.phase_kind == RatePhaseKind::device) {
                const auto admitted = std::find_if(
                    device_rate_plan->phases.begin(),
                    device_rate_plan->phases.end(),
                    [&](const CompiledDeviceRatePhase& phase) {
                        return phase.phase == release.phase &&
                               phase.domain == release.domain;
                    });
                if (admitted == device_rate_plan->phases.end()) {
                    diagnostic = {
                        Status::invalid_config,
                        "active device reference has no admitted execution record",
                        index};
                    return diagnostic.status;
                }
                candidate.device_phase_by_reference[index] =
                    static_cast<std::size_t>(std::distance(
                        device_rate_plan->phases.begin(), admitted));
            }
            if (!release.optional &&
                release.phase_kind == RatePhaseKind::cpu) {
                const auto declared_start =
                    std::max(previous_finish, release.release_time_ns);
                std::uint64_t declared_finish = 0;
                if (!checked_add(
                        declared_start,
                        release.budget_wcet_ns,
                        declared_finish)) {
                    diagnostic = {
                        Status::capacity_exceeded,
                        "declared-budget admission arithmetic overflowed",
                        index};
                    return diagnostic.status;
                }
                if (declared_finish > release.deadline_time_ns) {
                    diagnostic = {
                        Status::invalid_config,
                        "declared-budget admission found an infeasible mandatory record",
                        index};
                    return diagnostic.status;
                }
                candidate.admission.push_back({
                    index,
                    declared_start,
                    declared_finish,
                    release.deadline_time_ns,
                });
                previous_finish = declared_finish;
            }

            if (candidate.groups.empty() ||
                candidate.groups.back().domain_registration_index !=
                    release.domain_registration_index ||
                candidate.groups.back().domain_release_sequence !=
                    release.domain_release_sequence) {
                candidate.groups.push_back({
                    index,
                    1,
                    release.domain_registration_index,
                    release.domain_release_sequence,
                    release.release_time_ns,
                });
            } else {
                ++candidate.groups.back().reference_count;
            }
        }
        if (previous_finish > rate_plan.supercycle_ns) {
            diagnostic = {
                Status::invalid_config,
                "declared-budget admission is not idle at the supercycle boundary",
                rate_plan.releases.size() - 1};
            return diagnostic.status;
        }
        for (const auto& group : candidate.groups) {
            auto& records = candidate.records_per_domain_release[
                group.domain_registration_index];
            if (records == 0) {
                records = group.reference_count;
            } else if (records != group.reference_count) {
                diagnostic = {
                    Status::invalid_config,
                    "active domain release has an ambiguous record count",
                    group.first_reference_index};
                return diagnostic.status;
            }
        }
        candidate.device_dependency_offsets.resize(
            rate_plan.releases.size() + 1, 0);
        for (const auto& group : candidate.groups) {
            std::size_t device_records = 0;
            const auto group_end =
                group.first_reference_index + group.reference_count;
            for (std::size_t reference_index = group.first_reference_index;
                 reference_index < group_end; ++reference_index) {
                if (candidate.device_phase_by_reference[reference_index] !=
                    std::numeric_limits<std::size_t>::max()) {
                    ++device_records;
                }
                const auto& dependent = rate_plan.releases[reference_index];
                for (const auto& dependency : dependencies) {
                    if (dependency.dependent != dependent.phase) {
                        continue;
                    }
                    for (std::size_t prerequisite_index =
                             group.first_reference_index;
                         prerequisite_index < reference_index;
                         ++prerequisite_index) {
                        if (rate_plan.releases[prerequisite_index].phase ==
                                dependency.prerequisite &&
                            candidate.device_phase_by_reference[
                                prerequisite_index] !=
                                std::numeric_limits<std::size_t>::max()) {
                            candidate.device_dependencies.push_back(
                                prerequisite_index);
                            break;
                        }
                    }
                }
                candidate.device_dependency_offsets[reference_index + 1] =
                    candidate.device_dependencies.size();
            }
            candidate.maximum_device_records_per_group = std::max(
                candidate.maximum_device_records_per_group,
                device_records);
        }
        for (const auto& group : candidate.groups) {
            ++candidate.domain_group_offsets[
                group.domain_registration_index + 1];
        }
        for (std::size_t index = 1;
             index < candidate.domain_group_offsets.size();
             ++index) {
            candidate.domain_group_offsets[index] +=
                candidate.domain_group_offsets[index - 1];
        }
        candidate.domain_group_indices.resize(candidate.groups.size());
        auto next_group = candidate.domain_group_offsets;
        for (std::size_t group_index = 0;
             group_index < candidate.groups.size();
             ++group_index) {
            const auto domain = candidate.groups[group_index]
                .domain_registration_index;
            candidate.domain_group_indices[next_group[domain]++] =
                group_index;
        }

        output = std::move(candidate);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        diagnostic = {Status::resource_exhausted, nullptr};
        return diagnostic.status;
    } catch (...) {
        diagnostic = {Status::internal_error, nullptr};
        return diagnostic.status;
    }
}

Status count_due_domain_releases(
    const CompiledRatePlan& rate_plan,
    const CompiledRateDispatchPlan& dispatch_plan,
    std::size_t domain_index,
    std::uint64_t cursor_ns,
    std::uint64_t end_ns,
    std::uint64_t& domain_releases,
    std::uint64_t& reference_records) noexcept {
    domain_releases = 0;
    reference_records = 0;
    if (cursor_ns > end_ns || domain_index >= rate_plan.domains.size()) {
        return Status::invalid_argument;
    }
    const auto period = rate_plan.domains[domain_index].period_ns;
    const auto records =
        dispatch_plan.records_per_domain_release[domain_index];
    std::uint64_t first_sequence = 0;
    std::uint64_t end_sequence = 0;
    if (period == 0 || records == 0 ||
        !ceil_div(cursor_ns, period, first_sequence) ||
        !ceil_div(end_ns, period, end_sequence) ||
        end_sequence < first_sequence) {
        return Status::invalid_argument;
    }
    domain_releases = end_sequence - first_sequence;
    if (!checked_multiply(
            domain_releases,
            static_cast<std::uint64_t>(records),
            reference_records)) {
        domain_releases = 0;
        return Status::capacity_exceeded;
    }
    return Status::ok;
}

Status count_due_rate_work(
    const CompiledRatePlan& rate_plan,
    const CompiledRateDispatchPlan& dispatch_plan,
    std::uint64_t cursor_ns,
    std::uint64_t end_ns,
    RateDueCounts& output) noexcept {
    RateDueCounts candidate;
    if (cursor_ns > end_ns) {
        return Status::invalid_argument;
    }
    for (std::size_t domain = 0;
         domain < rate_plan.domains.size();
         ++domain) {
        std::uint64_t releases = 0;
        std::uint64_t records = 0;
        const auto status = count_due_domain_releases(
            rate_plan,
            dispatch_plan,
            domain,
            cursor_ns,
            end_ns,
            releases,
            records);
        if (status != Status::ok ||
            !checked_add(
                candidate.domain_releases,
                releases,
                candidate.domain_releases) ||
            !checked_add(
                candidate.reference_records,
                records,
                candidate.reference_records)) {
            return Status::capacity_exceeded;
        }
    }
    output = candidate;
    return Status::ok;
}

} // namespace rt::detail
