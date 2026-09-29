#include "/tmp/cpp-m26-04-ring-probe-session.hpp"
#include <cstdio>
int main() {
 golden::Options o;
 auto memory=std::make_unique<golden::Memory>();
 auto driver=std::make_unique<golden::xdma::SimulatedDriver>(o.count);
 auto io=std::make_unique<golden::xdma::Io>(*driver);
 auto s=std::make_unique<golden::xdma::Session>(o,nullptr,*memory,*io);
 auto result=s->prepare();
 auto error=s->runtime->last_error();
 std::printf("requested_ring=%d finalize_status=%d error=%.*s initialized=%llu events=%llu\n",PROBE_RING,static_cast<int>(result),static_cast<int>(error.size()),error.data(),static_cast<unsigned long long>(driver->initializes.load()),static_cast<unsigned long long>(driver->events.load()));
 return result==rt::Status::ok ? 0 : 1;
}
