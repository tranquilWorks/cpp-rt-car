#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace simcore {

// Simple hazard pointer implementation for lock-free structures.
// Every live guard owns one record; retired nodes are reclaimed when unprotected.

struct HazardRecord {
  std::atomic<void *> pointer{nullptr};
  std::atomic<bool> owned{false};
  HazardRecord *next{nullptr};
};

inline std::atomic<HazardRecord *> global_hazard_head{nullptr};

inline HazardRecord *acquire_hazard() {
  for (HazardRecord *rec = global_hazard_head.load(std::memory_order_seq_cst);
       rec; rec = rec->next) {
    bool expected = false;
    if (rec->owned.compare_exchange_strong(expected, true,
                                           std::memory_order_acquire,
                                           std::memory_order_relaxed))
      return rec;
  }
  auto *rec = new HazardRecord();
  rec->owned.store(true, std::memory_order_relaxed);
  HazardRecord *old = global_hazard_head.load(std::memory_order_seq_cst);
  do {
    rec->next = old;
  } while (!global_hazard_head.compare_exchange_weak(
      old, rec, std::memory_order_seq_cst, std::memory_order_relaxed));
  return rec;
}

class HazardGuard {
public:
  HazardGuard() : rec_(acquire_hazard()) {}
  HazardGuard(const HazardGuard &) = delete;
  HazardGuard &operator=(const HazardGuard &) = delete;
  ~HazardGuard() {
    clear();
    rec_->owned.store(false, std::memory_order_release);
  }

  template <typename T> T *protect(std::atomic<T *> &src) noexcept {
    T *p = nullptr;
    do {
      // Source removal, publication, and validation form one sequentially
      // consistent protocol with the reclaimer's hazard scan.
      p = src.load(std::memory_order_seq_cst);
      rec_->pointer.store(p, std::memory_order_seq_cst);
    } while (p != src.load(std::memory_order_seq_cst));
    return p;
  }

  void clear() noexcept { rec_->pointer.store(nullptr, std::memory_order_seq_cst); }

private:
  HazardRecord *rec_;
};

struct RetiredNode {
  void *ptr;
  void (*deleter)(void *);
};

inline void default_delete(void *p) { ::operator delete(p); }

inline std::vector<RetiredNode> &retired_list() {
  thread_local std::vector<RetiredNode> list;
  return list;
}

inline void scan() {
  // Gather all hazard pointers
  std::vector<void *> hazards;
  for (HazardRecord *r = global_hazard_head.load(std::memory_order_seq_cst); r;
       r = r->next) {
    hazards.push_back(r->pointer.load(std::memory_order_seq_cst));
  }
  auto &list = retired_list();
  auto it = list.begin();
  while (it != list.end()) {
    void *p = it->ptr;
    bool protected_ptr = false;
    for (void *h : hazards) {
      if (h == p) {
        protected_ptr = true;
        break;
      }
    }
    if (!protected_ptr) {
      it->deleter(p);
      it = list.erase(it);
    } else {
      ++it;
    }
  }
}

inline void retire(void *p, void (*deleter)(void *) = default_delete) {
  auto &list = retired_list();
  list.push_back({p, deleter});
  if (list.size() >= 16)
    scan();
}

} // namespace simcore
