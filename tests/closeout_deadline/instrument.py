#!/usr/bin/env python3
"""Generate one hash-bound offline diagnostic translation unit; never edit production."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path

SOURCE_SHA256 = "4d7b9007065374fee349b5ce40c5c0ac9d80623fa1eb290f25aeac4a582fe920"

def generate(root: Path, output: Path) -> None:
    source = root / "rt/src/device_manager.cpp"
    raw = source.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SOURCE_SHA256:
        raise SystemExit("Unreviewed DeviceManager source; diagnostic patch refused")
    if output.resolve().is_relative_to(root.resolve()):
        raise SystemExit("Diagnostic output must be outside the source checkout")
    output.mkdir(parents=True, exist_ok=False)
    before = raw.decode()
    text = '#include "trace.hpp" // M28-04 OFFLINE DIAGNOSTIC COPY ONLY\n' + before
    edits = [
        ('    slot->state.store(kBatchQueued, std::memory_order_release);',
         '    deadline_trace::record(requested.timeout_ns == 500000 ? "frame_queued" : "queued", batch_id, slot->deadline_ns.load(std::memory_order_relaxed), kBatchQueued, 0);\n'),
        ('        HalV2Status callback_status = HalV2Status::internal_error;\n        try {\n            callback_status = extension.submit(',
         '        const auto trace_id = selected->batch.batch_id;\n        const auto trace_deadline = selected->deadline_ns.load(std::memory_order_relaxed);\n        deadline_trace::before_submit(trace_id, trace_deadline, selected->batch.timeout_ns);\n'),
        ('        const auto status = hal_v2_status_to_runtime(callback_status);\n        if (status != Status::ok) {\n            auto prior = kBatchSubmitting;',
         '        deadline_trace::record("submit_exit", trace_id, trace_deadline, -1, static_cast<int>(callback_status));\n'),
        ('void DeviceManager::process_batch_completion(\n    std::size_t backend_index,\n    const HalV2BatchCompletion& completion) noexcept {\n',
         '    deadline_trace::record("completion", completion.batch_id, 0, -1, completion.status);\n'),
        ('    // This caller owns the slot. Validate simulator deadlines before either',
         '    deadline_trace::record("finish", slot.batch.batch_id, slot.deadline_ns.load(std::memory_order_relaxed), static_cast<int>(slot.state.load(std::memory_order_relaxed)), static_cast<int>(status));\n'),
        ('    completions_.fetch_add(1, std::memory_order_relaxed);\n    if (status != Status::ok) {\n        record_failure(status);\n    }\n    emit(DeviceEvent{DeviceEventKind::completed, status, slot.backend_index,\n                     slot.phase_index, slot.frame_index, slot.batch.batch_id,\n                     0});\n    slot.graph_released.store(true, std::memory_order_release);',
         '    deadline_trace::record("quarantine", slot.batch.batch_id, slot.deadline_ns.load(std::memory_order_relaxed), static_cast<int>(slot.state.load(std::memory_order_relaxed)), static_cast<int>(status));\n'),
        ('                    if (state == kBatchQueued) {\n                        // No vendor callback owns an unsent batch,',
         '                    deadline_trace::record("expired", slot.batch.batch_id, slot.deadline_ns.load(std::memory_order_relaxed), static_cast<int>(state), static_cast<int>(Status::device_timeout));\n'),
        ('        if (previous != kBatchFree && previous != kBatchReserved) {\n            auto count = outstanding_count_.load(std::memory_order_relaxed);',
         '        deadline_trace::record("shutdown_release", slot.batch.batch_id, slot.deadline_ns.load(std::memory_order_relaxed), static_cast<int>(previous), 0);\n'),
    ]
    for anchor, addition in edits:
        if text.count(anchor) != 1:
            raise SystemExit(f"Nonunique diagnostic anchor: {anchor!r}")
        text = text.replace(anchor, anchor + addition if anchor.startswith("void DeviceManager::") else addition + anchor)
    (output / "device_manager.cpp").write_text(text)
    (output / "diagnostic.patch").write_text(''.join(difflib.unified_diff(
        before.splitlines(True), text.splitlines(True), fromfile=str(source),
        tofile="OFFLINE/device_manager.cpp")))
    (output / "source-binding.json").write_text(json.dumps({
        "source": str(source), "source_sha256": SOURCE_SHA256,
        "diagnostic_sha256": hashlib.sha256(text.encode()).hexdigest(),
        "hook_sha256": hashlib.sha256((root / "tests/closeout_deadline/trace.hpp").read_bytes()).hexdigest(),
        "original_probe_sha256": hashlib.sha256((root / "tests/golden_xdma_native/test_native.cpp").read_bytes()).hexdigest(),
        "shipped": False, "deadlines_or_assertions_changed": False,
    }, indent=2) + '\n')

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.root, args.output)
