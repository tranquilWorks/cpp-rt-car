#pragma once
#include "world.hpp"

namespace golden {
// Optional, configuring-only sample extension. The borrowed owner and its
// resources outlive Session's checked close. The CPU default uses no extension.
class Physics {
public:
  virtual ~Physics() = default;
  virtual void limits(rt::RuntimeConfig &) const noexcept = 0;
  virtual bool replay_enabled() const noexcept = 0;
  virtual rt::Status configure(rt::Runtime &, World &) noexcept = 0;
  virtual rt::Status register_phase(rt::Runtime &, rt::RateDomainHandle,
                                   rt::PhaseHandle &) noexcept = 0;
  virtual bool input(const rt::CallbackContext &) noexcept = 0;
  virtual bool complete(const rt::CallbackContext &) noexcept = 0;
  virtual bool plan(const rt::MemoryPlan &) const noexcept = 0;
};
} // namespace golden
