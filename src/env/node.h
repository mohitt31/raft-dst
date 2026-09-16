#pragma once

#include <cstdint>
#include <span>

#include "common/types.h"
#include "env/clock.h"
#include "env/network.h"
#include "env/storage.h"

namespace raftdst {

// Everything a node may touch. The simulator and the real event loop each
// provide one per node; the node cannot tell which it is running on.
struct NodeContext {
  NodeId self;
  uint8_t cluster_size = 0;
  uint64_t seed = 0;  // for the node's own randomness (election jitter)
  IClock& clock;
  INetwork& net;
  IStorage& storage;
};

// A node program driven by an event loop. Callbacks are never reentered, and
// run to completion unless the process dies inside them.
class INode {
 public:
  virtual ~INode() = default;
  virtual void on_frame(NodeId from, std::span<const uint8_t> frame) = 0;
  virtual void on_wake() = 0;
};

}  // namespace raftdst
