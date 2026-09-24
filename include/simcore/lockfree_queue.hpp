#pragma once
#include "simcore/hazard_ptr.hpp"
#include <atomic>
#include <cassert>

namespace simcore {

// Michael & Scott lock-free queue using hazard pointers for safe memory
// reclamation.

template <typename T> class LockFreeQueue {
  struct Node {
    T value;
    std::atomic<Node *> next{nullptr};
    Node() = default;
    explicit Node(const T &v) : value(v) {}
  };

  std::atomic<Node *> head_{nullptr};
  std::atomic<Node *> tail_{nullptr};

public:
  LockFreeQueue() {
    Node *dummy = new Node();
    head_.store(dummy, std::memory_order_relaxed);
    tail_.store(dummy, std::memory_order_relaxed);
  }

  ~LockFreeQueue() {
    T tmp;
    while (pop(tmp)) {
    }
    Node *dummy = head_.load();
    delete dummy;
  }

  void push(const T &v) {
    HazardGuard tail_guard;
    Node *n = new Node(v);
    while (true) {
      Node *t = tail_guard.protect(tail_);
      Node *next = t->next.load(std::memory_order_acquire);
      if (t == tail_.load(std::memory_order_seq_cst)) {
        if (!next) {
          if (t->next.compare_exchange_weak(next, n)) {
            tail_.compare_exchange_weak(t, n);
            return;
          }
        } else {
          tail_.compare_exchange_weak(t, next);
        }
      }
    }
  }

  bool pop(T &out) {
    HazardGuard head_guard;
    HazardGuard next_guard;
    while (true) {
      Node *h = head_guard.protect(head_);
      Node *t = tail_.load(std::memory_order_acquire);
      Node *next = next_guard.protect(h->next);
      // A removed head retains its next link. Validate the queue head again
      // before accessing the separately protected successor.
      if (h == head_.load(std::memory_order_seq_cst)) {
        if (h == t) {
          if (!next)
            return false;
          tail_.compare_exchange_weak(t, next);
        } else {
          out = next->value;
          if (head_.compare_exchange_weak(h, next)) {
            head_guard.clear();
            retire(h, [](void *p) { delete static_cast<Node *>(p); });
            return true;
          }
        }
      }
    }
  }
};

} // namespace simcore
