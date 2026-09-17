#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "common/hash.h"
#include "common/types.h"
#include "env/state_machine.h"

namespace raftdst::kv {

// Hash of one (key, value) pair. The store's content hash is the XOR of this
// over every pair, so it depends only on the contents, not on history or
// iteration order. Length-prefixed so ("ab", "c") and ("a", "bc") differ.
uint64_t pair_hash(std::string_view key, std::string_view value);

// In-memory key-value state machine.
//
// unordered_map with heterogeneous lookup: gets and deletes take string_view
// without building a std::string. The hasher is FNV-1a rather than std::hash,
// which differs between standard libraries. The map is never iterated, so
// bucket order cannot leak into behaviour.
class KvStore final : public IStateMachine {
 public:
  [[nodiscard]] bool valid(std::span<const uint8_t> command) const override;
  void apply(Index index, std::span<const uint8_t> command) override;
  [[nodiscard]] uint64_t digest() const override { return content_hash_; }

  [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const;
  [[nodiscard]] size_t size() const { return map_.size(); }
  [[nodiscard]] Index applied() const { return applied_; }

 private:
  struct KeyHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const {
      Fnv1a64 h;
      h.add_bytes({reinterpret_cast<const uint8_t*>(s.data()), s.size()});
      return static_cast<size_t>(h.value());
    }
  };

  std::unordered_map<std::string, std::string, KeyHash, std::equal_to<>> map_;
  uint64_t content_hash_ = 0;
  Index applied_;
};

}  // namespace raftdst::kv
