#pragma once
#include "telemetry.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace rtfw_telemetry {
// Provider {7744f8d4-bb73-4f12-9be6-49e159811370}, keyword1, level4.
// Registration is shared safely across kit instances in this linked module.
// ETW event-header timestamps are emission time; payload timestamps are Runtime
// time and explicitly mapped Unix time. TraceLogging has no delivery ACK.
class Etw final {
public:
    Etw();
    ~Etw();
    Etw(const Etw&) = delete;
    Etw& operator=(const Etw&) = delete;
    bool write(const Snapshot&);
    static const GUID& provider_id() noexcept;
};
}
#endif
