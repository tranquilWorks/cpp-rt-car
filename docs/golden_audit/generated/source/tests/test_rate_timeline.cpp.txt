#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

#include <rt/runtime.hpp>
#include <rt/mock_device.hpp>

#include "rt/src/rate_timeline.hpp"

namespace {

using namespace std::chrono_literals;

rt::CallbackResult count_callback(
    void* user_data,
    const rt::CallbackContext&) {
    ++*static_cast<std::size_t*>(user_data);
    return rt::CallbackResult::ok;
}

rt::CallbackResult submit_noop(
    void*,
    const rt::DeviceCallbackContext&,
    rt::DeviceSubmission& submission) {
    submission.timeout_ns = 10'000;
    submission.opcode = rt::mock_device_opcode_noop;
    return rt::CallbackResult::ok;
}

struct ManualPeriodicClock final : rt::RuntimeClock {
    std::uint64_t now = 1'000;
    std::vector<std::uint64_t> sleeps;

    std::uint64_t now_ns() noexcept override { return now; }
    rt::Status sleep_until_ns(std::uint64_t release) noexcept override {
        sleeps.push_back(release);
        now = release;
        return rt::Status::ok;
    }
    bool supports_absolute_sleep() const noexcept override { return true; }
};

struct FinalizeProviderProbe {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(4096) Slot {
        std::array<std::byte, 16 * 1024> storage{};
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    rt::MemoryProvider table() noexcept {
        rt::MemoryProvider provider;
        provider.capabilities =
            rt::memory_provider_capability_bit(
                rt::MemoryProviderCapability::policy_operations) |
            rt::memory_provider_capability_bit(
                rt::MemoryProviderCapability::independent_observation);
        provider.user_data = this;
        provider.acquire = &acquire;
        provider.apply = &apply;
        provider.observe = &observe;
        provider.rollback = &rollback;
        provider.release = &release;
        return provider;
    }

    std::array<Slot, 3> slots{};
    std::size_t acquire_calls = 0;
    std::size_t apply_calls = 0;
    std::size_t observe_calls = 0;
    std::size_t rollback_calls = 0;
    std::size_t release_calls = 0;

private:
    static FinalizeProviderProbe& self(void* user_data) noexcept {
        return *static_cast<FinalizeProviderProbe*>(user_data);
    }

    static std::size_t slot_index(rt::MemoryRegionId region) noexcept {
        return static_cast<std::size_t>(
            region.value - rt::memory_region_phase_scratch.value);
    }

    static rt::Status acquire(
        void* user_data,
        const rt::MemoryProviderAcquireRequest& request,
        rt::MemoryProviderAllocation& allocation) noexcept {
        auto& probe = self(user_data);
        ++probe.acquire_calls;
        const auto index = slot_index(request.region);
        if (index >= probe.slots.size() ||
            request.logical_bytes > probe.slots[index].storage.size()) {
            return rt::Status::resource_exhausted;
        }
        auto& slot = probe.slots[index];
        allocation.token = &slot;
        allocation.allocation_base = slot.storage.data();
        allocation.allocation_bytes = slot.storage.size();
        allocation.usable_data = slot.storage.data();
        allocation.usable_bytes = request.logical_bytes;
        allocation.committed_bytes = request.logical_bytes;
        allocation.alignment = request.required_alignment;
        return rt::Status::ok;
    }

    static rt::Status apply(
        void* user_data,
        void*,
        const rt::MemoryPolicy&,
        rt::MemoryProviderObservation&) noexcept {
        ++self(user_data).apply_calls;
        return rt::Status::ok;
    }

    static rt::Status observe(
        void* user_data,
        void*,
        const rt::MemoryPolicy&,
        rt::MemoryProviderObservation&) noexcept {
        ++self(user_data).observe_calls;
        return rt::Status::ok;
    }

    static rt::Status rollback(
        void* user_data,
        void*,
        const rt::MemoryPolicy&,
        const rt::MemoryProviderObservation&) noexcept {
        ++self(user_data).rollback_calls;
        return rt::Status::ok;
    }

    static void release(
        void* user_data,
        void*,
        rt::RollbackIntent) noexcept {
        ++self(user_data).release_calls;
    }
};

struct ExpectedRelease {
    std::uint64_t time;
    std::size_t domain;
    std::uint64_t sequence;
    std::size_t phase;
    std::uint32_t substep;
};

std::vector<ExpectedRelease> independent_reference(
    std::uint64_t supercycle,
    const std::vector<std::uint64_t>& periods,
    const std::vector<std::uint32_t>& substeps,
    const std::vector<std::size_t>& phase_domains) {
    std::vector<ExpectedRelease> output;
    for (std::size_t domain = 0; domain < periods.size(); ++domain) {
        for (std::uint64_t sequence = 0;
             sequence < supercycle / periods[domain];
             ++sequence) {
            for (std::size_t phase = 0;
                 phase < phase_domains.size();
                 ++phase) {
                if (phase_domains[phase] != domain) {
                    continue;
                }
                for (std::uint32_t substep = 0;
                     substep < substeps[domain];
                     ++substep) {
                    output.push_back({
                        sequence * periods[domain],
                        domain,
                        sequence,
                        phase,
                        substep,
                    });
                }
            }
        }
    }
    std::sort(output.begin(), output.end(), [](const auto& left, const auto& right) {
        return std::tie(
                   left.time,
                   left.domain,
                   left.phase,
                   left.substep) <
            std::tie(
                   right.time,
                   right.domain,
                   right.phase,
                   right.substep);
    });
    return output;
}

rt::CheckpointMetadata checkpoint_metadata(rt::Runtime& runtime) {
    std::size_t required = 0;
    EXPECT_EQ(runtime.checkpoint_size(required), rt::Status::ok);
    std::vector<std::byte> bytes(required);
    rt::ArtifactWriteResult result;
    EXPECT_EQ(runtime.write_checkpoint(0, bytes, result), rt::Status::ok);
    rt::CheckpointMetadata metadata;
    EXPECT_EQ(rt::inspect_checkpoint_artifact(bytes, metadata), rt::Status::ok);
    return metadata;
}

std::uint64_t single_domain_graph_id(
    const rt::RateDomainRegistration& registration) {
    rt::Runtime runtime;
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    EXPECT_EQ(
        runtime.register_callback({"phase", &count_callback, nullptr}, phase),
        rt::Status::ok);
    EXPECT_EQ(runtime.register_rate_domain(registration, domain), rt::Status::ok);
    EXPECT_EQ(runtime.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::ok);
    return checkpoint_metadata(runtime).graph_id;
}

} // namespace

TEST(RateTimeline, PublicDefaultsAndLegacyNoPlanRemainAdditive) {
    const rt::RateDomainRegistration rate{};
    EXPECT_TRUE(rate.name.empty());
    EXPECT_EQ(rate.period_ns, 0u);
    EXPECT_EQ(rate.substep_count, 1u);
    EXPECT_EQ(rate.criticality, rt::RateCriticality::normal);

    // A shorter pre-M16 aggregate prefix remains valid after tail additions.
    const rt::MemoryPlan pre_m16_plan{1024, 512};
    EXPECT_EQ(pre_m16_plan.memory_budget_bytes, 1024u);
    EXPECT_EQ(pre_m16_plan.planned_bytes, 512u);
    EXPECT_EQ(pre_m16_plan.rate_plan_bytes, 0u);

    rt::Runtime runtime;
    EXPECT_FALSE(runtime.rate_model_enabled());
    EXPECT_EQ(runtime.rate_domain_count(), 0u);
    EXPECT_EQ(runtime.reference_supercycle_ns(), 0u);
    rt::CompiledRateDomain domain;
    rt::ReferenceRelease release;
    EXPECT_FALSE(runtime.compiled_rate_domain_at(0, domain));
    EXPECT_FALSE(runtime.reference_release_at(0, release));
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_FALSE(runtime.rate_model_enabled());
}

TEST(RateTimeline, HarmonicAndNonHarmonicTimelineMatchesIndependentGenerator) {
    rt::Runtime runtime;
    rt::PhaseHandle phase_a;
    rt::PhaseHandle phase_b;
    ASSERT_EQ(
        runtime.register_callback({"phase.a", &count_callback, nullptr}, phase_a),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"phase.b", &count_callback, nullptr}, phase_b),
        rt::Status::ok);
    ASSERT_EQ(runtime.add_dependency(phase_a, phase_b), rt::Status::ok);

    rt::RateDomainHandle period_four;
    rt::RateDomainHandle period_six;
    ASSERT_EQ(
        runtime.register_rate_domain(
            {"four", 4, 2, 3, 2, rt::RateCriticality::critical, false},
            period_four),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {"six", 6, 1, 5, 7, rt::RateCriticality::background, true},
            period_six),
        rt::Status::ok);
    // Registration order dominates compiled phase order at equal timestamps.
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phase_b, period_four), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phase_a, period_six), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    ASSERT_TRUE(runtime.rate_model_enabled());
    EXPECT_EQ(runtime.reference_supercycle_ns(), 12u);
    ASSERT_EQ(runtime.rate_domain_count(), 2u);
    ASSERT_EQ(runtime.rate_binding_count(), 2u);
    const auto expected = independent_reference(12, {4, 6}, {2, 1}, {1, 0});
    ASSERT_EQ(runtime.reference_release_count(), expected.size());

    rt::CompiledRateDomain four;
    rt::CompiledRateDomain six;
    ASSERT_TRUE(runtime.compiled_rate_domain_at(0, four));
    ASSERT_TRUE(runtime.compiled_rate_domain_at(1, six));
    EXPECT_STREQ(four.name.data(), "four");
    EXPECT_EQ(four.releases_per_supercycle, 3u);
    EXPECT_EQ(four.period_ratio_numerator, 1u);
    EXPECT_EQ(four.period_ratio_denominator, 1u);
    EXPECT_EQ(six.period_ratio_numerator, 3u);
    EXPECT_EQ(six.period_ratio_denominator, 2u);

    for (std::size_t index = 0; index < expected.size(); ++index) {
        rt::ReferenceRelease actual;
        ASSERT_TRUE(runtime.reference_release_at(index, actual));
        const auto expected_domain = expected[index].domain == 0
            ? period_four
            : period_six;
        const auto expected_phase = expected[index].phase == 0
            ? phase_a
            : phase_b;
        const std::array<std::uint32_t, 2> expected_substeps{2, 1};
        const std::array<std::uint64_t, 2> expected_deadlines{3, 5};
        const std::array<std::uint64_t, 2> expected_budgets{2, 7};
        const std::array<rt::RateCriticality, 2> expected_criticalities{
            rt::RateCriticality::critical,
            rt::RateCriticality::background,
        };
        const std::array<bool, 2> expected_optional{false, true};
        EXPECT_EQ(actual.release_time_ns, expected[index].time);
        EXPECT_EQ(actual.domain, expected_domain);
        EXPECT_EQ(actual.domain_registration_index, expected[index].domain);
        EXPECT_EQ(actual.domain_release_sequence, expected[index].sequence);
        EXPECT_EQ(actual.phase, expected_phase);
        EXPECT_EQ(actual.phase_kind, rt::RatePhaseKind::cpu);
        EXPECT_EQ(actual.compiled_phase_index, expected[index].phase);
        EXPECT_EQ(actual.substep_ordinal, expected[index].substep);
        EXPECT_EQ(
            actual.substep_count,
            expected_substeps[expected[index].domain]);
        EXPECT_EQ(
            actual.relative_deadline_ns,
            expected_deadlines[expected[index].domain]);
        EXPECT_EQ(
            actual.deadline_time_ns,
            expected[index].time + expected_deadlines[expected[index].domain]);
        EXPECT_EQ(
            actual.budget_wcet_ns,
            expected_budgets[expected[index].domain]);
        EXPECT_EQ(
            actual.criticality,
            expected_criticalities[expected[index].domain]);
        EXPECT_EQ(
            actual.optional,
            expected_optional[expected[index].domain]);
    }
    rt::ReferenceRelease out_of_range;
    EXPECT_FALSE(runtime.reference_release_at(expected.size(), out_of_range));
}

TEST(RateTimeline, ValidationIsBoundedOwnedAndTransactional) {
    rt::Runtime first;
    rt::Runtime second;
    rt::PhaseHandle first_phase;
    rt::PhaseHandle second_phase;
    ASSERT_EQ(
        first.register_callback({"first", &count_callback, nullptr}, first_phase),
        rt::Status::ok);
    ASSERT_EQ(
        second.register_callback({"second", &count_callback, nullptr}, second_phase),
        rt::Status::ok);

    rt::RateDomainHandle domain;
    rt::RateDomainHandle ignored;
    EXPECT_EQ(
        first.register_rate_domain({"", 1}, ignored),
        rt::Status::invalid_argument);
    EXPECT_EQ(
        first.register_rate_domain({"bad name", 1}, ignored),
        rt::Status::invalid_argument);
    EXPECT_EQ(
        first.register_rate_domain({"zero", 0}, ignored),
        rt::Status::invalid_argument);
    EXPECT_EQ(
        first.register_rate_domain({"zero.substeps", 1, 0}, ignored),
        rt::Status::invalid_argument);
    EXPECT_EQ(
        first.register_rate_domain(
            {"enum", 1, 1, 0, 0, static_cast<rt::RateCriticality>(255), false},
            ignored),
        rt::Status::invalid_argument);
    ASSERT_EQ(first.register_rate_domain({"main", 10, 1, 0}, domain), rt::Status::ok);
    EXPECT_EQ(first.register_rate_domain({"main", 20}, ignored), rt::Status::invalid_argument);
    EXPECT_EQ(
        first.bind_phase_to_rate_domain(second_phase, domain),
        rt::Status::invalid_handle);
    EXPECT_EQ(
        second.bind_phase_to_rate_domain(second_phase, domain),
        rt::Status::invalid_handle);

    EXPECT_EQ(first.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(first.state(), rt::RuntimeState::configuring);
    EXPECT_FALSE(first.rate_model_enabled());
    EXPECT_EQ(first.reference_release_count(), 0u);
    ASSERT_EQ(first.bind_phase_to_rate_domain(first_phase, domain), rt::Status::ok);
    EXPECT_EQ(
        first.bind_phase_to_rate_domain(first_phase, domain),
        rt::Status::invalid_argument);
    ASSERT_EQ(first.finalize(), rt::Status::ok);
    EXPECT_EQ(first.replace_rate_domain(domain, {"replacement", 20}), rt::Status::invalid_state);
    EXPECT_EQ(first.bind_phase_to_rate_domain(first_phase, domain), rt::Status::invalid_state);

    rt::Runtime capacity_runtime;
    for (std::size_t index = 0; index < rt::rate_domain_capacity; ++index) {
        ASSERT_EQ(
            capacity_runtime.register_rate_domain(
                {"domain." + std::to_string(index), 1},
                ignored),
            rt::Status::ok);
    }
    EXPECT_EQ(
        capacity_runtime.register_rate_domain({"overflow", 1}, ignored),
        rt::Status::capacity_exceeded);
}

TEST(RateTimeline, FailedCompilationHasNoProviderSideEffectsAndCanRetry) {
    FinalizeProviderProbe provider;
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.scratch_bytes = 64;
    config.trace_capacity = 1;
    config.executor_queue_capacity = 2;
    config.task_scratch_bytes = 64;
    config.task_scratch_slots = 1;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(runtime.set_memory_provider(provider.table()), rt::Status::ok);

    std::size_t callback_calls = 0;
    rt::PhaseHandle phase;
    ASSERT_EQ(
        runtime.register_callback(
            {"phase", &count_callback, &callback_calls},
            phase),
        rt::Status::ok);
    rt::RateDomainHandle domain;
    ASSERT_EQ(
        runtime.register_rate_domain({"rate", 1}, domain),
        rt::Status::ok);

    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    EXPECT_FALSE(runtime.rate_model_enabled());
    EXPECT_EQ(provider.acquire_calls, 0u);
    EXPECT_EQ(provider.apply_calls, 0u);
    EXPECT_EQ(provider.observe_calls, 0u);
    EXPECT_EQ(provider.rollback_calls, 0u);
    EXPECT_EQ(provider.release_calls, 0u);
    EXPECT_EQ(callback_calls, 0u);
    EXPECT_EQ(runtime.executor_stats().worker_starts, 0u);

    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(phase, domain),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    EXPECT_EQ(provider.acquire_calls, 3u);
    EXPECT_EQ(provider.apply_calls, 0u);
    EXPECT_EQ(provider.observe_calls, 0u);
    EXPECT_EQ(provider.rollback_calls, 0u);
    EXPECT_EQ(callback_calls, 0u);
    EXPECT_EQ(runtime.executor_stats().worker_starts, 0u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(provider.release_calls, 3u);
}

TEST(RateTimeline, CheckedArithmeticRejectsOverflowCapacityAndRecovers) {
    rt::Runtime maximum_valid;
    rt::PhaseHandle maximum_phase;
    rt::RateDomainHandle maximum_domain;
    ASSERT_EQ(maximum_valid.register_callback({"max", &count_callback, nullptr}, maximum_phase), rt::Status::ok);
    ASSERT_EQ(maximum_valid.register_rate_domain(
                  {"max", std::numeric_limits<std::uint64_t>::max(), 1,
                   std::numeric_limits<std::uint64_t>::max()},
                  maximum_domain),
              rt::Status::ok);
    ASSERT_EQ(maximum_valid.bind_phase_to_rate_domain(maximum_phase, maximum_domain), rt::Status::ok);
    ASSERT_EQ(maximum_valid.finalize(), rt::Status::ok);
    EXPECT_EQ(maximum_valid.reference_supercycle_ns(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(maximum_valid.reference_release_count(), 1u);

    rt::Runtime overflow;
    rt::PhaseHandle first;
    rt::PhaseHandle second;
    ASSERT_EQ(overflow.register_callback({"a", &count_callback, nullptr}, first), rt::Status::ok);
    ASSERT_EQ(overflow.register_callback({"b", &count_callback, nullptr}, second), rt::Status::ok);
    rt::RateDomainHandle huge;
    rt::RateDomainHandle near_huge;
    ASSERT_EQ(
        overflow.register_rate_domain(
            {"huge", std::numeric_limits<std::uint64_t>::max(), 1, 0},
            huge),
        rt::Status::ok);
    ASSERT_EQ(
        overflow.register_rate_domain(
            {"near", std::numeric_limits<std::uint64_t>::max() - 1, 1, 0},
            near_huge),
        rt::Status::ok);
    ASSERT_EQ(overflow.bind_phase_to_rate_domain(first, huge), rt::Status::ok);
    ASSERT_EQ(overflow.bind_phase_to_rate_domain(second, near_huge), rt::Status::ok);
    EXPECT_EQ(overflow.finalize(), rt::Status::capacity_exceeded);
    EXPECT_EQ(overflow.state(), rt::RuntimeState::configuring);
    ASSERT_EQ(overflow.replace_rate_domain(huge, {"huge", 2, 1, 0}), rt::Status::ok);
    ASSERT_EQ(overflow.replace_rate_domain(near_huge, {"near", 4, 1, 0}), rt::Status::ok);
    EXPECT_EQ(overflow.finalize(), rt::Status::ok);

    rt::Runtime count_overflow;
    rt::PhaseHandle fast_phase;
    rt::PhaseHandle slow_phase;
    ASSERT_EQ(count_overflow.register_callback({"fast", &count_callback, nullptr}, fast_phase), rt::Status::ok);
    ASSERT_EQ(count_overflow.register_callback({"slow", &count_callback, nullptr}, slow_phase), rt::Status::ok);
    rt::RateDomainHandle fast;
    rt::RateDomainHandle slow;
    ASSERT_EQ(count_overflow.register_rate_domain({"fast", 1}, fast), rt::Status::ok);
    ASSERT_EQ(count_overflow.register_rate_domain({"slow", 65'536}, slow), rt::Status::ok);
    ASSERT_EQ(count_overflow.bind_phase_to_rate_domain(fast_phase, fast), rt::Status::ok);
    ASSERT_EQ(count_overflow.bind_phase_to_rate_domain(slow_phase, slow), rt::Status::ok);
    EXPECT_EQ(count_overflow.finalize(), rt::Status::capacity_exceeded);

    rt::Runtime deadline_overflow;
    rt::PhaseHandle p2;
    rt::PhaseHandle p4;
    ASSERT_EQ(deadline_overflow.register_callback({"p2", &count_callback, nullptr}, p2), rt::Status::ok);
    ASSERT_EQ(deadline_overflow.register_callback({"p4", &count_callback, nullptr}, p4), rt::Status::ok);
    rt::RateDomainHandle d2;
    rt::RateDomainHandle d4;
    ASSERT_EQ(deadline_overflow.register_rate_domain(
                  {"p2", 2, 1, std::numeric_limits<std::uint64_t>::max()}, d2),
              rt::Status::ok);
    ASSERT_EQ(deadline_overflow.register_rate_domain({"p4", 4, 1, 0}, d4), rt::Status::ok);
    ASSERT_EQ(deadline_overflow.bind_phase_to_rate_domain(p2, d2), rt::Status::ok);
    ASSERT_EQ(deadline_overflow.bind_phase_to_rate_domain(p4, d4), rt::Status::ok);
    EXPECT_EQ(deadline_overflow.finalize(), rt::Status::capacity_exceeded);
}

TEST(RateTimeline, ReferenceReleaseCapacityBoundaryIsInclusive) {
    rt::Runtime runtime;
    rt::PhaseHandle fast_phase;
    rt::PhaseHandle slow_phase;
    ASSERT_EQ(
        runtime.register_callback(
            {"fast", &count_callback, nullptr},
            fast_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"slow", &count_callback, nullptr},
            slow_phase),
        rt::Status::ok);

    rt::RateDomainHandle fast;
    rt::RateDomainHandle slow;
    ASSERT_EQ(
        runtime.register_rate_domain({"fast", 1}, fast),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain({"slow", 65'535}, slow),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(fast_phase, fast),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(slow_phase, slow),
        rt::Status::ok);

    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_EQ(runtime.reference_supercycle_ns(), 65'535u);
    ASSERT_EQ(
        runtime.reference_release_count(),
        rt::reference_release_capacity);

    rt::ReferenceRelease first;
    rt::ReferenceRelease second;
    rt::ReferenceRelease last;
    ASSERT_TRUE(runtime.reference_release_at(0, first));
    ASSERT_TRUE(runtime.reference_release_at(1, second));
    ASSERT_TRUE(runtime.reference_release_at(
        rt::reference_release_capacity - 1,
        last));
    EXPECT_EQ(first.release_time_ns, 0u);
    EXPECT_EQ(first.domain, fast);
    EXPECT_EQ(first.domain_release_sequence, 0u);
    EXPECT_EQ(second.release_time_ns, 0u);
    EXPECT_EQ(second.domain, slow);
    EXPECT_EQ(second.domain_release_sequence, 0u);
    EXPECT_EQ(last.release_time_ns, 65'534u);
    EXPECT_EQ(last.domain, fast);
    EXPECT_EQ(last.domain_release_sequence, 65'534u);

    rt::ReferenceRelease out_of_range;
    EXPECT_FALSE(runtime.reference_release_at(
        rt::reference_release_capacity,
        out_of_range));

    rt::MemoryPlan plan;
    ASSERT_TRUE(runtime.memory_plan(plan));
    EXPECT_EQ(
        plan.reference_release_count,
        rt::reference_release_capacity);
}

TEST(RateTimeline, ExplicitPlanDoesNotChangeHostOrPeriodicDispatch) {
    ManualPeriodicClock clock;
    rt::Runtime runtime(clock);
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    std::array<std::size_t, 2> calls{};
    rt::PhaseHandle first;
    rt::PhaseHandle second;
    ASSERT_EQ(runtime.register_callback({"first", &count_callback, &calls[0]}, first), rt::Status::ok);
    ASSERT_EQ(runtime.register_callback({"second", &count_callback, &calls[1]}, second), rt::Status::ok);
    rt::RateDomainHandle fast;
    rt::RateDomainHandle slow;
    ASSERT_EQ(runtime.register_rate_domain({"fast", 2, 3, 1, 1, rt::RateCriticality::critical, false}, fast), rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain({"slow", 5, 1, 1, 0, rt::RateCriticality::background, true}, slow), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(first, fast), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(second, slow), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(runtime.step({1, 1ns, std::nullopt}), rt::Status::ok);
    EXPECT_EQ(calls, (std::array<std::size_t, 2>{1, 1}));

    rt::PeriodicRunConfig periodic;
    periodic.frame_count = 3;
    periodic.period = 10ns;
    periodic.relative_deadline = 10ns;
    rt::PeriodicRunResult result;
    ASSERT_EQ(runtime.run_periodic(periodic, nullptr, nullptr, &result), rt::Status::ok);
    EXPECT_EQ(result.frames_executed, 3u);
    EXPECT_EQ(calls, (std::array<std::size_t, 2>{4, 4}));
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(runtime.reference_release_count(), 17u);
}

TEST(RateTimeline, ExplicitSemanticsChangeIdentityAndNoPlanIdentityIsStable) {
    rt::Runtime first_legacy;
    rt::Runtime second_legacy;
    rt::PhaseHandle first_phase;
    rt::PhaseHandle second_phase;
    ASSERT_EQ(first_legacy.register_callback({"phase", &count_callback, nullptr}, first_phase), rt::Status::ok);
    ASSERT_EQ(second_legacy.register_callback({"phase", &count_callback, nullptr}, second_phase), rt::Status::ok);
    ASSERT_EQ(first_legacy.finalize(), rt::Status::ok);
    ASSERT_EQ(second_legacy.finalize(), rt::Status::ok);
    const auto first_metadata = checkpoint_metadata(first_legacy);
    const auto second_metadata = checkpoint_metadata(second_legacy);
    EXPECT_EQ(first_metadata.graph_id, 0xbd3b06e4b2b6b217ull);
    EXPECT_EQ(first_metadata.graph_id, second_metadata.graph_id);
    EXPECT_EQ(first_metadata.replay_id, second_metadata.replay_id);

    const rt::RateDomainRegistration base{
        "rate", 10, 2, 8, 7, rt::RateCriticality::normal, false};
    const auto base_id = single_domain_graph_id(base);
    EXPECT_NE(base_id, first_metadata.graph_id);
    EXPECT_NE(single_domain_graph_id({"renamed", 10, 2, 8, 7, rt::RateCriticality::normal, false}), base_id);
    EXPECT_NE(single_domain_graph_id({"rate", 11, 2, 8, 7, rt::RateCriticality::normal, false}), base_id);
    EXPECT_NE(single_domain_graph_id({"rate", 10, 3, 8, 7, rt::RateCriticality::normal, false}), base_id);
    EXPECT_NE(single_domain_graph_id({"rate", 10, 2, 9, 7, rt::RateCriticality::normal, false}), base_id);
    EXPECT_NE(single_domain_graph_id({"rate", 10, 2, 8, 9, rt::RateCriticality::normal, false}), base_id);
    EXPECT_NE(single_domain_graph_id({"rate", 10, 2, 8, 7, rt::RateCriticality::critical, false}), base_id);
    EXPECT_NE(single_domain_graph_id({"rate", 10, 2, 8, 7, rt::RateCriticality::normal, true}), base_id);

    const auto bound_graph_id = [](bool swap) {
        rt::Runtime runtime;
        rt::PhaseHandle first;
        rt::PhaseHandle second;
        EXPECT_EQ(runtime.register_callback({"first", &count_callback, nullptr}, first), rt::Status::ok);
        EXPECT_EQ(runtime.register_callback({"second", &count_callback, nullptr}, second), rt::Status::ok);
        rt::RateDomainHandle first_domain;
        rt::RateDomainHandle second_domain;
        EXPECT_EQ(runtime.register_rate_domain({"first.rate", 2}, first_domain), rt::Status::ok);
        EXPECT_EQ(runtime.register_rate_domain({"second.rate", 3}, second_domain), rt::Status::ok);
        EXPECT_EQ(runtime.bind_phase_to_rate_domain(first, swap ? second_domain : first_domain), rt::Status::ok);
        EXPECT_EQ(runtime.bind_phase_to_rate_domain(second, swap ? first_domain : second_domain), rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::ok);
        return checkpoint_metadata(runtime).graph_id;
    };
    EXPECT_NE(bound_graph_id(false), bound_graph_id(true));
}

TEST(RateTimeline, DirectCompilerRejectsMalformedNamesAndPhaseOrder) {
    const std::uint32_t owner = 42;
    const std::array phases{rt::PhaseHandle{owner, 0}, rt::PhaseHandle{owner, 1}};
    const std::array bindings{
        rt::detail::RateBindingSpec{phases[0], rt::RateDomainHandle{owner, 0}, rt::RatePhaseKind::cpu},
        rt::detail::RateBindingSpec{phases[1], rt::RateDomainHandle{owner, 1}, rt::RatePhaseKind::device},
    };
    std::array domains{
        rt::detail::RateDomainSpec{"one", 2, 1, 0, 0, rt::RateCriticality::normal, false},
        rt::detail::RateDomainSpec{"two", 3, 1, 0, 0, rt::RateCriticality::normal, false},
    };
    rt::detail::CompiledRatePlan output;
    rt::detail::RateCompileDiagnostic diagnostic;
    EXPECT_EQ(
        rt::detail::compile_rate_timeline(owner, 2, phases, domains, bindings, output, diagnostic),
        rt::Status::ok);
    const auto retained_count = output.releases.size();

    domains[1].name = std::string(rt::rate_domain_name_capacity, 'x');
    EXPECT_EQ(
        rt::detail::compile_rate_timeline(owner, 2, phases, domains, bindings, output, diagnostic),
        rt::Status::invalid_config);
    EXPECT_EQ(output.releases.size(), retained_count);

    domains[1].name = "one";
    EXPECT_EQ(
        rt::detail::compile_rate_timeline(owner, 2, phases, domains, bindings, output, diagnostic),
        rt::Status::invalid_config);
    EXPECT_EQ(output.releases.size(), retained_count);

    domains[1].name = "two";
    const std::array duplicate_order{phases[0], phases[0]};
    EXPECT_EQ(
        rt::detail::compile_rate_timeline(owner, 2, duplicate_order, domains, bindings, output, diagnostic),
        rt::Status::invalid_config);
    EXPECT_EQ(output.releases.size(), retained_count);
}

TEST(RateTimeline, CpuAndMockDeviceBindingsRemainInstanceOwned) {
    rt::MockDeviceBackend backend({4, 1, 1, 1'000});
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.device_outstanding_capacity = 4;
    config.device_completion_batch = 1;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend({"mock", backend.api()}, backend_handle),
        rt::Status::ok);
    std::size_t cpu_calls = 0;
    rt::PhaseHandle cpu_phase;
    rt::PhaseHandle device_phase;
    ASSERT_EQ(runtime.register_callback({"cpu", &count_callback, &cpu_calls}, cpu_phase), rt::Status::ok);
    ASSERT_EQ(runtime.register_device_phase({"device", backend_handle, &submit_noop, nullptr}, device_phase), rt::Status::ok);
    rt::RateDomainHandle cpu_domain;
    rt::RateDomainHandle device_domain;
    ASSERT_EQ(runtime.register_rate_domain({"cpu.rate", 2}, cpu_domain), rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain({"device.rate", 3}, device_domain), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(cpu_phase, cpu_domain), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(device_phase, device_domain), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    rt::CompiledRateBinding cpu_binding;
    rt::CompiledRateBinding device_binding;
    ASSERT_TRUE(runtime.compiled_rate_binding_at(0, cpu_binding));
    ASSERT_TRUE(runtime.compiled_rate_binding_at(1, device_binding));
    EXPECT_EQ(cpu_binding.phase_kind, rt::RatePhaseKind::cpu);
    EXPECT_EQ(device_binding.phase_kind, rt::RatePhaseKind::device);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(runtime.step({0, 1ns, std::nullopt}), rt::Status::ok);
    EXPECT_EQ(cpu_calls, 1u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MemoryPlan, RateStorageIsExactlyInsideRuntimeControl) {
    rt::Runtime legacy;
    rt::Runtime explicit_rate;
    rt::PhaseHandle legacy_phase;
    rt::PhaseHandle rate_phase;
    ASSERT_EQ(legacy.register_callback({"phase", &count_callback, nullptr}, legacy_phase), rt::Status::ok);
    ASSERT_EQ(explicit_rate.register_callback({"phase", &count_callback, nullptr}, rate_phase), rt::Status::ok);
    rt::RateDomainHandle domain;
    ASSERT_EQ(explicit_rate.register_rate_domain({"rate", 5, 2, 4, 3}, domain), rt::Status::ok);
    ASSERT_EQ(explicit_rate.bind_phase_to_rate_domain(rate_phase, domain), rt::Status::ok);
    ASSERT_EQ(legacy.finalize(), rt::Status::ok);
    ASSERT_EQ(explicit_rate.finalize(), rt::Status::ok);
    rt::MemoryPlan legacy_plan;
    rt::MemoryPlan rate_plan;
    ASSERT_TRUE(legacy.memory_plan(legacy_plan));
    ASSERT_TRUE(explicit_rate.memory_plan(rate_plan));
    EXPECT_EQ(legacy_plan.rate_plan_bytes, 0u);
    EXPECT_GT(rate_plan.rate_plan_bytes, 0u);
    EXPECT_EQ(rate_plan.rate_domain_count, 1u);
    EXPECT_EQ(rate_plan.rate_binding_count, 1u);
    EXPECT_EQ(rate_plan.reference_release_count, 2u);
    EXPECT_EQ(
        rate_plan.runtime_control_bytes - legacy_plan.runtime_control_bytes,
        rate_plan.rate_plan_bytes);
    EXPECT_EQ(
        rate_plan.planned_bytes,
        rate_plan.runtime_control_bytes + rate_plan.executor_control_bytes +
            rate_plan.device_control_bytes + rate_plan.phase_scratch_total_bytes +
            rate_plan.task_scratch_total_bytes + rate_plan.trace_storage_bytes);
}

TEST(DeterminismReplay, RateIdentityRejectsSemanticallyDifferentPlanTransactionally) {
    std::array<std::byte, 8> producer_state{};
    std::array<std::byte, 8> consumer_state{};
    producer_state.fill(std::byte{0x2a});
    consumer_state.fill(std::byte{0x55});
    rt::Runtime producer;
    rt::Runtime consumer;
    rt::PhaseHandle producer_phase;
    rt::PhaseHandle consumer_phase;
    ASSERT_EQ(producer.register_callback({"phase", &count_callback, nullptr}, producer_phase), rt::Status::ok);
    ASSERT_EQ(consumer.register_callback({"phase", &count_callback, nullptr}, consumer_phase), rt::Status::ok);
    ASSERT_EQ(producer.register_state({"state", 1, producer_state}), rt::Status::ok);
    ASSERT_EQ(consumer.register_state({"state", 1, consumer_state}), rt::Status::ok);
    rt::RateDomainHandle producer_domain;
    rt::RateDomainHandle consumer_domain;
    ASSERT_EQ(producer.register_rate_domain({"rate", 5}, producer_domain), rt::Status::ok);
    ASSERT_EQ(consumer.register_rate_domain({"rate", 7}, consumer_domain), rt::Status::ok);
    ASSERT_EQ(producer.bind_phase_to_rate_domain(producer_phase, producer_domain), rt::Status::ok);
    ASSERT_EQ(consumer.bind_phase_to_rate_domain(consumer_phase, consumer_domain), rt::Status::ok);
    ASSERT_EQ(producer.finalize(), rt::Status::ok);
    ASSERT_EQ(consumer.finalize(), rt::Status::ok);
    std::size_t required = 0;
    ASSERT_EQ(producer.checkpoint_size(required), rt::Status::ok);
    std::vector<std::byte> checkpoint(required);
    rt::ArtifactWriteResult write_result;
    ASSERT_EQ(producer.write_checkpoint(0, checkpoint, write_result), rt::Status::ok);
    const auto before = consumer_state;
    EXPECT_EQ(consumer.restore_checkpoint(checkpoint), rt::Status::incompatible_artifact);
    EXPECT_EQ(consumer_state, before);
}
