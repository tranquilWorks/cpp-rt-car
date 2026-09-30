#include "provider.hpp"
#include <charconv>
#include <iostream>
namespace b = rtfw::benchmark;
namespace s = golden::showcase;
int main(int argc, char **argv) {
  try {
    golden::Options options;
    std::filesystem::path output;
    std::string id, peer, variant = "cpu", dispatch = "cpu";
    bool fake = false, list = false;
    for (int i = 1; i < argc; ++i) {
      const std::string_view key(argv[i]);
      if (key == "--list") { list = true; continue; }
      if (++i == argc) return 2;
      const std::string_view value(argv[i]);
      if (key == "--case") id = value;
      else if (key == "--variant") variant = value;
      else if (key == "--dispatch") dispatch = value;
      else if (key == "--output") output = argv[i];
      else if (key == "--peer-prefix") peer = value;
      else if (key == "--clock") { if (value != "fake" && value != "steady") return 2; fake = value == "fake"; }
      else if (key == "--mode") { if (value != "native" && value != "host") return 2; options.host = value == "host"; }
      else {
        std::size_t n = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), n);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return 2;
        if (key == "--count") options.count = n;
        else if (key == "--ticks") options.ticks = n;
        else if (key == "--workers") options.workers = n;
        else if (key == "--grain") options.grain = n;
        else return 2;
      }
    }
    s::Provider provider(options,output.empty() ? output : output / "invocations",peer,variant,dispatch);
    b::Runner runner; b::ProviderHandle handle;
    if (runner.register_provider(provider.table(),handle) != b::Status::ok) return 1;
    if (list) {
      for (auto name : s::case_ids) {
        b::Descriptor d;
        if (runner.describe("rtfw.golden",name,d) != b::Status::ok) return 1;
        std::cout << b::encode_descriptor("rtfw.golden",1,d,b::ClockKind::fake) << '\n';
      }
      return runner.unregister_provider(handle) == b::Status::ok ? 0 : 1;
    }
    const auto selected = std::find(s::case_ids.begin(),s::case_ids.end(),id);
    if (selected == s::case_ids.end() || (options.host && selected - s::case_ids.begin() < 7) ||
        (!peer.empty() && id != "golden-external") || output.empty() || std::filesystem::exists(output) ||
        (variant != "cpu" && id != "golden-loop") || (id == "golden-controls" && options.ticks < 19)) return 2;
    std::filesystem::create_directories(output);
    std::uint64_t clock_value = 0;
    auto clock = b::steady_clock();
    if (fake) { clock.kind = b::ClockKind::fake; clock.user = &clock_value;
      clock.read_ns = [](void *p,std::uint64_t &n) { auto &v = *static_cast<std::uint64_t *>(p); v += 100; n = v; return true; }; }
    auto identity = b::capture_identity();
    identity.backend = "cpu"; identity.driver = "none";
    const auto result = runner.run("rtfw.golden",id,clock,identity);
    const auto published = b::publish(result,output / "benchmark");
    const auto removed = runner.unregister_provider(handle);
    std::cout << b::status_name(result.status) << '\n';
    return result.status == b::Status::ok && published == b::Status::ok && removed == b::Status::ok ? 0 : 1;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
