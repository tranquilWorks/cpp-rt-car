#include "RTFWUnrealAdapters.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>

namespace
{
constexpr EAutomationTestFlags TestFlags =
    EAutomationTestFlags::EditorContext |
    EAutomationTestFlags::EngineFilter;

bool WaitForQuiescence(FRTFWUnrealJobAdapter& Adapter, double TimeoutSeconds = 5.0)
{
    const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
    while (!Adapter.IsQuiescent() && FPlatformTime::Seconds() < Deadline)
    {
        FPlatformProcess::SleepNoStats(0.0f);
    }
    return Adapter.IsQuiescent();
}

struct FJobProbe
{
    std::atomic<uint64> Calls{0};
    std::atomic<uint64> WorkerMask{0};
    std::atomic<uint64> Token{0};
    std::atomic<void*> CompletionContext{nullptr};
    std::atomic<std::byte*> Scratch{nullptr};
    std::atomic<uint64> ScratchBytes{0};
    std::atomic<bool> bGate{true};
};

void ExecuteProbe(
    void* Context,
    void* CompletionContext,
    std::uint64_t Token,
    std::uint32_t WorkerIndex)
{
    auto& Probe = *static_cast<FJobProbe*>(Context);
    while (!Probe.bGate.load(std::memory_order_acquire))
    {
        FPlatformProcess::YieldThread();
    }
    Probe.Token.store(Token, std::memory_order_relaxed);
    Probe.CompletionContext.store(CompletionContext, std::memory_order_relaxed);
    Probe.WorkerMask.fetch_or(uint64{1} << WorkerIndex, std::memory_order_relaxed);
    Probe.Calls.fetch_add(1, std::memory_order_release);
}

rt::HostExecutorJob MakeJob(FJobProbe& Probe, std::uint64_t Token)
{
    static std::byte Scratch[64]{};
    return {
        &ExecuteProbe,
        &Probe,
        &Probe,
        Token,
        Scratch,
        sizeof(Scratch),
    };
}

struct FRuntimeProbe
{
    std::atomic<uint64> Roots{0};
    std::atomic<uint64> RangeItems{0};
    std::atomic<uint64> Failures{0};
};

rt::TaskResult CountRange(
    void* Context,
    const rt::TaskContext& Task,
    const rt::TaskRange& Range)
{
    auto& Probe = *static_cast<FRuntimeProbe*>(Context);
    if (Task.worker_index() >= 2 || Task.scratch().size() != 64)
    {
        Probe.Failures.fetch_add(1, std::memory_order_relaxed);
        return rt::TaskResult::error;
    }
    Probe.RangeItems.fetch_add(Range.end - Range.begin, std::memory_order_relaxed);
    return rt::TaskResult::ok;
}

rt::CallbackResult RunRoot(
    void* Context,
    const rt::CallbackContext& Callback)
{
    auto& Probe = *static_cast<FRuntimeProbe*>(Context);
    Probe.Roots.fetch_add(1, std::memory_order_relaxed);
    return Callback.tasks.parallel_for(64, 4, &CountRange, &Probe) ==
            rt::Status::ok
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

rt::CallbackResult FailRoot(void*, const rt::CallbackContext&)
{
    return rt::CallbackResult::error;
}

rt::RuntimeConfig AdapterConfiguration(uint32 Workers, uint32 Capacity)
{
    rt::RuntimeConfig Configuration;
    Configuration.executor_policy = rt::ExecutorPolicy::host_adapter;
    Configuration.worker_count = Workers;
    Configuration.executor_queue_capacity = Capacity;
    Configuration.task_scratch_slots = Capacity;
    Configuration.task_scratch_bytes = 64;
    Configuration.trace_capacity = 64;
    return Configuration;
}

bool ConfigureAdapters(
    rt::Runtime& Runtime,
    FRTFWUnrealJobAdapter& Jobs,
    FRTFWUnrealMemoryProvider& Memory,
    const rt::RuntimeConfig& Configuration)
{
    return Runtime.configure(Configuration) == rt::Status::ok &&
        Jobs.Attach(Runtime, Configuration) == rt::Status::ok &&
        Memory.Attach(Runtime) == rt::Status::ok;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRTFWModulePassiveTest,
    "RTFW.M19_02.Module.PassiveLoad",
    TestFlags)

bool FRTFWModulePassiveTest::RunTest(const FString&)
{
    TestFalse(TEXT("runtime module does not auto-load"),
        FModuleManager::Get().IsModuleLoaded(TEXT("RTFWUnreal")));
    TestNotNull(TEXT("runtime module explicitly loads"),
        FModuleManager::Get().LoadModule(TEXT("RTFWUnreal")));
    FRTFWUnrealJobAdapter Jobs(2, 8);
    TestTrue(TEXT("explicit construction is valid"), Jobs.IsValid());
    const auto Stats = Jobs.GetStats();
    TestEqual(TEXT("module load accepted no jobs"), Stats.Accepted, uint64{0});
    TestEqual(TEXT("module load executed no jobs"), Stats.Executed, uint64{0});
    Jobs.CloseAdmission();
    TestTrue(TEXT("empty adapter is quiescent"), Jobs.IsQuiescent());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRTFWJobValidationTest,
    "RTFW.M19_02.Jobs.ConfigurationAndIsolation",
    TestFlags)

bool FRTFWJobValidationTest::RunTest(const FString&)
{
    FRTFWUnrealJobAdapter ZeroWorkers(0, 8);
    FRTFWUnrealJobAdapter ZeroCapacity(1, 0);
    FRTFWUnrealJobAdapter NonPowerOfTwo(1, 3);
    FRTFWUnrealJobAdapter TooLarge(1, 1'048'577);
    TestFalse(TEXT("zero workers rejected"), ZeroWorkers.IsValid());
    TestFalse(TEXT("zero capacity rejected"), ZeroCapacity.IsValid());
    TestFalse(TEXT("non-power-of-two rejected"), NonPowerOfTwo.IsValid());
    TestFalse(TEXT("overflow capacity rejected"), TooLarge.IsValid());

    FRTFWUnrealJobAdapter Jobs(2, 8);
    rt::Runtime First;
    rt::Runtime Second;
    auto Configuration = AdapterConfiguration(2, 8);
    TestEqual(TEXT("first configure"), First.configure(Configuration), rt::Status::ok);
    TestEqual(TEXT("second configure"), Second.configure(Configuration), rt::Status::ok);
    auto Mismatch = Configuration;
    Mismatch.worker_count = 1;
    TestEqual(TEXT("capacity mismatch rejected"),
        Jobs.Attach(First, Mismatch), rt::Status::invalid_config);
    TestEqual(TEXT("first attach"), Jobs.Attach(First, Configuration), rt::Status::ok);
    TestEqual(TEXT("double attach rejected"),
        Jobs.Attach(First, Configuration), rt::Status::invalid_state);
    TestEqual(TEXT("two-Runtime attach rejected"),
        Jobs.Attach(Second, Configuration), rt::Status::invalid_state);
    Jobs.CloseAdmission();
    TestTrue(TEXT("closed empty adapter quiescent"), Jobs.IsQuiescent());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRTFWJobExecutionTest,
    "RTFW.M19_02.Jobs.ExactOnceSaturationHelpAndRetirement",
    TestFlags)

bool FRTFWJobExecutionTest::RunTest(const FString&)
{
    FRTFWUnrealJobAdapter Jobs(2, 2);
    auto Table = Jobs.GetAdapter();
    FJobProbe First;
    FJobProbe Second;
    First.bGate.store(false, std::memory_order_release);
    Second.bGate.store(false, std::memory_order_release);
    const auto FirstJob = MakeJob(First, 11);
    const auto SecondJob = MakeJob(Second, 22);
    TestEqual(TEXT("first accepted"), Table.submit(Table.user_data, FirstJob), rt::Status::ok);
    TestEqual(TEXT("second accepted"), Table.submit(Table.user_data, SecondJob), rt::Status::ok);
    FJobProbe Rejected;
    TestEqual(TEXT("full capacity rejects without acceptance"),
        Table.submit(Table.user_data, MakeJob(Rejected, 33)),
        rt::Status::queue_full);
    TestEqual(TEXT("rejected job not invoked"), Rejected.Calls.load(), uint64{0});

    First.bGate.store(true, std::memory_order_release);
    Second.bGate.store(true, std::memory_order_release);
    TestTrue(TEXT("task nodes retire"), WaitForQuiescence(Jobs));
    TestEqual(TEXT("first exactly once"), First.Calls.load(), uint64{1});
    TestEqual(TEXT("second exactly once"), Second.Calls.load(), uint64{1});
    TestEqual(TEXT("token preserved"), First.Token.load(), uint64{11});
    TestTrue(TEXT("completion context preserved"),
        First.CompletionContext.load() == &First);
    TestTrue(TEXT("logical workers bounded"),
        (First.WorkerMask.load() | Second.WorkerMask.load()) < uint64{4});

    FJobProbe Helped;
    Helped.bGate.store(true, std::memory_order_release);
    TestEqual(TEXT("help candidate accepted"),
        Table.submit(Table.user_data, MakeJob(Helped, 44)), rt::Status::ok);
    const bool bHelpAttempt = Table.try_execute_one(Table.user_data);
    TestTrue(TEXT("help attempts at most one accepted task"),
        bHelpAttempt || Helped.Calls.load(std::memory_order_acquire) == 1);
    TestTrue(TEXT("helped node retires"), WaitForQuiescence(Jobs));
    TestEqual(TEXT("help candidate exact once"), Helped.Calls.load(), uint64{1});

    const auto BeforeStale = Helped.Calls.load();
    TestFalse(TEXT("stale dispatch rejected"), Jobs.DispatchForTesting(0, 1));
    TestEqual(TEXT("stale dispatch invokes nothing"), Helped.Calls.load(), BeforeStale);

    Jobs.RejectNextLaunchForTesting();
    FJobProbe LaunchRejected;
    TestEqual(TEXT("injected launch gate rejects"),
        Table.submit(Table.user_data, MakeJob(LaunchRejected, 55)),
        rt::Status::queue_full);
    TestEqual(TEXT("launch rejection invokes nothing"),
        LaunchRejected.Calls.load(), uint64{0});

    Jobs.CloseAdmission();
    FJobProbe Late;
    TestEqual(TEXT("closed admission rejects late submit"),
        Table.submit(Table.user_data, MakeJob(Late, 66)),
        rt::Status::invalid_state);
    TestEqual(TEXT("late submit invokes nothing"), Late.Calls.load(), uint64{0});
    TestTrue(TEXT("closed adapter quiescent"), Jobs.IsQuiescent());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRTFWGenerationWrapTest,
    "RTFW.M19_02.Jobs.GenerationWrapFailsClosed",
    TestFlags)

bool FRTFWGenerationWrapTest::RunTest(const FString&)
{
    FRTFWUnrealJobAdapter Jobs(1, 2);
    Jobs.SetNextGenerationForTesting(
        (std::numeric_limits<uint64>::max() >> 3) + 1);
    FJobProbe Probe;
    const auto Table = Jobs.GetAdapter();
    TestEqual(TEXT("generation exhaustion rejected"),
        Table.submit(Table.user_data, MakeJob(Probe, 1)),
        rt::Status::resource_exhausted);
    TestEqual(TEXT("generation exhaustion invokes nothing"), Probe.Calls.load(), uint64{0});
    TestFalse(TEXT("generation exhaustion closes admission"),
        Jobs.GetStats().bAdmissionOpen);
    TestTrue(TEXT("generation exhaustion remains quiescent"), Jobs.IsQuiescent());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRTFWMemoryLifecycleTest,
    "RTFW.M19_02.Allocator.AcquireObserveRollbackRelease",
    TestFlags)

bool FRTFWMemoryLifecycleTest::RunTest(const FString&)
{
    FRTFWUnrealClock Clock;
    FRTFWUnrealJobAdapter Jobs(2, 32);
    FRTFWUnrealMemoryProvider Memory;
    rt::Runtime Runtime(Clock);
    const auto Configuration = AdapterConfiguration(2, 32);
    TestTrue(TEXT("adapters configured"),
        ConfigureAdapters(Runtime, Jobs, Memory, Configuration));
    FRuntimeProbe Probe;
    TestEqual(TEXT("callback registered"),
        Runtime.register_callback({"unreal.memory", &RunRoot, &Probe}),
        rt::Status::ok);
    TestEqual(TEXT("provider finalizes"), Runtime.finalize(), rt::Status::ok);
    auto Stats = Memory.GetStats();
    TestEqual(TEXT("three regions acquired"), Stats.Acquired, uint64{3});
    TestEqual(TEXT("three live tokens"), Stats.LiveAllocations, uint32{3});
    TestTrue(TEXT("positive exact allocator extents"), Stats.LiveBytes > 0);
    TestEqual(TEXT("runtime starts"), Runtime.start(), rt::Status::ok);
    Stats = Memory.GetStats();
    TestEqual(TEXT("three policies applied"), Stats.Applied, uint64{3});
    TestEqual(TEXT("three extents observed"), Stats.Observed, uint64{3});
    TestEqual(TEXT("no extent mismatch"), Stats.ExtentMismatches, uint64{0});

    rt::HostFrameContext Frame{};
    TestEqual(TEXT("frame mapped"),
        FRTFWUnrealFrameContext::Make(
            Clock, 7, 1.0 / 60.0, {}, {}, Frame),
        rt::Status::ok);
    TestEqual(TEXT("host-driven nested step"), Runtime.step(Frame), rt::Status::ok);
    TestEqual(TEXT("root exact once"), Probe.Roots.load(), uint64{1});
    TestEqual(TEXT("nested range complete"), Probe.RangeItems.load(), uint64{64});
    TestEqual(TEXT("scratch and worker checks"), Probe.Failures.load(), uint64{0});

    Jobs.CloseAdmission();
    TestEqual(TEXT("checked stop"), Runtime.stop(), rt::Status::ok);
    TestTrue(TEXT("engine nodes retired"), WaitForQuiescence(Jobs));
    Stats = Memory.GetStats();
    TestEqual(TEXT("reverse rollback covers three"), Stats.RolledBack, uint64{3});
    TestEqual(TEXT("release covers three"), Stats.Released, uint64{3});
    TestEqual(TEXT("no live tokens after stop"), Stats.LiveAllocations, uint32{0});
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRTFWMemoryFailureTest,
    "RTFW.M19_02.Allocator.FailuresRollbackRetryAndUnsupportedTruth",
    TestFlags)

bool FRTFWMemoryFailureTest::RunTest(const FString&)
{
    {
        FRTFWUnrealMemoryProvider Memory;
        Memory.FailAcquireAtForTesting(1);
        rt::Runtime Runtime;
        auto Configuration = AdapterConfiguration(1, 8);
        FRTFWUnrealJobAdapter Jobs(1, 8);
        TestTrue(TEXT("partial acquisition configured"),
            ConfigureAdapters(Runtime, Jobs, Memory, Configuration));
        TestEqual(TEXT("partial acquisition fails"),
            Runtime.finalize(), rt::Status::resource_exhausted);
        auto Stats = Memory.GetStats();
        TestEqual(TEXT("completed prefix released"), Stats.Released, uint64{1});
        TestEqual(TEXT("no live prefix remains"), Stats.LiveAllocations, uint32{0});
        Jobs.CloseAdmission();
    }

    {
        FRTFWUnrealMemoryProvider Memory;
        Memory.FailApplyAtForTesting(0);
        rt::Runtime Runtime;
        auto Configuration = AdapterConfiguration(1, 8);
        FRTFWUnrealJobAdapter Jobs(1, 8);
        TestTrue(TEXT("apply failure configured"),
            ConfigureAdapters(Runtime, Jobs, Memory, Configuration));
        FRuntimeProbe Probe;
        TestEqual(TEXT("apply fixture callback registered"),
            Runtime.register_callback({"unreal.apply.failure", &RunRoot, &Probe}),
            rt::Status::ok);
        rt::CpuMemoryPolicy Policy;
        Policy.memory_policy_count = 1;
        Policy.memory_policies[0].region = rt::memory_region_phase_scratch;
        Policy.memory_policies[0].policy.requirement = rt::PolicyRequirement::strict;
        Policy.memory_policies[0].policy.provider = rt::MemoryProviderOwnership::host;
        TestEqual(TEXT("strict policy configured"),
            Runtime.set_cpu_memory_policy(Policy), rt::Status::ok);
        TestEqual(TEXT("apply fixture finalizes"), Runtime.finalize(), rt::Status::ok);
        TestEqual(TEXT("apply failure aborts start"),
            Runtime.start(), rt::Status::internal_error);
        auto Stats = Memory.GetStats();
        TestEqual(TEXT("failed start retains all tokens for retry"),
            Stats.LiveAllocations, uint32{3});
        TestTrue(TEXT("failed apply is rollback eligible"),
            Stats.RolledBack >= uint64{1});
        Memory.FailApplyAtForTesting(-1);
        TestEqual(TEXT("apply retry starts"), Runtime.start(), rt::Status::ok);
        Jobs.CloseAdmission();
        TestEqual(TEXT("apply retry stops"), Runtime.stop(), rt::Status::ok);
        TestEqual(TEXT("apply retry releases all tokens"),
            Memory.GetStats().Released, uint64{3});
    }

    {
        FRTFWUnrealMemoryProvider Memory;
        Memory.FailObserveAtForTesting(0);
        rt::Runtime Runtime;
        auto Configuration = AdapterConfiguration(1, 8);
        FRTFWUnrealJobAdapter Jobs(1, 8);
        TestTrue(TEXT("observe failure configured"),
            ConfigureAdapters(Runtime, Jobs, Memory, Configuration));
        FRuntimeProbe Probe;
        TestEqual(TEXT("observe fixture callback registered"),
            Runtime.register_callback({"unreal.observe.failure", &RunRoot, &Probe}),
            rt::Status::ok);
        rt::CpuMemoryPolicy Policy;
        Policy.memory_policy_count = 1;
        Policy.memory_policies[0].region = rt::memory_region_phase_scratch;
        Policy.memory_policies[0].policy.requirement = rt::PolicyRequirement::strict;
        Policy.memory_policies[0].policy.provider = rt::MemoryProviderOwnership::host;
        TestEqual(TEXT("observe fixture policy configured"),
            Runtime.set_cpu_memory_policy(Policy), rt::Status::ok);
        TestEqual(TEXT("observe fixture finalizes"), Runtime.finalize(), rt::Status::ok);
        TestEqual(TEXT("observe failure aborts start"),
            Runtime.start(), rt::Status::internal_error);
        const auto Stats = Memory.GetStats();
        TestEqual(TEXT("failed observation is not counted"), Stats.Observed, uint64{0});
        TestTrue(TEXT("observe failure rolls back applied policy"),
            Stats.RolledBack >= uint64{1});
        TestEqual(TEXT("observe failure retains all tokens for retry"),
            Stats.LiveAllocations, uint32{3});
        Memory.FailObserveAtForTesting(-1);
        TestEqual(TEXT("observe retry starts"), Runtime.start(), rt::Status::ok);
        Jobs.CloseAdmission();
        TestEqual(TEXT("observe retry stops"), Runtime.stop(), rt::Status::ok);
        TestEqual(TEXT("observe retry releases all tokens"),
            Memory.GetStats().Released, uint64{3});
    }

    {
        FRTFWUnrealMemoryProvider Memory;
        rt::Runtime Runtime;
        auto Configuration = AdapterConfiguration(1, 8);
        FRTFWUnrealJobAdapter Jobs(1, 8);
        TestTrue(TEXT("unsupported policy configured"),
            ConfigureAdapters(Runtime, Jobs, Memory, Configuration));
        rt::CpuMemoryPolicy Policy;
        Policy.memory_policy_count = 1;
        Policy.memory_policies[0].region = rt::memory_region_phase_scratch;
        Policy.memory_policies[0].policy.requirement = rt::PolicyRequirement::strict;
        Policy.memory_policies[0].policy.provider = rt::MemoryProviderOwnership::host;
        Policy.memory_policies[0].policy.locking = rt::PolicyToggle::enabled;
        TestEqual(TEXT("unsupported strict policy accepted for validation"),
            Runtime.set_cpu_memory_policy(Policy), rt::Status::ok);
        TestEqual(TEXT("unsupported lock fails during capability validation"),
            Runtime.finalize(), rt::Status::invalid_config);
        TestEqual(TEXT("provider callback is not entered after preflight rejection"),
            Memory.GetStats().UnsupportedPolicies, uint64{0});
        TestEqual(TEXT("unsupported failure acquires nothing"),
            Memory.GetStats().LiveAllocations, uint32{0});
        Jobs.CloseAdmission();
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRTFWRollbackRetryTest,
    "RTFW.M19_02.Allocator.UnresolvedRollbackRetriesWithoutDoubleFree",
    TestFlags)

bool FRTFWRollbackRetryTest::RunTest(const FString&)
{
    FRTFWUnrealClock Clock;
    FRTFWUnrealMemoryProvider Memory;
    FRTFWUnrealJobAdapter Jobs(1, 8);
    rt::Runtime Runtime(Clock);
    const auto Configuration = AdapterConfiguration(1, 8);
    TestTrue(TEXT("retry runtime configured"),
        ConfigureAdapters(Runtime, Jobs, Memory, Configuration));
    FRuntimeProbe Probe;
    TestEqual(TEXT("retry fixture callback registered"),
        Runtime.register_callback({"unreal.rollback.retry", &RunRoot, &Probe}),
        rt::Status::ok);
    TestEqual(TEXT("retry runtime finalizes"), Runtime.finalize(), rt::Status::ok);
    TestEqual(TEXT("retry runtime starts"), Runtime.start(), rt::Status::ok);
    Memory.FailRollbackCountForTesting(1);
    Jobs.CloseAdmission();
    TestEqual(TEXT("first checked stop retains ownership"),
        Runtime.stop(), rt::Status::internal_error);
    TestEqual(TEXT("tokens retained on unresolved rollback"),
        Memory.GetStats().Released, uint64{0});
    TestEqual(TEXT("second checked stop recovers"), Runtime.stop(), rt::Status::ok);
    const auto Stats = Memory.GetStats();
    TestEqual(TEXT("three tokens released once"), Stats.Released, uint64{3});
    TestEqual(TEXT("no live tokens"), Stats.LiveAllocations, uint32{0});
    TestTrue(TEXT("job adapter quiescent"), Jobs.IsQuiescent());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRTFWClockFrameTest,
    "RTFW.M19_02.Clock.MonotonicCheckedConversionAndFrames",
    TestFlags)

bool FRTFWClockFrameTest::RunTest(const FString&)
{
    FRTFWUnrealClock First;
    FRTFWUnrealClock Second;
    const uint64 FirstNow = First.now_ns();
    const uint64 FirstLater = First.now_ns();
    const uint64 SecondNow = Second.now_ns();
    TestTrue(TEXT("first clock nondecreasing"), FirstLater >= FirstNow);
    TestTrue(TEXT("second instance has valid independent state"), SecondNow > 0);
    TestFalse(TEXT("absolute sleep unsupported"), First.supports_absolute_sleep());
    TestEqual(TEXT("absolute sleep fails"),
        First.sleep_until_ns(FirstLater), rt::Status::clock_failure);

    uint64 Nanoseconds = 0;
    TestTrue(TEXT("half-up rounding lower boundary"),
        FRTFWUnrealClock::TrySecondsToNanoseconds(0.00000000049L, false, Nanoseconds));
    TestEqual(TEXT("rounds below half down"), Nanoseconds, uint64{0});
    TestTrue(TEXT("half-up rounding exact boundary"),
        FRTFWUnrealClock::TrySecondsToNanoseconds(0.0000000005L, false, Nanoseconds));
    TestEqual(TEXT("rounds half up"), Nanoseconds, uint64{1});
    TestFalse(TEXT("negative rejected"),
        FRTFWUnrealClock::TrySecondsToNanoseconds(-1.0L, false, Nanoseconds));
    TestFalse(TEXT("infinity rejected"),
        FRTFWUnrealClock::TrySecondsToNanoseconds(
            std::numeric_limits<long double>::infinity(), false, Nanoseconds));
    TestFalse(TEXT("overflow rejected"),
        FRTFWUnrealClock::TrySecondsToNanoseconds(
            static_cast<long double>(std::numeric_limits<uint64>::max()),
            false,
            Nanoseconds));

    rt::HostFrameContext Frame{};
    TestEqual(TEXT("finite positive frame mapped"),
        FRTFWUnrealFrameContext::Make(
            First, 42, 1.0 / 60.0, 1, 2, Frame),
        rt::Status::ok);
    TestEqual(TEXT("explicit frame sequence preserved"), Frame.frame_index, uint64{42});
    TestTrue(TEXT("rounded delta positive"), Frame.delta.count() > 0);
    TestTrue(TEXT("deadline copied in clock domain"), Frame.deadline_ns.has_value());
    TestTrue(TEXT("nominal release copied in clock domain"),
        Frame.nominal_release_ns.has_value());
    const auto Preserved = Frame;
    TestEqual(TEXT("zero delta rejected"),
        FRTFWUnrealFrameContext::Make(First, 43, 0.0, {}, {}, Frame),
        rt::Status::invalid_argument);
    TestEqual(TEXT("invalid mapping leaves output unchanged"),
        Frame.frame_index, Preserved.frame_index);
    TestEqual(TEXT("NaN delta rejected"),
        FRTFWUnrealFrameContext::Make(
            First,
            43,
            std::numeric_limits<double>::quiet_NaN(),
            {},
            {},
            Frame),
        rt::Status::invalid_argument);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRTFWLifecycleIsolationTest,
    "RTFW.M19_02.Lifecycle.HostDrivenFailureAndTwoInstances",
    TestFlags)

bool FRTFWLifecycleIsolationTest::RunTest(const FString&)
{
    FRTFWUnrealClock FirstClock;
    FRTFWUnrealClock SecondClock;
    FRTFWUnrealJobAdapter FirstJobs(2, 32);
    FRTFWUnrealJobAdapter SecondJobs(2, 32);
    FRTFWUnrealMemoryProvider FirstMemory;
    FRTFWUnrealMemoryProvider SecondMemory;
    rt::Runtime First(FirstClock);
    rt::Runtime Second(SecondClock);
    const auto Configuration = AdapterConfiguration(2, 32);
    TestTrue(TEXT("first pair configured"),
        ConfigureAdapters(First, FirstJobs, FirstMemory, Configuration));
    TestTrue(TEXT("second pair configured"),
        ConfigureAdapters(Second, SecondJobs, SecondMemory, Configuration));
    FRuntimeProbe FirstProbe;
    TestEqual(TEXT("first callback registered"),
        First.register_callback({"first", &RunRoot, &FirstProbe}), rt::Status::ok);
    TestEqual(TEXT("second failing callback registered"),
        Second.register_callback({"second", &FailRoot, nullptr}), rt::Status::ok);
    TestEqual(TEXT("first finalizes"), First.finalize(), rt::Status::ok);
    TestEqual(TEXT("second finalizes"), Second.finalize(), rt::Status::ok);
    TestEqual(TEXT("first starts"), First.start(), rt::Status::ok);
    TestEqual(TEXT("second starts"), Second.start(), rt::Status::ok);

    rt::HostFrameContext FirstFrame{};
    rt::HostFrameContext SecondFrame{};
    TestEqual(TEXT("first frame"),
        FRTFWUnrealFrameContext::Make(
            FirstClock, 1, 86'400.0, {}, {}, FirstFrame), rt::Status::ok);
    TestEqual(TEXT("second frame"),
        FRTFWUnrealFrameContext::Make(
            SecondClock, 2, 1.0 / 60.0, {}, {}, SecondFrame), rt::Status::ok);
    const double Begin = FPlatformTime::Seconds();
    TestEqual(TEXT("large host delta does not pace"), First.step(FirstFrame), rt::Status::ok);
    TestTrue(TEXT("large host delta returned promptly"),
        FPlatformTime::Seconds() - Begin < 1.0);
    TestEqual(TEXT("callback failure contained"),
        Second.step(SecondFrame), rt::Status::callback_failed);
    TestEqual(TEXT("first instance unaffected"), FirstProbe.Roots.load(), uint64{1});

    FirstJobs.CloseAdmission();
    SecondJobs.CloseAdmission();
    TestEqual(TEXT("first checked stop"), First.stop(), rt::Status::ok);
    TestEqual(TEXT("second checked stop"), Second.stop(), rt::Status::ok);
    TestTrue(TEXT("first nodes retire"), WaitForQuiescence(FirstJobs));
    TestTrue(TEXT("second nodes retire"), WaitForQuiescence(SecondJobs));
    TestEqual(TEXT("first provider released independently"),
        FirstMemory.GetStats().Released, uint64{3});
    TestEqual(TEXT("second provider released independently"),
        SecondMemory.GetStats().Released, uint64{3});
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRTFWPerformanceBudgetTest,
    "RTFW.M19_02.Performance.PreselectedFunctionalBudgets",
    TestFlags)

bool FRTFWPerformanceBudgetTest::RunTest(const FString&)
{
    // These functional regression ceilings were selected before measurement
    // for the single approved engine/host/build tuple. They are neither a
    // portable latency bound nor RT1/RT2 evidence.
    constexpr uint32 JobCount = 256;
    constexpr double MaxMeanSubmitHelpMicroseconds = 5'000.0;
    constexpr double MaxSustainedCompletionSeconds = 10.0;
    constexpr double MaxAllocatorLifecycleSeconds = 5.0;

    FRTFWUnrealJobAdapter Jobs(2, 64);
    const auto Table = Jobs.GetAdapter();
    FJobProbe Probe;
    const double JobBegin = FPlatformTime::Seconds();
    for (uint32 Index = 0; Index < JobCount; ++Index)
    {
        if (Table.submit(Table.user_data, MakeJob(Probe, Index + 1)) !=
                rt::Status::ok ||
            !WaitForQuiescence(Jobs))
        {
            AddError(TEXT("bounded job completion failed during performance sample"));
            Jobs.CloseAdmission();
            return false;
        }
    }
    const double JobSeconds = FPlatformTime::Seconds() - JobBegin;
    const double MeanMicroseconds =
        JobSeconds * 1'000'000.0 / static_cast<double>(JobCount);
    TestTrue(TEXT("preselected submit/help mean budget"),
        MeanMicroseconds <= MaxMeanSubmitHelpMicroseconds);
    TestTrue(TEXT("preselected sustained completion budget"),
        JobSeconds <= MaxSustainedCompletionSeconds);
    TestEqual(TEXT("sustained exact completion count"),
        Probe.Calls.load(), static_cast<uint64>(JobCount));
    Jobs.CloseAdmission();

    const double AllocatorBegin = FPlatformTime::Seconds();
    {
        FRTFWUnrealClock Clock;
        FRTFWUnrealJobAdapter AllocatorJobs(1, 8);
        FRTFWUnrealMemoryProvider Memory;
        rt::Runtime Runtime(Clock);
        const auto Configuration = AdapterConfiguration(1, 8);
        TestTrue(TEXT("allocator budget configuration"),
            ConfigureAdapters(Runtime, AllocatorJobs, Memory, Configuration));
        FRuntimeProbe AllocatorProbe;
        TestEqual(TEXT("allocator budget callback registered"),
            Runtime.register_callback(
                {"unreal.performance.allocator", &RunRoot, &AllocatorProbe}),
            rt::Status::ok);
        TestEqual(TEXT("allocator budget finalize"), Runtime.finalize(), rt::Status::ok);
        TestEqual(TEXT("allocator budget start"), Runtime.start(), rt::Status::ok);
        AllocatorJobs.CloseAdmission();
        TestEqual(TEXT("allocator budget stop"), Runtime.stop(), rt::Status::ok);
        TestEqual(TEXT("allocator budget release count"),
            Memory.GetStats().Released, uint64{3});
    }
    const double AllocatorSeconds = FPlatformTime::Seconds() - AllocatorBegin;
    TestTrue(TEXT("preselected allocator setup/teardown budget"),
        AllocatorSeconds <= MaxAllocatorLifecycleSeconds);
    AddInfo(FString::Printf(
        TEXT("M19-02 tuple-only samples: mean_submit_help_us=%.3f sustained_s=%.6f allocator_s=%.6f"),
        MeanMicroseconds,
        JobSeconds,
        AllocatorSeconds));
    return true;
}
