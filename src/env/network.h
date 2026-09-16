#pragma once

#include <cstdint>
#include <span>

#include "common/types.h"

namespace raftdst {

// Best-effort datagram-style delivery between cluster nodes. A frame may be
// dropped, delayed, reordered or duplicated. The implementation copies the
// bytes before send() returns; the caller keeps ownership of `frame`.
// Receiving is push-based: the event loop calls INode::on_frame.
class INetwork {
 public:
  virtual ~INetwork() = default;
  virtual void send(NodeId to, std::span<const uint8_t> frame) = 0;
};

}  // namespace raftdst
