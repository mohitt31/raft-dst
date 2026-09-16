#pragma once

#include <cstdint>
#include <type_traits>
#include <vector>

#include "common/check.h"
#include "common/types.h"

namespace raftdst::sim {

enum class EventKind : uint8_t {
  kDeliver,         // arg = message slot
  kWake,            // arg = timer generation
  kRestart,         // arg = restart token
  kCrashTick,       // random crash injection
  kPartitionStart,  // random partition injection
  kHeal,            // arg = partition token
  kFaultsEnd,       // stop injecting faults, heal everything
};

// 24 bytes, trivially copyable, so heap sifts are plain copies.
struct Event {
  Time at;
  uint64_t seq;
  uint32_t arg;
  EventKind kind;
  uint8_t node;
};

inline constexpr bool event_before(const Event& a, const Event& b) {
  return a.at != b.at ? a.at < b.at : a.seq < b.seq;
}

// Binary min-heap on (at, seq). seq is a per-queue insertion counter, so the
// key is unique and pop order is fully defined by the key: equal-time events
// come out in insertion order on every standard library.
class EventQueue {
 public:
  void push(Time at, EventKind kind, uint8_t node, uint32_t arg) {
    heap_.push_back(Event{at, next_seq_++, arg, kind, node});
    sift_up(heap_.size() - 1);
  }

  [[nodiscard]] bool empty() const { return heap_.empty(); }
  [[nodiscard]] size_t size() const { return heap_.size(); }

  [[nodiscard]] const Event& top() const {
    RAFT_DCHECK(!heap_.empty(), "top() on empty queue");
    return heap_.front();
  }

  Event pop() {
    RAFT_ASSERT(!heap_.empty(), "pop() on empty queue");
    Event out = heap_.front();
    heap_.front() = heap_.back();
    heap_.pop_back();
    if (!heap_.empty()) sift_down(0);
    return out;
  }

  void reserve(size_t n) { heap_.reserve(n); }

 private:
  void sift_up(size_t i) {
    Event e = heap_[i];
    while (i > 0) {
      size_t parent = (i - 1) / 2;
      if (!event_before(e, heap_[parent])) break;
      heap_[i] = heap_[parent];
      i = parent;
    }
    heap_[i] = e;
  }

  void sift_down(size_t i) {
    const size_t n = heap_.size();
    Event e = heap_[i];
    for (;;) {
      size_t child = 2 * i + 1;
      if (child >= n) break;
      if (child + 1 < n && event_before(heap_[child + 1], heap_[child])) ++child;
      if (!event_before(heap_[child], e)) break;
      heap_[i] = heap_[child];
      i = child;
    }
    heap_[i] = e;
  }

  std::vector<Event> heap_;
  uint64_t next_seq_ = 0;
};

static_assert(sizeof(Event) == 24);
static_assert(std::is_trivially_copyable_v<Event>);

}  // namespace raftdst::sim
