// Actual Windows ETL collection + TDH decoding. No mock, skip or admin fallback.
#include "etw.hpp"
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void need(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
void win(ULONG status, const char* what) {
    if (status != ERROR_SUCCESS) throw std::runtime_error(std::string(what) + ": " + std::to_string(status));
}
class Clock final : public rt::RuntimeClock {
    std::uint64_t time_ = 1000;
public:
    std::uint64_t now_ns() noexcept override { return time_++; }
};
void ok(rt::Status s) { need(s == rt::Status::ok, rt::status_message(s)); }
std::vector<std::byte> property(EVENT_RECORD* event, const wchar_t* name) {
    PROPERTY_DATA_DESCRIPTOR d{};
    d.PropertyName = reinterpret_cast<ULONGLONG>(name);
    d.ArrayIndex = ULONG_MAX;
    ULONG size = 0;
    win(TdhGetPropertySize(event, 0, nullptr, 1, &d, &size), "TDH size");
    need(size <= 4096, "TDH property bound");
    std::vector<std::byte> value(size);
    win(TdhGetProperty(event, 0, nullptr, 1, &d, size, reinterpret_cast<PBYTE>(value.data())), "TDH property");
    return value;
}
std::uint64_t u64(EVENT_RECORD* e, const wchar_t* name) {
    const auto v = property(e, name); need(v.size() == 8, "TDH uint64 size");
    std::uint64_t result = 0; std::memcpy(&result, v.data(), 8); return result;
}
std::string text(EVENT_RECORD* e, const wchar_t* name) {
    const auto v = property(e, name); need(!v.empty() && v.back() == std::byte{0}, "TDH string terminator");
    return {reinterpret_cast<const char*>(v.data()), v.size() - 1};
}
struct Decode {
    std::map<std::string, std::uint64_t> owners;
    unsigned snapshots = 0, events = 0, metrics = 0;
    std::string failure;
};
void WINAPI consume(EVENT_RECORD* event) {
    auto& state = *static_cast<Decode*>(event->UserContext);
    if (!IsEqualGUID(event->EventHeader.ProviderId, rtfw_telemetry::Etw::provider_id())) return;
    try {
        ULONG size = 0;
        need(TdhGetEventInformation(event, 0, nullptr, nullptr, &size) == ERROR_INSUFFICIENT_BUFFER,
             "TDH native metadata size");
        std::vector<std::byte> bytes(size);
        auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(bytes.data());
        win(TdhGetEventInformation(event, 0, nullptr, info, &size), "TDH native metadata");
        need(info->DecodingSource == DecodingSourceTlg, "native TraceLogging schema");
        need(info->EventNameOffset != 0 && info->EventNameOffset < size, "event name metadata");
        const auto* name = reinterpret_cast<const wchar_t*>(bytes.data() + info->EventNameOffset);
        const auto session = text(event, L"Session");
        const auto it = state.owners.find(session);
        need(it != state.owners.end() && u64(event, L"RuntimeId") == it->second, "collected owner identity");
        need(u64(event, L"BatchSequence") == 1, "collected batch identity");
        if (std::wcscmp(name, L"Snapshot") == 0) {
            ++state.snapshots;
            need(u64(event, L"RuntimeAnchorNs") == 1000 && u64(event, L"UnixAnchorNs") == 1700000000000000000ULL,
                 "collected clock correlation");
            need(u64(event, L"RuntimeLost") == 0 && u64(event, L"QueueFull") == 0, "collected losses");
        } else if (std::wcscmp(name, L"RuntimeEvent") == 0) {
            ++state.events;
            const auto timestamp = u64(event, L"RuntimeTimestampNs");
            need(u64(event, L"UnixTimestampNs") == 1700000000000000000ULL + timestamp - 1000,
                 "collected exact timestamp");
            need(!text(event, L"Name").empty(), "collected event name");
        } else if (std::wcscmp(name, L"RuntimeMetric") == 0) {
            ++state.metrics;
            need(!text(event, L"Name").empty(), "collected metric name");
            (void)u64(event, L"Value");
        } else throw std::runtime_error("unexpected native event");
    } catch (const std::exception& e) { state.failure = e.what(); }
}
struct Session {
    struct Buffer { EVENT_TRACE_PROPERTIES properties{}; wchar_t name[128]{}; wchar_t file[1024]{}; } buffer;
    TRACEHANDLE handle = 0;
    explicit Session(const std::filesystem::path& path) {
        auto& p = buffer.properties;
        p.Wnode.BufferSize = sizeof(buffer);
        p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p.Wnode.ClientContext = 1; // ETW QPC delivery timestamps, distinct from payload clock
        p.Wnode.Guid = rtfw_telemetry::Etw::provider_id();
        p.BufferSize = 64; p.MinimumBuffers = 2; p.MaximumBuffers = 8;
        p.LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL | EVENT_TRACE_PRIVATE_LOGGER_MODE | EVENT_TRACE_PRIVATE_IN_PROC;
        p.LoggerNameOffset = offsetof(Buffer, name); p.LogFileNameOffset = offsetof(Buffer, file);
        const auto name = L"RTFW-M27-04-" + std::to_wstring(GetCurrentProcessId());
        need(name.size() < 128 && path.wstring().size() < 1024, "session path bound");
        std::copy(name.begin(), name.end(), buffer.name);
        const auto file = path.wstring(); std::copy(file.begin(), file.end(), buffer.file);
        win(StartTraceW(&handle, buffer.name, &p), "StartTrace private in-process");
    }
    ~Session() { if (handle) (void)ControlTraceW(handle, nullptr, &buffer.properties, EVENT_TRACE_CONTROL_STOP); }
    void enable(bool enabled) {
        win(EnableTraceEx2(handle, &rtfw_telemetry::Etw::provider_id(),
            enabled ? EVENT_CONTROL_CODE_ENABLE_PROVIDER : EVENT_CONTROL_CODE_DISABLE_PROVIDER,
            4, 1, 0, 0, nullptr), "EnableTraceEx2");
    }
    void stop() {
        win(ControlTraceW(handle, nullptr, &buffer.properties, EVENT_TRACE_CONTROL_STOP), "ETW stop/flush");
        handle = 0;
        need(buffer.properties.EventsLost == 0 && buffer.properties.LogBuffersLost == 0, "ETW collector loss");
    }
};
}
int main() {
    try {
        Clock clock_a, clock_b;
        rt::Runtime a(clock_a), b(clock_b);
        rt::RuntimeConfig config; config.trace_capacity = 64;
        for (auto* runtime : {&a, &b}) { ok(runtime->configure(config)); ok(runtime->finalize()); }
        rtfw_telemetry::Queue qa({"etw-owner-a", "synthetic", 1000, 1700000000000000000ULL, 7});
        rtfw_telemetry::Queue qb({"etw-owner-b", "synthetic", 1000, 1700000000000000000ULL, 7});
        ok(qa.capture(a)); ok(qb.capture(b));
        rtfw_telemetry::Etw survivor;
        need(!qa.drain_one([&](const auto& s) { return survivor.write(s); }), "disabled ETW must retain queue");
        const auto path = std::filesystem::absolute("m27-04-native-" + std::to_string(GetCurrentProcessId()) + ".etl");
        std::filesystem::remove(path);
        Session session(path); session.enable(true);
        Decode state;
        {
            rtfw_telemetry::Etw second;
            need(qa.drain_one([&](const auto& s) {
                state.owners.emplace(s.context.session, s.metrics.metadata.runtime_id);
                return second.write(s);
            }), "ETW first emission");
        }
        // Unregistering one instance must leave the other usable.
        need(qb.drain_one([&](const auto& s) {
            state.owners.emplace(s.context.session, s.metrics.metadata.runtime_id);
            return survivor.write(s);
        }), "ETW surviving instance emission");
        session.enable(false); session.stop();
        EVENT_TRACE_LOGFILEW log{};
        auto filename = path.wstring(); log.LogFileName = filename.data();
        log.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
        log.EventRecordCallback = consume; log.Context = &state;
        TRACEHANDLE reader = OpenTraceW(&log);
        need(reader != INVALID_PROCESSTRACE_HANDLE, "OpenTrace ETL");
        const auto processed = ProcessTrace(&reader, 1, nullptr, nullptr);
        const auto closed = CloseTrace(reader);
        win(processed, "ProcessTrace actual ETL"); win(closed, "CloseTrace");
        need(state.failure.empty(), state.failure.c_str());
        need(state.snapshots == 2 && state.events == 2 && state.metrics == 64, "exact native decoded event counts");
        std::cout << "PASS native Windows ETL/TDH: 2 owners, 2 snapshots, 2 Runtime events, 64 metrics, zero collector losses; "
                  << path.string() << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
