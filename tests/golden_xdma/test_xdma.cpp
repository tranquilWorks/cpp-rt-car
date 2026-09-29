#include "../../samples/golden_system/oracle.hpp"
#include "../../samples/golden_xdma/session.hpp"
#include <cstdio>
int main() {
  golden::Options o;
  auto memory = std::make_unique<golden::Memory>();
  auto driver = std::make_unique<golden::xdma::SimulatedDriver>(o.count);
  auto io = std::make_unique<golden::xdma::Io>(*driver);
  auto session =
      std::make_unique<golden::xdma::Session>(o, nullptr, *memory, *io);
  auto status = session->prepare();
  std::printf("prepare=%d error=%.*s\n", static_cast<int>(status),
              static_cast<int>(session->runtime->last_error().size()),
              session->runtime->last_error().data());
  if (status != rt::Status::ok)
    return 1;
  if (!session->controls())
    return 2;
  golden::Oracle oracle(o);
  for (std::size_t t = 0; t < o.ticks; ++t) {
    status = session->step(t);
    std::printf("tick=%zu status=%d calls=%llu,%llu events=%llu safe=%llu\n", t,
                static_cast<int>(status),
                static_cast<unsigned long long>(session->world.calls[3]),
                static_cast<unsigned long long>(session->world.calls[5]),
                static_cast<unsigned long long>(driver->events.load()),
                static_cast<unsigned long long>(driver->safe_acks.load()));
    if (status != rt::Status::ok || !oracle.step(t, session->world))
      return 3;
  }
  if (session->close() != rt::Status::ok)
    return 4;
  return 0;
}
