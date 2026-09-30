#include "/home/kbianco/.local/share/portfolio-control/worktrees/targets/cpp-rt-car-m24/samples/golden_xdma/owner.hpp"
#include <chrono>
#include <iostream>
#include <thread>
namespace gx=golden::xdma;
struct Gate {
 gx::SimulatedDriver &driver;
 std::atomic<bool> entered{false};
 std::thread worker;
 explicit Gate(gx::SimulatedDriver &d):driver(d) {
  driver.hold_event=true;
  worker=std::thread([this]{
   const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
   while(!driver.event_waiting && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
   entered=driver.event_waiting.load();
   if(entered)std::this_thread::sleep_for(std::chrono::milliseconds(30));
   driver.hold_event=false;
  });
 }
 ~Gate(){worker.join();driver.hold_event=false;}
};
int main(int argc,char **argv){
 if(argc!=2)return 2;
 const std::string_view which=argv[1];
 gx::Owner owner(golden::Options{});
 rt::Status result=rt::Status::internal_error;
 if(which=="startup") {
  Gate gate(*owner.driver);result=owner.prepare();
  std::cout<<"startup status="<<int(result)<<" ACK="<<owner.driver->safe_acks<<" detail="<<owner.session->runtime->last_error()<<'\n';
 } else {
  if(owner.prepare()!=rt::Status::ok)return 3;
  if(which=="regular") {Gate gate(*owner.driver);result=owner.session->step(0);}
  else if(which=="stop") {Gate gate(*owner.driver);result=owner.close();}
  else return 2;
  std::cout<<which<<" status="<<int(result)<<" ACK="<<owner.driver->safe_acks<<" retained="<<owner.memory->live_count()<<'\n';
 }
 const auto cleanup=owner.close();
 std::cout<<"cleanup="<<int(cleanup)<<" live="<<owner.driver->live<<" regions="<<owner.memory->live_count()<<'\n';
 return result==rt::Status::ok && cleanup==rt::Status::ok ? 0:1;
}
