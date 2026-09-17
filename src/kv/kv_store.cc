#include "kv/kv_store.h"

#include "common/check.h"
#include "kv/command.h"

namespace raftdst::kv {

uint64_t pair_hash(std::string_view key, std::string_view value) {
  Fnv1a64 h;
  h.add_u64(key.size());
  h.add_bytes({reinterpret_cast<const uint8_t*>(key.data()), key.size()});
  h.add_u64(value.size());
  h.add_bytes({reinterpret_cast<const uint8_t*>(value.data()), value.size()});
  return h.value();
}

bool KvStore::valid(std::span<const uint8_t> command) const {
  Command c;
  return decode_command(command, c);
}

void KvStore::apply(Index index, std::span<const uint8_t> command) {
  RAFT_ASSERT(index == applied_.next(), "entries applied out of order");
  applied_ = index;
  if (command.empty()) return;  // leader no-op

  Command c;
  RAFT_ASSERT(decode_command(command, c), "committed entry is not a valid command");
  auto it = map_.find(c.key);
  if (it != map_.end()) content_hash_ ^= pair_hash(it->first, it->second);

  if (c.op == Op::kPut) {
    if (it == map_.end()) {
      it = map_.emplace(std::string{c.key}, std::string{c.value}).first;
    } else {
      it->second.assign(c.value);
    }
    content_hash_ ^= pair_hash(it->first, it->second);
  } else if (it != map_.end()) {
    map_.erase(it);
  }
}

std::optional<std::string_view> KvStore::get(std::string_view key) const {
  auto it = map_.find(key);
  if (it == map_.end()) return std::nullopt;
  return std::string_view{it->second};
}

}  // namespace raftdst::kv
