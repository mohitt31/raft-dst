#include "real/file_wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>

#include "common/bytes.h"
#include "common/check.h"
#include "common/crc32c.h"

namespace raftdst::real {

namespace {

constexpr uint8_t kRecordHardState = 1;
constexpr uint8_t kRecordEntries = 2;
constexpr size_t kRecordHeader = 8;  // length + crc

std::string errno_message(const char* what) {
  return std::string{what} + ": " + std::strerror(errno);
}

// Flushes to the physical medium, not just to the page cache.
bool flush_to_disk(int fd) {
#ifdef __APPLE__
  // fsync on macOS only pushes to the drive; F_FULLFSYNC makes it commit.
  return fcntl(fd, F_FULLFSYNC) != -1;
#elif defined(__linux__)
  return fdatasync(fd) == 0;
#else
  return fsync(fd) == 0;
#endif
}

void start_record(std::vector<uint8_t>& out, size_t& start, uint8_t type) {
  start = out.size();
  ByteWriter w{out};
  w.u32(0);  // length, patched in finish_record
  w.u32(0);  // crc, patched in finish_record
  w.u8(type);
}

void finish_record(std::vector<uint8_t>& out, size_t start) {
  ByteWriter w{out};
  const size_t body = out.size() - start - kRecordHeader;
  w.patch_u32(start, static_cast<uint32_t>(body));
  const std::span<const uint8_t> covered{out.data() + start + kRecordHeader, body};
  w.patch_u32(start + 4, crc32c(covered));
}

}  // namespace

std::unique_ptr<FileWal> FileWal::open(const std::string& path, std::string* error) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    if (error) *error = errno_message("open");
    return nullptr;
  }

  std::vector<uint8_t> image;
  uint8_t buf[64 * 1024];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      if (error) *error = errno_message("read");
      ::close(fd);
      return nullptr;
    }
    if (n == 0) break;
    image.insert(image.end(), buf, buf + n);
  }

  Recovered recovered;
  const uint64_t good = replay(image, recovered);
  const uint64_t torn = image.size() - good;
  if (torn > 0 && ftruncate(fd, static_cast<off_t>(good)) != 0) {
    if (error) *error = errno_message("ftruncate");
    ::close(fd);
    return nullptr;
  }
  if (lseek(fd, 0, SEEK_END) < 0) {
    if (error) *error = errno_message("lseek");
    ::close(fd);
    return nullptr;
  }

  auto wal = std::unique_ptr<FileWal>(new FileWal(fd, std::move(recovered)));
  wal->stats_.torn_tail_bytes = torn;
  return wal;
}

FileWal::~FileWal() {
  if (fd_ >= 0) ::close(fd_);
}

uint64_t FileWal::replay(std::span<const uint8_t> image, Recovered& out) {
  out = Recovered{};
  size_t offset = 0;
  while (offset + kRecordHeader <= image.size()) {
    ByteReader header{image.subspan(offset, kRecordHeader)};
    const uint64_t body_len = header.u32();
    const uint32_t want_crc = header.u32();
    const size_t total = kRecordHeader + body_len;
    if (body_len < 1 || offset + total > image.size()) break;  // torn tail

    const std::span<const uint8_t> body = image.subspan(offset + kRecordHeader, body_len);
    if (crc32c(body) != want_crc) break;  // damaged record

    ByteReader r{body};
    const uint8_t type = r.u8();
    if (type == kRecordHardState) {
      HardState hs;
      hs.term = Term{r.u64()};
      const uint8_t vote = r.u8();
      hs.voted_for = vote == 0xFF ? NodeId::none() : NodeId{vote};
      if (!r.ok() || !r.at_end()) break;
      out.hard_state = hs;
    } else if (type == kRecordEntries) {
      const Index first{r.u64()};
      const uint64_t count = r.u32();
      if (!r.ok() || first.get() == 0 || first > out.log.last_index().next()) break;
      if (count > r.remaining() / 12) break;
      const auto heads = r.bytes(count * 12);
      if (!r.ok()) break;
      out.log.truncate_after(first.prev());
      bool damaged = false;
      for (uint64_t i = 0; i < count && !damaged; ++i) {
        ByteReader h{heads.subspan(i * 12, 12)};
        const Term term{h.u64()};
        const uint32_t len = h.u32();
        const auto payload = r.bytes(len);
        if (!r.ok() || term < out.log.last_term()) {
          damaged = true;
          break;
        }
        out.log.append(term, payload);
      }
      if (damaged || !r.at_end()) break;
    } else {
      break;  // unknown record type
    }
    offset += total;
  }
  return offset;
}

void FileWal::set_hard_state(const HardState& hs) {
  size_t start = 0;
  start_record(staged_, start, kRecordHardState);
  ByteWriter w{staged_};
  w.u64(hs.term.get());
  w.u8(hs.voted_for == NodeId::none() ? 0xFF : hs.voted_for.get());
  finish_record(staged_, start);
}

void FileWal::append(const LogSlice& entries) {
  RAFT_ASSERT(entries.first().get() >= 1, "append at the sentinel index");
  if (entries.empty()) return;
  size_t start = 0;
  start_record(staged_, start, kRecordEntries);
  ByteWriter w{staged_};
  w.u64(entries.first().get());
  w.u32(static_cast<uint32_t>(entries.size()));
  for (uint64_t i = 0; i < entries.size(); ++i) {
    w.u64(entries.term(i).get());
    w.u32(static_cast<uint32_t>(entries.payload(i).size()));
  }
  w.bytes(entries.payload_blob());
  finish_record(staged_, start);
}

void FileWal::sync() {
  if (staged_.empty()) return;
  size_t written = 0;
  while (written < staged_.size()) {
    const ssize_t n = ::write(fd_, staged_.data() + written, staged_.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      RAFT_ASSERT(false, "write to the write-ahead log failed");
    }
    written += static_cast<size_t>(n);
  }
  RAFT_ASSERT(flush_to_disk(fd_), "flushing the write-ahead log failed");
  stats_.bytes_written += staged_.size();
  ++stats_.syncs;
  staged_.clear();
}

Recovered FileWal::load() {
  RAFT_ASSERT(!loaded_, "load() called twice");
  RAFT_ASSERT(staged_.empty(), "load() with unsynced writes");
  loaded_ = true;
  return std::move(recovered_);
}

}  // namespace raftdst::real
