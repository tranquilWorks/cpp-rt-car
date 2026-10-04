#include "sampled.hpp"
#include <string_view>
int main(int argc,char** argv) {
 if(argc!=2) return 2;
 const std::string_view mode=argv[1];
 schedule_control::delay_gate=mode=="gate";
 const unsigned id=mode=="gate"?0u:(mode=="native"?5u:4u);
 const auto begin=std::chrono::steady_clock::now();
 const bool original=sampled_simulation::run_case(id);
 const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count();
 const auto expected=mode=="gate"?rt::Status::device_timeout:rt::Status::invalid_state;
 std::printf("CONTROL original_assertion=%d status=%d expected_control=%d elapsed_ms=%lld tail_entered=%d\n",original,int(schedule_control::start_status),int(expected),static_cast<long long>(elapsed),schedule_control::tail_entered.load());
 if(original || schedule_control::start_status!=expected) return 1;
 if(mode=="gate" && elapsed<2000) return 1;
 if(mode!="gate" && !schedule_control::tail_entered) return 1;
 return 0;
}
