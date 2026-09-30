#include "experiments.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
int main(int argc,char **argv) {
  try {
    if (argc != 4) return 2;
    golden::showcase::Experiment e;
    e.lever = argv[1]; const std::string_view value(argv[2]);
    const auto parsed = std::from_chars(value.data(),value.data() + value.size(),e.requested);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        !golden::showcase::finite_value(e.lever,e.requested)) return 2;
    const bool ok = golden::showcase::experiment(e);
    if (e.has_state) {
      std::ofstream state(std::string(argv[3])+".state.bin",std::ios::binary);
      state.write(reinterpret_cast<const char *>(e.state.data()),static_cast<std::streamsize>(e.state.size()));
      state.close();if (state.fail()) return 1;
    }
    std::ofstream f(argv[3]);
    f << "{\"schema\":1,\"lever\":\"" << e.lever << "\",\"scope\":\"" << e.scope << '"';
#define FIELD(name) f << ",\"" #name "\":" << e.name
    FIELD(requested); FIELD(effective); FIELD(runtime_id); FIELD(config_id);
    FIELD(operations); FIELD(checks); FIELD(accepted); FIELD(rejected);
    FIELD(replaced); FIELD(gaps); FIELD(bytes); FIELD(scratch); FIELD(capacity);
    FIELD(calls); FIELD(checksum); FIELD(status);
#undef FIELD
    f << ",\"has_state\":" << (e.has_state ? "true" : "false");
    f << ",\"cleanup\":" << (e.cleanup ? "true" : "false") << ",\"correct\":" << (e.correct ? "true" : "false") << "}\n";
    f.close();
    if (!ok) std::cerr << "experiment failed " << e.lever << ' ' << e.requested << " status " << e.status << '\n';
    return ok && !f.fail() ? 0 : 1;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
