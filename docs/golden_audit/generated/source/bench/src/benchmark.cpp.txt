#include <rtfw/benchmark.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif

#ifndef RTFW_BENCH_SOURCE_COMMIT
#define RTFW_BENCH_SOURCE_COMMIT "unknown"
#define RTFW_BENCH_SOURCE_TREE "unknown"
#define RTFW_BENCH_SOURCE_DIRTY "unknown"
#define RTFW_BENCH_COMPILER "unknown"
#define RTFW_BENCH_COMPILER_VERSION "unknown"
#define RTFW_BENCH_BUILD_CONFIG "unknown"
#define RTFW_BENCH_BUILD_FLAGS "unknown"
#endif

namespace rtfw::benchmark {
namespace {
bool identifier(std::string_view text, std::size_t maximum = 64) noexcept {
    if (text.empty() || text.size() > maximum) return false;
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (!alnum(text.front())) return false;
    for (const char c : text) if (!alnum(c) && c != '.' && c != '_' && c != '-') return false;
    return text.find("..") == std::string_view::npos;
}
bool label(std::string_view text) noexcept {
    if (text.empty() || text.size() > 128 || text.front() == ' ' || text.back() == ' ') return false;
    for (const char c : text) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '_' ||
              c == '-' || c == '(' || c == ')' || c == '+')) return false;
    }
    return text.find("..") == std::string_view::npos;
}
bool hex(std::string_view value, std::size_t count) noexcept {
    return value.size() == count && std::all_of(value.begin(), value.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
[[maybe_unused]] std::string sanitize(std::string value) {
    return label(value) ? value : "not_available";
}
std::string json_quote(std::string_view s) {
    std::string out{"\""};
    for (const char character : s) {
        const auto c = static_cast<unsigned char>(character);
        if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32 || c > 126) throw std::invalid_argument("non-ASCII artifact string");
        else out += static_cast<char>(c);
    }
    return out + '"';
}
using Object = std::map<std::string, std::string>;
std::string object(const Object& fields) {
    std::string out{"{"};
    for (const auto& [key, value] : fields) {
        if (out.size() > 1) out += ',';
        out += json_quote(key) + ':' + value;
    }
    return out + '}';
}
std::string array(const std::vector<std::string>& values) {
    std::string out{"["};
    for (const auto& v : values) { if (out.size() > 1) out += ','; out += v; }
    return out + ']';
}
std::string number(std::uint64_t n) {
    if (n > max_integer) throw std::invalid_argument("integer exceeds schema bound");
    return std::to_string(n);
}
std::string known_number(std::uint64_t n) { return n ? number(n) : json_quote("not_available"); }
const char* kind_name(ClockKind k) { return k == ClockKind::fake ? "fake" : "steady"; }
const char* evidence(ClockKind k) {
    return k == ClockKind::fake ? "structural_fixture" : "portable_characterization";
}
std::string utc(ClockKind kind) {
    if (kind == ClockKind::fake) return "1970-01-01T00:00:00Z";
    const std::time_t now = std::time(nullptr);
    std::tm value{};
#ifdef _WIN32
    if (gmtime_s(&value, &now) != 0) return "not_available";
#else
    if (!gmtime_r(&now, &value)) return "not_available";
#endif
    char bytes[32]{};
    if (std::strftime(bytes, sizeof(bytes), "%Y-%m-%dT%H:%M:%SZ", &value) == 0)
        return "not_available";
    return bytes;
}
std::string identity_json(const Identity& i) {
    return object({{"architecture", json_quote(i.architecture)}, {"backend", json_quote(i.backend)},
        {"build_configuration", json_quote(i.build_configuration)}, {"build_flags_sha256", json_quote(i.build_flags_sha256)},
        {"compiler", json_quote(i.compiler)}, {"compiler_version", json_quote(i.compiler_version)},
        {"cpu_model", json_quote(i.cpu_model)}, {"driver", json_quote(i.driver)},
        {"frequency", json_quote("not_available")}, {"host_label", json_quote(i.host_label)},
        {"kernel", json_quote(i.kernel)}, {"logical_cpus", known_number(i.logical_cpus)},
        {"memory_policy", json_quote(i.memory_policy)}, {"os", json_quote(i.os)},
        {"page_size", known_number(i.page_size)}, {"policy_origin", json_quote("caller_declared_or_unavailable")},
        {"source_commit", json_quote(i.source_commit)}, {"source_dirty", json_quote(i.source_dirty)},
        {"source_tree", json_quote(i.source_tree)}, {"temperature", json_quote("not_available")},
        {"thread_policy", json_quote(i.thread_policy)}, {"total_memory_bytes", known_number(i.total_memory_bytes)}});
}
std::string descriptor_json(const Result& r) {
    const auto& d = r.descriptor;
    std::vector<std::string> params, counters;
    for (const auto& p : d.parameters) params.push_back(object({{"maximum", number(p.maximum)},
        {"minimum", number(p.minimum)}, {"name", json_quote(p.name)}, {"value", number(p.value)}}));
    for (const auto& c : d.counters) counters.push_back(object({{"maximum", number(c.maximum)},
        {"minimum", number(c.minimum)}, {"name", json_quote(c.name)}, {"unit", json_quote(c.unit)}}));
    return object({{"case_id", json_quote(d.case_id)}, {"clock", json_quote(kind_name(r.clock))},
        {"comparison_policy", json_quote("none")}, {"configuration", json_quote(d.configuration)},
        {"counters", array(counters)}, {"evidence_class", json_quote(evidence(r.clock))},
        {"implementation", json_quote(d.implementation)}, {"parameters", array(params)},
        {"provider_id", json_quote(r.provider_id)}, {"provider_version", number(r.provider_version)},
        {"raw_policy", json_quote("all_measured")}, {"repetitions", number(d.repetitions)},
        {"schema_version", "1"}, {"subsystem", json_quote(d.subsystem)}, {"unit", json_quote("ns")},
        {"warmup", number(d.warmup)}, {"workload_kind", json_quote(d.workload_kind)},
        {"workload_sha256", json_quote(d.workload_sha256)}}) + '\n';
}
bool read_clock(void*, std::uint64_t& value) {
    const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (ticks < 0) return false;
    value = static_cast<std::uint64_t>(ticks);
    return value <= max_integer;
}
bool good_observation(const Descriptor& d, const Observation& o) noexcept {
    if (!o.correct || o.checksum > max_integer || o.counters.size() != d.counters.size()) return false;
    for (std::size_t n = 0; n < o.counters.size(); ++n)
        if (o.counters[n] < d.counters[n].minimum || o.counters[n] > d.counters[n].maximum) return false;
    return true;
}
struct ActiveGuard { bool& flag; ~ActiveGuard() { flag = false; } };
}

const char* status_name(Status s) noexcept {
    switch (s) {
#define RTFW_STATUS(x) case Status::x: return #x
        RTFW_STATUS(ok); RTFW_STATUS(invalid); RTFW_STATUS(duplicate); RTFW_STATUS(capacity);
        RTFW_STATUS(not_found); RTFW_STATUS(stale); RTFW_STATUS(busy); RTFW_STATUS(provider_error);
        RTFW_STATUS(clock_error); RTFW_STATUS(invariant_failed); RTFW_STATUS(not_run);
        RTFW_STATUS(io_error); RTFW_STATUS(exists);
#undef RTFW_STATUS
    }
    return "invalid";
}
Status validate(const Descriptor& d) noexcept {
    if (d.version != schema_version || !identifier(d.case_id) || !identifier(d.subsystem) ||
        !identifier(d.implementation) || !identifier(d.configuration) || !identifier(d.workload_kind) ||
        !hex(d.workload_sha256, 64) || !d.retain_raw || d.warmup > max_warmup ||
        d.repetitions == 0 || d.repetitions > max_repetitions || d.parameters.size() > max_parameters ||
        d.counters.empty() || d.counters.size() > max_counters) return Status::invalid;
    for (std::size_t n = 0; n < d.parameters.size(); ++n) {
        const auto& p = d.parameters[n];
        if (!identifier(p.name) || p.minimum > p.value || p.value > p.maximum || p.maximum > max_integer)
            return Status::invalid;
        for (std::size_t j = 0; j < n; ++j) if (d.parameters[j].name == p.name) return Status::duplicate;
    }
    for (std::size_t n = 0; n < d.counters.size(); ++n) {
        const auto& c = d.counters[n];
        if (!identifier(c.name) || !identifier(c.unit) || c.minimum > c.maximum || c.maximum > max_integer)
            return Status::invalid;
        for (std::size_t j = 0; j < n; ++j) if (d.counters[j].name == c.name) return Status::duplicate;
    }
    return Status::ok;
}
Status validate(const Identity& i) noexcept {
    if ((i.source_commit != "unknown" && !hex(i.source_commit, 40)) ||
        (i.source_tree != "unknown" && !hex(i.source_tree, 40)) ||
        (i.build_flags_sha256 != "unknown" && !hex(i.build_flags_sha256, 64)) ||
        (i.source_dirty != "true" && i.source_dirty != "false" && i.source_dirty != "unknown") ||
        i.logical_cpus > 1048576 || i.total_memory_bytes > max_integer || i.page_size > max_integer)
        return Status::invalid;
    const std::string* fields[]{&i.compiler, &i.compiler_version, &i.build_configuration,
        &i.os, &i.kernel, &i.architecture, &i.cpu_model, &i.host_label,
        &i.thread_policy, &i.memory_policy, &i.backend, &i.driver};
    for (const auto* value : fields) if (!label(*value)) return Status::invalid;
    if (!identifier(i.host_label)) return Status::invalid;
    return Status::ok;
}
ClockV1 steady_clock() noexcept { ClockV1 c; c.read_ns = read_clock; return c; }

Status Runner::register_provider(const ProviderV1& p, ProviderHandle& handle) {
    handle = {};
    if (active_) return Status::busy;
    if (p.size != sizeof(p) || p.version != 1 || !p.id || !p.describe || !p.invoke ||
        !p.implementation_version || !p.case_count || p.case_count > max_cases || p.reserved[0] || p.reserved[1])
        return Status::invalid;
    // Borrowed C strings must be valid for inspection; scan only the declared bound.
    std::size_t length = 0;
    while (length <= 64 && p.id[length]) ++length;
    if (length > 64 || !identifier(std::string_view(p.id, length))) return Status::invalid;
    for (const auto& existing : providers_) if (existing.id == p.id) return Status::duplicate;
    std::size_t count = p.case_count;
    for (const auto& existing : providers_) count += existing.cases.size();
    if (providers_.size() >= max_providers || count > max_cases || next_generation_ == max_integer)
        return Status::capacity;
    active_ = true;
    ActiveGuard guard{active_};
    Registered candidate{p, std::string(p.id, length), {}, next_generation_};
    candidate.table.id = nullptr;  // copied string is the only retained name
    try {
        for (std::size_t n = 0; n < p.case_count; ++n) {
            Descriptor d;
            const auto status = p.describe(p.user, n, d);
            if (status != Status::ok) return Status::provider_error;
            const auto checked = validate(d);
            if (checked != Status::ok) return checked;
            std::sort(d.parameters.begin(), d.parameters.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
            // Counter order is part of the invocation ABI and remains unchanged.
            candidate.cases.push_back(std::move(d));
        }
    } catch (...) { return Status::provider_error; }
    std::sort(candidate.cases.begin(), candidate.cases.end(), [](const auto& a, const auto& b) { return a.case_id < b.case_id; });
    for (std::size_t n = 1; n < candidate.cases.size(); ++n)
        if (candidate.cases[n - 1].case_id == candidate.cases[n].case_id) return Status::duplicate;
    providers_.push_back(std::move(candidate));
    handle = {this, next_generation_++};
    return Status::ok;
}
Status Runner::unregister_provider(ProviderHandle h) noexcept {
    if (active_) return Status::busy;
    if (h.owner != this || !h.generation) return Status::stale;
    for (auto it = providers_.begin(); it != providers_.end(); ++it)
        if (it->generation == h.generation) { providers_.erase(it); return Status::ok; }
    return Status::stale;
}
std::vector<std::string> Runner::list() const {
    std::vector<std::string> result;
    for (const auto& p : providers_) for (const auto& d : p.cases) result.push_back(p.id + ":" + d.case_id);
    std::sort(result.begin(), result.end());
    return result;
}
Status Runner::describe(std::string_view provider, std::string_view id, Descriptor& out) const {
    for (const auto& p : providers_) if (p.id == provider)
        for (const auto& d : p.cases) if (d.case_id == id) { out = d; return Status::ok; }
    return Status::not_found;
}
Result Runner::run(std::string_view provider, std::string_view id, const ClockV1& c, const Identity& identity) {
    Result r;
    if (active_) { r.status = Status::busy; return r; }
    if (c.size != sizeof(c) || c.version != 1 || !c.read_ns ||
        (c.kind != ClockKind::fake && c.kind != ClockKind::steady) || validate(identity) != Status::ok) return r;
    const Registered* selected = nullptr;
    for (const auto& p : providers_) if (p.id == provider) selected = &p;
    if (!selected || describe(provider, id, r.descriptor) != Status::ok) { r.status = Status::not_found; return r; }
    r.provider_id = selected->id; r.provider_version = selected->table.implementation_version;
    r.identity = identity; r.clock = c.kind; r.start_utc = utc(c.kind);
    r.samples.reserve(r.descriptor.repetitions);
    active_ = true; ActiveGuard guard{active_};
    r.status = Status::ok;
    std::uint64_t previous = 0;
    bool first = true;
    std::uint64_t duration_sum = 0;
    std::vector<std::uint64_t> totals(r.descriptor.counters.size(), 0);
    const auto finish = [&r] { r.end_utc = utc(r.clock); return r; };
    for (std::uint64_t n = 0; n < static_cast<std::uint64_t>(r.descriptor.warmup) + r.descriptor.repetitions; ++n) {
        std::uint64_t begin{}, end{};
        try {
            if (!c.read_ns(c.user, begin) || begin > max_integer || (!first && begin < previous)) {
                r.status = Status::clock_error; r.diagnostic = "invalid start clock"; return finish();
            }
        } catch (...) { r.status = Status::clock_error; r.diagnostic = "clock callback threw"; return finish(); }
        Observation observation;
        Status invocation;
        try { invocation = selected->table.invoke(selected->table.user, r.descriptor.case_id, n, observation); }
        catch (...) { r.status = Status::provider_error; r.diagnostic = "provider callback threw"; return finish(); }
        if (invocation != Status::ok) {
            r.status = invocation == Status::not_run && n == 0 ? Status::not_run : Status::provider_error;
            r.diagnostic = r.status == Status::not_run ? "prerequisite unavailable" : "provider invocation failed";
            return finish();
        }
        try {
            if (!c.read_ns(c.user, end) || end > max_integer || end < begin) {
                r.status = Status::clock_error; r.diagnostic = "invalid end clock"; return finish();
            }
        } catch (...) { r.status = Status::clock_error; r.diagnostic = "clock callback threw"; return finish(); }
        previous = end; first = false;
        if (!good_observation(r.descriptor, observation)) {
            r.status = Status::invariant_failed; r.diagnostic = "counter or correctness invariant failed";
            return finish();
        }
        if (n < r.descriptor.warmup) { ++r.warmup_completed; continue; }
        for (std::size_t k = 0; k < totals.size(); ++k) {
            if (observation.counters[k] > max_integer - totals[k]) {
                r.status = Status::invariant_failed; r.diagnostic = "counter aggregate overflow"; return finish();
            }
            totals[k] += observation.counters[k];
        }
        if (end - begin > max_integer - duration_sum) {
            r.status = Status::invariant_failed; r.diagnostic = "duration aggregate overflow"; return finish();
        }
        duration_sum += end - begin;
        r.samples.push_back({n - r.descriptor.warmup, begin, end, std::move(observation), true});
    }
    return finish();
}

std::string sha256(std::string_view input) {
    if (input.size() > 64U * 1024U * 1024U) throw std::length_error("hash input bound");
    constexpr std::uint32_t k[64] = {
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};
    std::array<std::uint32_t,8> h{0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
        0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
    std::vector<unsigned char> bytes(input.size());
    if (!input.empty()) std::memcpy(bytes.data(),input.data(),input.size());
    const auto bits = static_cast<std::uint64_t>(bytes.size()) * 8;
    bytes.push_back(0x80);
    while (bytes.size() % 64 != 56) bytes.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) bytes.push_back(static_cast<unsigned char>(bits >> shift));
    const auto rotate = [](std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); };
    for (std::size_t offset = 0; offset < bytes.size(); offset += 64) {
        std::uint32_t w[64]{};
        for (std::size_t n = 0; n < 16; ++n)
            for (std::size_t j = 0; j < 4; ++j) w[n] = (w[n] << 8) | bytes[offset + 4*n + j];
        for (std::size_t n = 16; n < 64; ++n) {
            const auto a = rotate(w[n-15],7) ^ rotate(w[n-15],18) ^ (w[n-15] >> 3);
            const auto b = rotate(w[n-2],17) ^ rotate(w[n-2],19) ^ (w[n-2] >> 10);
            w[n] = w[n-16] + a + w[n-7] + b;
        }
        auto a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
        for (std::size_t n = 0; n < 64; ++n) {
            const auto s1 = rotate(e,6) ^ rotate(e,11) ^ rotate(e,25);
            const auto t1 = hh + s1 + ((e&f) ^ (~e&g)) + k[n] + w[n];
            const auto s0 = rotate(a,2) ^ rotate(a,13) ^ rotate(a,22);
            const auto t2 = s0 + ((a&b) ^ (a&c) ^ (b&c));
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    constexpr char digits[]="0123456789abcdef";
    std::string output;
    for (const auto value : h) for (int shift=28; shift>=0; shift-=4) output += digits[(value >> shift)&15];
    return output;
}

Identity capture_identity() {
    Identity i;
    i.source_commit=RTFW_BENCH_SOURCE_COMMIT; i.source_tree=RTFW_BENCH_SOURCE_TREE;
    i.source_dirty=RTFW_BENCH_SOURCE_DIRTY; i.compiler=RTFW_BENCH_COMPILER;
    i.compiler_version=RTFW_BENCH_COMPILER_VERSION; i.build_configuration=RTFW_BENCH_BUILD_CONFIG;
    i.build_flags_sha256=RTFW_BENCH_BUILD_FLAGS;
#ifdef _WIN32
    i.os="Windows";
    SYSTEM_INFO info{}; GetNativeSystemInfo(&info);
    i.logical_cpus=info.dwNumberOfProcessors; i.page_size=info.dwPageSize;
    i.architecture=info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x86_64" : "unknown";
    MEMORYSTATUSEX memory{}; memory.dwLength=static_cast<DWORD>(sizeof(memory));
    if (GlobalMemoryStatusEx(&memory)) i.total_memory_bytes=memory.ullTotalPhys;
#else
    struct utsname info{};
    if (uname(&info) == 0) {
        i.os=sanitize(info.sysname); i.kernel=sanitize(info.release); i.architecture=sanitize(info.machine);
        // Deliberately never inspect nodename/domainname.
    }
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    const long page = sysconf(_SC_PAGESIZE);
    if (cpus > 0) i.logical_cpus=static_cast<std::uint64_t>(cpus);
    if (page > 0) i.page_size=static_cast<std::uint64_t>(page);
#ifdef _SC_PHYS_PAGES
    const long pages = sysconf(_SC_PHYS_PAGES);
    if (pages > 0 && i.page_size && static_cast<std::uint64_t>(pages) <= max_integer/i.page_size)
        i.total_memory_bytes=static_cast<std::uint64_t>(pages)*i.page_size;
#endif
#ifdef __linux__
    // Read one fixed allowlisted field, not CPU serials, IDs, or environment.
    std::ifstream cpu("/proc/cpuinfo");
    char bounded_line[512]{};
    for (unsigned n=0; n<256 && cpu.getline(bounded_line,sizeof(bounded_line)); ++n) {
        std::string line(bounded_line);
        if (line.rfind("model name",0) != 0) continue;
        const auto colon=line.find(':');
        if (colon != std::string::npos) {
            const auto begin=line.find_first_not_of(" \t",colon+1);
            if (begin != std::string::npos) {
                auto model=line.substr(begin);
                const auto at=model.find('@');
                if (at!=std::string::npos) model.replace(at,1,"at");
                i.cpu_model=sanitize(model);
            }
        }
        break;
    }
#endif
#endif
    return i;
}

std::string encode_descriptor(std::string_view provider, std::uint32_t provider_version,
                              const Descriptor& d, ClockKind clock) {
    if (!identifier(provider) || !provider_version || validate(d) != Status::ok ||
        (clock != ClockKind::fake && clock != ClockKind::steady)) throw std::invalid_argument("invalid descriptor");
    Result r; r.descriptor=d; r.provider_id=provider; r.provider_version=provider_version; r.clock=clock;
    std::sort(r.descriptor.parameters.begin(),r.descriptor.parameters.end(),
        [](const auto& a,const auto& b) { return a.name<b.name; });
    return descriptor_json(r);
}
Artifacts encode(const Result& r) {
    const auto invalid = [] { throw std::invalid_argument("invalid benchmark result"); };
    if (validate(r.identity) != Status::ok || r.warmup_completed > r.descriptor.warmup ||
        r.samples.size() > r.descriptor.repetitions || r.diagnostic.size() > 128) invalid();
    const auto utc_ok = [](std::string_view s) {
        if (s == "not_available") return true;
        if (s.size()!=20 || s[4]!='-' || s[7]!='-' || s[10]!='T' || s[13]!=':' || s[16]!=':' || s[19]!='Z') return false;
        for (std::size_t n=0; n<s.size(); ++n) {
            if (n==4 || n==7 || n==10 || n==13 || n==16 || n==19) continue;
            if (s[n]<'0' || s[n]>'9') return false;
        }
        const auto two=[s](std::size_t n) { return (s[n]-'0')*10+s[n+1]-'0'; };
        const int year=two(0)*100+two(2), month=two(5), day=two(8);
        const int days[]{31,28,31,30,31,30,31,31,30,31,30,31};
        if (year<1 || month<1 || month>12 || day<1 || two(11)>23 || two(14)>59 || two(17)>59) return false;
        const bool leap=year%4==0 && (year%100!=0 || year%400==0);
        return day<=days[month-1]+(month==2 && leap ? 1 : 0);
    };
    if (!utc_ok(r.start_utc) || !utc_ok(r.end_utc) ||
        (r.start_utc!="not_available" && r.end_utc!="not_available" && r.end_utc < r.start_utc)) invalid();
    if (r.clock==ClockKind::fake && (r.start_utc!="1970-01-01T00:00:00Z" || r.end_utc!=r.start_utc)) invalid();
    if (r.status==Status::ok) {
        if (r.warmup_completed!=r.descriptor.warmup || r.samples.size()!=r.descriptor.repetitions || !r.diagnostic.empty()) invalid();
    } else if (r.status==Status::not_run) {
        if (r.warmup_completed || !r.samples.empty() || r.diagnostic!="prerequisite unavailable") invalid();
    } else if (r.status!=Status::provider_error && r.status!=Status::clock_error && r.status!=Status::invariant_failed) invalid();
    if (r.status!=Status::ok && (r.samples.size()>=r.descriptor.repetitions || r.diagnostic.empty())) invalid();
    if (r.status==Status::provider_error && r.diagnostic!="provider callback threw" &&
        r.diagnostic!="provider invocation failed") invalid();
    if (r.status==Status::clock_error && r.diagnostic!="invalid start clock" &&
        r.diagnostic!="invalid end clock" && r.diagnostic!="clock callback threw") invalid();
    if (r.status==Status::invariant_failed && r.diagnostic!="counter or correctness invariant failed" &&
        r.diagnostic!="counter aggregate overflow" && r.diagnostic!="duration aggregate overflow") invalid();
    if (!r.samples.empty() && r.warmup_completed!=r.descriptor.warmup) invalid();
    Artifacts artifacts;
    artifacts.descriptor=encode_descriptor(r.provider_id,r.provider_version,r.descriptor,r.clock);
    const auto descriptor_hash=sha256(artifacts.descriptor);
    const auto identity=identity_json(r.identity);
    const auto context=sha256(object({{"descriptor_sha256",json_quote(descriptor_hash)},
        {"identity",identity},{"start_utc",json_quote(r.start_utc)}}));
    std::vector<std::string> samples;
    std::vector<std::uint64_t> durations, totals(r.descriptor.counters.size(),0);
    std::uint64_t previous=0, total_ns=0;
    for (std::size_t n=0; n<r.samples.size(); ++n) {
        const auto& s=r.samples[n];
        if (s.index!=n || s.start_ns < previous || s.end_ns < s.start_ns || s.end_ns>max_integer ||
            !s.invariants_passed || !good_observation(r.descriptor,s.observation)) invalid();
        previous=s.end_ns;
        const auto duration=s.end_ns-s.start_ns;
        if (duration>max_integer-total_ns) invalid();
        total_ns+=duration; durations.push_back(duration);
        Object counters;
        for (std::size_t c=0; c<totals.size(); ++c) {
            if (s.observation.counters[c]>max_integer-totals[c]) invalid();
            totals[c]+=s.observation.counters[c];
            counters.emplace(r.descriptor.counters[c].name,number(s.observation.counters[c]));
        }
        samples.push_back(object({{"checksum",number(s.observation.checksum)}, {"counters",object(counters)},
            {"duration_ns",number(duration)}, {"end_ns",number(s.end_ns)}, {"index",number(s.index)},
            {"invariants_passed","true"}, {"start_ns",number(s.start_ns)}}));
    }
    artifacts.raw=object({{"descriptor_sha256",json_quote(descriptor_hash)}, {"run_context_sha256",json_quote(context)},
        {"samples",array(samples)}, {"schema_version","1"}})+'\n';
    std::string statistics="null";
    if (r.status==Status::ok) {
        std::sort(durations.begin(),durations.end());
        const auto percentile=[&durations](std::size_t percent) { return durations[(percent*durations.size()+99)/100-1]; };
        Object counters;
        for (std::size_t n=0; n<totals.size(); ++n) counters.emplace(r.descriptor.counters[n].name,number(totals[n]));
        statistics=object({{"counter_totals",object(counters)}, {"max_ns",number(durations.back())},
            {"min_ns",number(durations.front())}, {"p50_ns",number(percentile(50))},
            {"p95_ns",number(percentile(95))}, {"p99_ns",number(percentile(99))},
            {"percentile_method",json_quote("nearest_rank")}, {"total_ns",number(total_ns)}});
    }
    Object summary{{"clock",json_quote(kind_name(r.clock))}, {"descriptor_file",json_quote("descriptor.json")},
        {"descriptor_sha256",json_quote(descriptor_hash)}, {"diagnostic",json_quote(r.diagnostic)},
        {"end_utc",json_quote(r.end_utc)}, {"evidence_class",json_quote(evidence(r.clock))},
        {"identity",identity}, {"measured_completed",number(r.samples.size())},
        {"qualification",json_quote("none")}, {"raw_file",json_quote("raw.json")},
        {"raw_sha256",json_quote(sha256(artifacts.raw))}, {"run_context_sha256",json_quote(context)},
        {"runner_version",json_quote("1")}, {"schema_version","1"}, {"start_utc",json_quote(r.start_utc)},
        {"statistics",statistics}, {"status",json_quote(status_name(r.status))},
        {"warmup_completed",number(r.warmup_completed)}, {"workload_sha256",json_quote(r.descriptor.workload_sha256)}};
    summary.emplace("result_sha256",json_quote(sha256(object(summary)+'\n')));
    artifacts.summary=object(summary)+'\n';
    for (const auto* text : {&artifacts.descriptor,&artifacts.raw,&artifacts.summary})
        if (text->size()>32U*1024U*1024U) throw std::length_error("artifact extent");
    return artifacts;
}

namespace {
constexpr const char* files[]{"descriptor.json","raw.json","result.json"};
bool safe_path(const std::filesystem::path& output) {
    if (output.empty() || !identifier(output.filename().string())) return false;
    for (const auto& part : output) if (part=="..") return false;
#ifdef _WIN32
    // UNC and device paths can contact a network or bypass normal path rules.
    const auto native=output.native();
    const auto slash=[](wchar_t c) { return c==L'/' || c==L'\\'; };
    if ((native.size()>1 && slash(native[0]) && slash(native[1])) ||
        native.find(L':',2)!=std::wstring::npos ||
        (output.has_root_name() && !output.is_absolute())) return false;
    auto name=output.filename().string();
    if (name.back()=='.') return false;
    name=name.substr(0,name.find('.'));
    for (auto& c:name) if (c>='a' && c<='z') c=static_cast<char>(c-'a'+'A');
    if (name=="CON" || name=="PRN" || name=="AUX" || name=="NUL" ||
        (name.size()==4 && (name.substr(0,3)=="COM" || name.substr(0,3)=="LPT") &&
         name[3]>='0' && name[3]<='9')) return false;
#endif
    return true;
}
std::string temporary_name() {
    std::random_device random;
    std::string result=".rtfw-bench-";
    constexpr char digits[]="0123456789abcdef";
    for (unsigned n=0; n<24; ++n) result+=digits[random()&15U];
    return result;
}
#ifndef _WIN32
struct Fd {
    int value{-1};
    explicit Fd(int fd=-1):value(fd) {}
    ~Fd() { if (value>=0) ::close(value); }
    Fd(const Fd&)=delete; Fd& operator=(const Fd&)=delete;
};
int parent_fd(const std::filesystem::path& output) {
    Fd current(::open(output.is_absolute()?"/":".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    if (current.value<0) return -1;
    for (const auto& part : output.parent_path().relative_path()) {
        if (part.empty() || part==".") continue;
        const int next=::openat(current.value,part.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if (next<0) return -1;
        ::close(current.value); current.value=next;
    }
    const int result=current.value; current.value=-1; return result;
}
bool write_file(int directory,const char* name,std::string_view content) {
    Fd file(::openat(directory,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
    if (file.value<0) return false;
    std::size_t offset=0;
    unsigned interrupts=0;
    while (offset<content.size()) {
        const auto written=::write(file.value,content.data()+offset,content.size()-offset);
        if (written<0 && errno==EINTR && ++interrupts<=32) continue;
        if (written<=0) return false;
        offset+=static_cast<std::size_t>(written);
    }
    return ::fsync(file.value)==0;
}
#else
struct WinHandles {
    std::vector<HANDLE> handles;
    ~WinHandles() { for (const auto h:handles) if (h!=INVALID_HANDLE_VALUE) CloseHandle(h); }
};
bool lock_parents(const std::filesystem::path& output,WinHandles& locks) {
    auto parent=std::filesystem::absolute(output).parent_path();
    auto current=parent.root_path();
    for (const auto& part:parent.relative_path()) {
        current/=part;
        const HANDLE h=CreateFileW(current.c_str(),FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if (h==INVALID_HANDLE_VALUE) return false;
        locks.handles.push_back(h);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
            !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) return false;
    }
    return true;
}
bool write_file(const std::filesystem::path& name,std::string_view content) {
    const HANDLE file=CreateFileW(name.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    std::size_t offset=0;
    bool good=true;
    while (offset<content.size()) {
        DWORD written=0;
        if (!WriteFile(file,content.data()+offset,static_cast<DWORD>(content.size()-offset),&written,nullptr) || !written) {
            good=false; break;
        }
        offset+=written;
    }
    if (good) good=FlushFileBuffers(file)!=0;
    return CloseHandle(file)!=0 && good;
}
#endif
}

Status check_destination(const std::filesystem::path& output) noexcept {
    try {
        if (!safe_path(output)) return Status::invalid;
#ifdef _WIN32
        WinHandles parents;
        if (!lock_parents(output,parents)) return Status::io_error;
        const DWORD attributes=GetFileAttributesW(output.c_str());
        if (attributes!=INVALID_FILE_ATTRIBUTES) return Status::exists;
        return GetLastError()==ERROR_FILE_NOT_FOUND ? Status::ok : Status::io_error;
#else
        Fd parent(parent_fd(output));
        if (parent.value<0) return Status::io_error;
        struct stat info{};
        if (::fstatat(parent.value,output.filename().c_str(),&info,AT_SYMLINK_NOFOLLOW)==0) return Status::exists;
        return errno==ENOENT ? Status::ok : Status::io_error;
#endif
    } catch (...) { return Status::invalid; }
}
Status publish(const Result& result,const std::filesystem::path& output) {
    const auto checked=check_destination(output);
    if (checked!=Status::ok) return checked;
    Artifacts a;
    try { a=encode(result); } catch (...) { return Status::invalid; }
    const std::string* content[]{&a.descriptor,&a.raw,&a.summary};
    const auto temporary=temporary_name();
    [[maybe_unused]] const auto leaf=output.filename();
#ifdef _WIN32
    WinHandles parents;
    if (!lock_parents(output,parents)) return Status::io_error;
    const auto temporary_path=output.parent_path()/temporary;
    const std::filesystem::path paths[]{temporary_path/files[0],temporary_path/files[1],temporary_path/files[2]};
    const auto target=std::filesystem::absolute(output).native();
    if (target.size()>32767) return Status::invalid;
    const auto extent=sizeof(FILE_RENAME_INFO)+target.size()*sizeof(wchar_t);
    std::vector<std::max_align_t> storage((extent+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
    auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    std::memset(rename,0,extent);
    rename->FileNameLength=static_cast<DWORD>(target.size()*sizeof(wchar_t));
    std::memcpy(rename->FileName,target.data(),target.size()*sizeof(wchar_t));
    if (!CreateDirectoryW(temporary_path.c_str(),nullptr)) return Status::io_error;
    const HANDLE directory=CreateFileW(temporary_path.c_str(),DELETE|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (directory==INVALID_HANDLE_VALUE) { RemoveDirectoryW(temporary_path.c_str()); return Status::io_error; }
    bool good=true;
    for (std::size_t n=0; n<3 && good; ++n) good=write_file(paths[n],*content[n]);
    Status status=Status::io_error;
    if (good) {
        if (SetFileInformationByHandle(directory,FileRenameInfo,rename,static_cast<DWORD>(extent))) status=Status::ok;
        else { const DWORD error=GetLastError();
            if (error==ERROR_ALREADY_EXISTS || error==ERROR_FILE_EXISTS) status=Status::exists; }
    }
    CloseHandle(directory);
    if (status!=Status::ok) {
        for (const auto& path:paths) DeleteFileW(path.c_str());
        RemoveDirectoryW(temporary_path.c_str());
    }
    return status;
#elif defined(__linux__) && defined(SYS_renameat2)
    Fd parent(parent_fd(output));
    if (parent.value<0 || ::mkdirat(parent.value,temporary.c_str(),0700)!=0) return Status::io_error;
    Fd directory(::openat(parent.value,temporary.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    const auto cleanup=[&] {
        if (directory.value>=0) for (const auto* name:files) ::unlinkat(directory.value,name,0);
        ::unlinkat(parent.value,temporary.c_str(),AT_REMOVEDIR);
    };
    if (directory.value<0) { cleanup(); return Status::io_error; }
    for (std::size_t n=0; n<3; ++n) if (!write_file(directory.value,files[n],*content[n])) {
        cleanup(); return Status::io_error;
    }
    if (::fsync(directory.value)!=0) { cleanup(); return Status::io_error; }
    // RENAME_NOREPLACE = 1. Never replace an existing empty directory, even in a race.
    if (::syscall(SYS_renameat2,parent.value,temporary.c_str(),parent.value,leaf.c_str(),1U)!=0) {
        const auto status=errno==EEXIST ? Status::exists : Status::io_error;
        cleanup(); return status;
    }
    return Status::ok;
#else
    // A check followed by POSIX rename is not create-new atomic publication.
    return Status::io_error;
#endif
}
}  // namespace rtfw::benchmark
