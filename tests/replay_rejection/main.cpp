#include "scenario.hpp"
#include <iostream>
#include <new>
namespace rtfw_physics_allocation { void begin() noexcept; std::size_t end() noexcept; }
namespace a = rtfw_physics_allocation;
using namespace replay_rejection;

void active_refusal(bool lossless, unsigned destination_kind, bool forge_outer) {
    Scenario source(true, lossless), destination(true, lossless), control(true, lossless);
    const auto source_initial = source.checkpoint(0);
    source.step(1); source.step(2);
    const auto middle = source.checkpoint(2);
    source.step(3); source.step(4);
    auto artifact = source.capture(source_initial, 1, 4);
    if (forge_outer) {
        rt::LiveControlActionMetadata identity;
        destination.check(destination.runtime.live_control_action_metadata(identity), "owner identity");
        forge_outer_owner(artifact, identity.runtime_id);
    }
    const auto source_state = source.state;
    if (destination_kind == 1) {
        destination.check(destination.runtime.restore_checkpoint(middle), "fresh restore");
        control.check(control.runtime.restore_checkpoint(middle), "control restore");
    }
    if (destination_kind == 2) { destination.step(1); destination.step(2); control.step(1); control.step(2); }
    const std::uint64_t frame = destination_kind == 0 ? 0 : 2;
    auto initial = destination.checkpoint(frame);
    require(initial == control.checkpoint(frame), "paired checkpoint baseline differs");
    const auto state_before = destination.state;
    const auto calls_before = destination.calls;
    rt::LiveControlMailboxInfo info_before;
    require(destination.runtime.live_control_mailbox_info(1, info_before), "mailbox before");
    rt::LiveControlActionMetadata actions_before;
    destination.check(destination.runtime.live_control_action_metadata(actions_before), "actions before");
    rt::LiveControlActionCursor cursor;
    std::array<rt::LiveControlActionRecord, 256> records{};
    rt::LiveControlActionReadResult read;
    destination.check(destination.runtime.read_live_control_actions(cursor, records, read), "drain before");
    const auto cursor_before = cursor.next_sequence;
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        rt::LiveControlReplayResult result;
        a::begin();
        const auto status = destination.runtime.replay_live_control(artifact, Scenario::input, &destination, &result);
        const auto allocations = a::end();
        require(allocations == 0, "foreign rejection allocated");
        require(result.frames_replayed == 0 && destination.input_calls == 0 && destination.calls == calls_before,
                "foreign rejection invoked callbacks");
        require(destination.state == state_before && source.state == source_state, "foreign rejection changed application state");
        rt::LiveControlMailboxInfo info_after;
        require(destination.runtime.live_control_mailbox_info(1, info_after), "mailbox after");
        require(mailbox_fields(info_after) == mailbox_fields(info_before), "foreign rejection changed mailbox accounting");
        rt::LiveControlActionMetadata actions_after;
        destination.check(destination.runtime.live_control_action_metadata(actions_after), "actions after");
        require(action_fields(actions_after) == action_fields(actions_before), "foreign rejection changed action/correlation metadata");
        destination.check(destination.runtime.read_live_control_actions(cursor, records, read), "read after");
        require(cursor.next_sequence == cursor_before && read.records_read == 0 && read.lost_records == 0,
                "foreign rejection changed action cursor");
        require(status == rt::Status::incompatible_artifact, "foreign active status");
    }
    // Export itself emits checkpoint correlation; compare to an untouched paired
    // owner with identical history, rather than mistaking export for a pure read.
    initial = destination.checkpoint(frame);
    require(initial == control.checkpoint(frame), "foreign rejection changed checkpoint bytes");
    // Public live admission, execution and capture must still work after refusal.
    for (auto f = frame + 1; f <= 4; ++f) destination.step(f);
    const auto expected = destination.state;
    const auto recovery = destination.capture(initial, frame + 1, 4);
    rt::LiveControlReplayResult replay;
    a::begin();
    const auto status = destination.runtime.replay_live_control(recovery, Scenario::input, &destination, &replay);
    const auto allocations = a::end();
    destination.check(status, "own recovery replay");
    require(allocations == 0 && replay.frames_replayed == 4 - frame && destination.state == expected,
            "own replay did not preserve state/counts/allocation");
    // The original owner remains independently usable as well.
    const auto own = source.capture(source_initial, 1, 4);
    source.check(source.runtime.replay_live_control(own, Scenario::input, &source), "source replay");
    require(source.state == source_state, "source state changed after originating replay");
    destination.check(destination.runtime.stop(), "destination stop");
    source.check(source.runtime.stop(), "source stop");
}
void ordinary_cross_owner(bool lossless) {
    Scenario source(false, lossless), destination(false, lossless);
    const auto initial = source.checkpoint(0);
    for (std::uint64_t f = 1; f <= 4; ++f) source.step(f);
    const auto artifact = source.capture(initial, 1, 4);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        rt::LiveControlReplayResult result;
        a::begin();
        const auto status = destination.runtime.replay_live_control(artifact, Scenario::input, &destination, &result);
        const auto allocations = a::end();
        destination.check(status, "compatible ordinary cross-owner replay");
        require(allocations == 0 && result.frames_replayed == 4 && destination.state == source.state,
                "ordinary cross-owner compatibility/state/counts/allocation");
    }
}
int main() {
    // Runtime graph owners and live-control owners are separate counters. Create
    // an owner without mailboxes so equal numeric IDs cannot hide a mixed check.
    Clock clock;
    rt::Runtime unrelated(clock);
    require(unrelated.configure({}) == rt::Status::ok, "unrelated configure");
    rt::PhaseHandle phase;
    require(unrelated.register_callback({"unrelated", [](void*, const rt::CallbackContext&) {
        return rt::CallbackResult::ok;
    }, nullptr}, phase) == rt::Status::ok, "unrelated callback");
    require(unrelated.finalize() == rt::Status::ok, "unrelated finalize");
    a::begin();
    void* ordinary = ::operator new(32);
    void* aligned = ::operator new(64, std::align_val_t{64});
    const auto positive = a::end();
    ::operator delete(ordinary); ::operator delete(aligned, std::align_val_t{64});
    if (positive < 2) { std::cerr << "allocation positive control failed\n"; return 1; }
    unsigned failed = 0;
    for (bool lossless : {false,true}) {
        for (unsigned kind = 0; kind != 3; ++kind) for (bool forged : {false,true}) {
            try { active_refusal(lossless,kind,forged); std::cout << "PASS active v" << (lossless?2:1) << " owner " << kind << " forged-outer=" << forged << '\n'; }
            catch (const std::exception& e) { ++failed; std::cerr << "FAIL active v" << (lossless?2:1) << " owner " << kind << " forged-outer=" << forged << ": " << e.what() << '\n'; }
        }
        try { ordinary_cross_owner(lossless); std::cout << "PASS ordinary v" << (lossless?2:1) << " compatible cross-owner\n"; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL ordinary v" << (lossless?2:1) << ": " << e.what() << '\n'; }
    }
    return failed ? 1 : 0;
}
