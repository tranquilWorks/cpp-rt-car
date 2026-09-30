#include "golden_xdma/owner.hpp"
#include <cstdio>
int main(){golden::Options o;auto p=std::make_unique<golden::xdma::Owner>(o,golden::xdma::Dispatch::graph);auto s=p->prepare();std::printf("prepare=%d error=%.*s xdma_initializes=%llu xdma_events=%llu\n",int(s),int(p->session->runtime->last_error().size()),p->session->runtime->last_error().data(),(unsigned long long)p->driver->initializes.load(),(unsigned long long)p->driver->events.load()); if(p->close()!=rt::Status::ok)return 2;return s==rt::Status::ok?0:1;}
