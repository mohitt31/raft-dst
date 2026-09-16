#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/hash.h"
#include "common/types.h"

namespace raftdst::sim {

enum class TraceKind : uint8_t {
  kStart,
  kSend,
  kDropRandom,
  kDropPartition,
  kDropDead,
  kDuplicate,
  kDeliver,
  kWake,
  kCrash,
  kCrashInSync,
  kArmCrashInSync,
  kRestart,
  kPartition,
  kHeal,
  kFaultsEnd,
  kUser,  // recorded by node programs or the checker
};

std::string_view trace_kind_name(TraceKind k);

// Every simulator event goes through record(). The fingerprint is always
// maintained; the text form is optional and only for humans. Both are built
// from integers only, so they are identical across platforms.
class Trace {
 public:
  explicit Trace(bool text) : text_enabled_(text) {}

  void record(Time t, TraceKind kind, uint8_t node, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0);

  [[nodiscard]] uint64_t fingerprint() const { return hash_.value(); }
  [[nodiscard]] uint64_t records() const { return records_; }
  [[nodiscard]] const std::string& text() const { return text_; }
  [[nodiscard]] bool text_enabled() const { return text_enabled_; }

 private:
  Fnv1a64 hash_;
  uint64_t records_ = 0;
  bool text_enabled_;
  std::string text_;
};

}  // namespace raftdst::sim
