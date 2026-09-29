#define RTFW_SAMPLED_STORAGE_NO_MAIN
#include "/home/kbianco/.local/share/portfolio-control/worktrees/targets/cpp-rt-car-m24/tests/package_consumer/sampled_io_storage_consumer.cpp"
int main(){sampled_storage::Fixture f;if(f.configure(2,2)!=rt::Status::ok || f.runtime.finalize()!=rt::Status::ok)return 1;rt::MemoryPlan p;if(!f.runtime.memory_plan(p))return 2;std::printf("runtime=%zu device=%zu planned=%zu slots=%zu\n",p.runtime_control_bytes,p.device_control_bytes,p.planned_bytes,p.device_batch_queue_slots);}
