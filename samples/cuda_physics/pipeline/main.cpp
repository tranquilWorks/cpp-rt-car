#include "cli.hpp"
#include "simulated_driver.hpp"
int main(int argc,char** argv) {
    namespace p=rtfw::cuda_physics::pipeline;
    p::Options o; const auto parsed=p::parse(argc,argv,o); if (parsed) return parsed==3?0:parsed;
    try {
        auto driver=std::make_unique<p::SimulatedDriver>(o);
        auto scenario=std::make_unique<p::Scenario>(o,driver->session());
        auto s=scenario->prepare();
        if (s==rt::Status::ok && o.active && !scenario->active_plan()) s=rt::Status::internal_error;
        if (s==rt::Status::ok) s=scenario->start();
        if (s==rt::Status::ok) s=scenario->run();
        if (s!=rt::Status::ok) std::cerr<<"pipeline status="<<static_cast<int>(s)<<" "<<scenario->error()<<'\n';
        const auto stopped=scenario->stop();
        if (stopped!=rt::Status::ok && scenario->stop()!=rt::Status::ok) std::terminate();
        const auto completed=scenario->completed(); scenario.reset();
        if (s!=rt::Status::ok || stopped!=rt::Status::ok || !driver->close()) return 1;
        p::summary(o,completed,"simulated-driver-protocol"); return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
