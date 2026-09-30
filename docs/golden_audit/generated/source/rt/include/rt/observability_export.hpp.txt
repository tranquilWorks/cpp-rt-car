#pragma once

#include <iosfwd>

#include <rt/runtime.hpp>

namespace rt {

// Non-RT convenience export. The function gathers a complete bounded snapshot
// before writing, may allocate staging storage, and never runs from a frame
// callback. Cursors are committed only after the stream accepts the document.
// Interval export requires a non-null metric cursor.
[[nodiscard]] Status write_observability_json(
    Runtime& runtime,
    std::ostream& output,
    RuntimeMetricWindow window =
        RuntimeMetricWindow::cumulative,
    RuntimeMetricCursor* metric_cursor = nullptr,
    RuntimeTraceCursor* trace_cursor = nullptr) noexcept;

} // namespace rt
