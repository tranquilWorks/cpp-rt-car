#include "session.hpp"
#include <rt/xdma_linux.hpp>
#include <iostream>
namespace gn=golden::xdma::native;
class Clock final : public rt::RuntimeClock {
public:
  std::uint64_t now_ns() noexcept override {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  }
};
int main(int argc,char **argv) {
  if (argc!=3 || std::string_view(argv[1])!="--prepare") {
    std::cerr<<"usage: golden_xdma_native --prepare NAMED_TUPLE_CONFIG\n";return 2;
  }
  try {
    gn::Configuration config;
    if (!gn::read_configuration(argv[2],config)) { std::cerr<<"invalid configuration\n";return 2; }
    const auto available=gn::availability(config);
    if (available==gn::Availability::invalid) { std::cerr<<"invalid endpoint configuration\n";return 2; }
    if (available==gn::Availability::absent) {
      std::cout<<"NOT_RUN missing device; no driver initialized\n";return 3;
    }
    // All validation above precedes driver construction, worker creation and
    // output submission. Endpoints are supplied by an approved bench operator.
    const std::array<std::string_view,2> h2c{config.paths[0],config.paths[1]},
        c2h{config.paths[2],config.paths[3]},events{config.paths[5],config.paths[6]};
    rt::LinuxXdmaConfig native;native.h2c_paths=h2c;native.c2h_paths=c2h;
    native.user_path=config.paths[4];native.event_paths=events;
    rt::LinuxXdmaDriver driver(native);
    Clock clock;
    auto memory=std::make_unique<golden::Memory>();
    const auto origin=clock.now_ns();
    auto io=std::make_unique<gn::Io>(driver.api(),config,origin);
    auto session=std::make_unique<gn::Session>(golden::Options{},*memory,*io,clock,origin);
    const auto prepared=session->prepare();
    // Even failed startup may own resources. Never release borrowed storage or
    // the driver until checked stop succeeds. Persistent failure terminates via
    // Session's existing fail-stop destructor contract rather than freeing it.
    auto stopped=session->close();
    for (unsigned retry=0;stopped!=rt::Status::ok && retry<2;++retry) stopped=session->close();
    if (prepared!=rt::Status::ok || stopped!=rt::Status::ok) {
      std::cerr<<"preparation failed startup="<<int(prepared)<<" stop="<<int(stopped)<<'\n';return 1;
    }
    std::cout<<"PREPARED tuple="<<config.tuple<<" bitstream="<<config.bitstream
             <<"; graph/startup/stop only; scenario execution and physical safety UNVERIFIED\n";
    return 0;
  } catch (const std::exception &e) { std::cerr<<"preparation error: "<<e.what()<<'\n';return 1; }
}
