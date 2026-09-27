#include "../samples/cuda_physics/lifetime/conformance.hpp"
#include "cuda_physics/allocation_guard.hpp"
int main() {
    namespace l=rtfw::cuda_physics::lifetime;
    for(bool active:{false,true}) for(bool graph:{false,true}) {
        l::Fixture f(active,graph,4096,0xffffffffu);
        if(!f.start()) return 1;
        rtfw_physics_allocation::begin();
        const auto status=f.scenario->run();
        const auto allocations=rtfw_physics_allocation::end();
        if(status!=rt::Status::ok || allocations!=0 || !f.finish()) return 1;
    }
    return l::suite()?0:1;
}
