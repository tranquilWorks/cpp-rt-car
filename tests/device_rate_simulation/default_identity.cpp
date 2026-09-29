// Compile this unchanged source against the baseline and repaired public SDKs.
// It intentionally does not name or opt into the additive simulation policy.
#include "fixture.hpp"
#include <fstream>
#include <iostream>
int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  simulation_test::Fixture f;
  if (f.configure() != rt::Status::ok ||
      f.runtime.bind_device_phase_to_rate_domain(f.binding()) !=
          rt::Status::ok ||
      f.runtime.finalize() != rt::Status::ok ||
      f.runtime.start() != rt::Status::ok) {
    std::cerr << f.runtime.last_error() << '\n';
    return 2;
  }
  std::size_t n = 0;
  rt::ArtifactWriteResult out;
  rt::ObservabilityMetadata metadata;
  if (f.runtime.observability_metadata(metadata) != rt::Status::ok ||
      f.runtime.checkpoint_size(n) != rt::Status::ok)
    return 3;
  std::vector<std::byte> bytes(n);
  if (f.runtime.write_checkpoint(0, bytes, out) != rt::Status::ok)
    return 4;
  std::ofstream output(argv[1], std::ios::binary);
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(out.bytes_written));
  std::cout << "default config_id=" << metadata.config_id
            << " checkpoint_bytes=" << out.bytes_written << '\n';
  return output && f.runtime.stop() == rt::Status::ok ? 0 : 5;
}
