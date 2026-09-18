#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "env/storage.h"

namespace raftdst::real {

// Append-only write-ahead log on a real file.
//
// Record layout, little-endian:
//   u32 body_len        bytes after this field, excluding the CRC
//   u32 crc32c          over the type byte and the body
//   u8  type            1 = hard state, 2 = entries
//   ... body
//
// A hard-state record is (u64 term, u8 voted_for). An entries record is
// (u64 first_index, u32 count, count x (u64 term, u32 len), payload blob) and
// means "the log from first_index onward is exactly these entries", so the
// truncation point travels with the entries and cannot land without them.
//
// Writes are staged until sync(), which writes them and flushes the drive
// cache (F_FULLFSYNC on macOS, fdatasync on Linux). A crash can therefore
// leave a torn record at the end; load() stops at the first short or
// bad-CRC record and truncates the file there.
class FileWal final : public IStorage {
 public:
  struct Stats {
    uint64_t records_written = 0;
    uint64_t bytes_written = 0;
    uint64_t syncs = 0;
    uint64_t torn_tail_bytes = 0;  // discarded at open
  };

  // Opens or creates `path`. Returns nullptr and sets `error` on failure.
  static std::unique_ptr<FileWal> open(const std::string& path, std::string* error);
  ~FileWal() override;

  void set_hard_state(const HardState& hs) override;
  void append(const LogSlice& entries) override;
  void sync() override;
  [[nodiscard]] Recovered load() override;

  [[nodiscard]] const Stats& stats() const { return stats_; }
  [[nodiscard]] uint64_t staged_bytes() const { return staged_.size(); }

  // Parses a whole WAL image. Stops at the first damaged record and reports
  // how many bytes were good. Exposed for tests.
  static uint64_t replay(std::span<const uint8_t> image, Recovered& out);

 private:
  FileWal(int fd, Recovered recovered) : fd_(fd), recovered_(std::move(recovered)) {}

  int fd_;
  Recovered recovered_;
  std::vector<uint8_t> staged_;
  bool loaded_ = false;
  Stats stats_;
};

}  // namespace raftdst::real
