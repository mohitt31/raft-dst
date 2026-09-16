#pragma once

#include <cstdint>
#include <span>

namespace raftdst {

// CRC-32C (Castagnoli), reflected, as used by iSCSI and ext4. Protects WAL
// records against torn writes. Table-driven software version.

// Continue a CRC over more data. Start with crc = 0.
uint32_t crc32c_extend(uint32_t crc, std::span<const uint8_t> data);

inline uint32_t crc32c(std::span<const uint8_t> data) { return crc32c_extend(0, data); }

}  // namespace raftdst
