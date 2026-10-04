#pragma once
#include <rt/runtime.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
namespace schedule_control {
inline bool delay_gate=false;
inline std::atomic<bool> release{false}, tail_entered{false};
inline rt::Status start_status=rt::Status::internal_error;
template<class P> void wait(P ready) {
 const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
 while(!ready()) {
  if(std::chrono::steady_clock::now()>end) std::_Exit(43);
  std::this_thread::yield();
 }
}
inline void native_tail() { if(!delay_gate) { tail_entered=true;wait([] {return release.load();}); } }
inline void gate_entry() { if(delay_gate) wait([] {return release.load();}); }
inline void after_start(rt::Status s) { start_status=s;release=true; }
}
