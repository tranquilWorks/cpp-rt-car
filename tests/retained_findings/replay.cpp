#include "../replay_rejection/scenario.hpp"
#include <cstdio>
namespace rtfw_physics_allocation { void begin() noexcept; std::size_t end() noexcept; }
namespace a=rtfw_physics_allocation;
using namespace replay_rejection;
static void pre_effect(bool active,bool lossless) {
 Scenario s(active,lossless);
 const auto initial=s.checkpoint(0);s.step(1);s.step(2);
 const auto artifact=s.capture(initial,1,2);
 const auto state=s.state;const auto calls=s.calls;
 rt::LiveControlMailboxInfo mailbox;
 require(s.runtime.live_control_mailbox_info(1,mailbox),"mailbox");
 rt::LiveControlActionMetadata actions;
 s.check(s.runtime.live_control_action_metadata(actions),"actions");
 // Fixed structural/header/body corruption plus truncation, not a universal parser proof.
 for(unsigned mode=0;mode<7;++mode) {
  auto malformed=artifact;
  if(mode<3) malformed.resize(mode==0?0:(mode==1?24:artifact.size()-1));
  else malformed[mode==3?0:(mode==4?8:(mode==5?24:artifact.size()-1))]^=std::byte{1};
  rt::LiveControlReplayResult result;
  a::begin();const auto status=s.runtime.replay_live_control(malformed,Scenario::input,&s,&result);const auto allocated=a::end();
  require(status==rt::Status::invalid_artifact,"malformed status");
  require(allocated==0 && result.frames_replayed==0 && s.calls==calls && s.input_calls==0 && s.state==state,"pre-effect state/callback/allocation conservation");
  rt::LiveControlMailboxInfo after;require(s.runtime.live_control_mailbox_info(1,after),"mailbox after");
  require(mailbox_fields(after)==mailbox_fields(mailbox),"mailbox conservation");
  rt::LiveControlActionMetadata after_actions;s.check(s.runtime.live_control_action_metadata(after_actions),"actions after");
  require(action_fields(after_actions)==action_fields(actions),"capture history conservation");
 }
 rt::LiveControlReplayResult result;
 s.check(s.runtime.replay_live_control(artifact,Scenario::input,&s,&result),"valid recovery replay");
 require(result.frames_replayed==2 && s.state==state,"recovery state");
 std::printf("PASS pre-effect active=%d v%u seven malformed controls, capture conservation and valid recovery\n",active,lossless?2u:1u);
}
struct Effect { Scenario& scenario; unsigned calls=0; };
static rt::CallbackResult fail_after_effect(void* p,const rt::ReplayInputView&) {
 auto& e=*static_cast<Effect*>(p);++e.calls;e.scenario.state[7]=std::byte{99};
 return rt::CallbackResult::error;
}
static void post_effect(bool active,bool lossless) {
 Scenario s(active,lossless);const auto initial=s.checkpoint(0);s.step(1);s.step(2);
 const auto artifact=s.capture(initial,1,2);const auto expected=s.state;
 Effect effect{s};rt::LiveControlReplayResult result;
 a::begin();const auto status=s.runtime.replay_live_control(artifact,fail_after_effect,&effect,&result);const auto allocated=a::end();
 require(status!=rt::Status::ok && result.mismatch_status==status && result.frames_replayed==0 && effect.calls==1 && allocated==0,"post-effect failure accounting");
 require(s.state[0]==std::byte{0} && s.state[7]==std::byte{99},"restore happened and caller side effect is not rolled back");
 s.check(s.runtime.replay_live_control(artifact,Scenario::input,&s,&result),"explicit recovery");
 require(result.frames_replayed==2 && s.state==expected,"explicit replay recovery state");
 std::printf("PASS post-effect active=%d v%u status=%d no implicit application rollback; explicit recovery\n",active,lossless?2u:1u,int(status));
}
int main() {
 try {
  Clock clock;rt::Runtime unrelated(clock);require(unrelated.configure({})==rt::Status::ok,"configure owner namespace separation");
  rt::PhaseHandle phase;require(unrelated.register_callback({"unused",[](void*,const rt::CallbackContext&) {return rt::CallbackResult::ok;},nullptr},phase)==rt::Status::ok,"unused callback");
  require(unrelated.finalize()==rt::Status::ok,"unused finalize");
  for(bool active:{false,true}) for(bool lossless:{false,true}) {pre_effect(active,lossless);post_effect(active,lossless);}
 } catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
