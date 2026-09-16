#pragma once

// RAFT_ASSERT is always compiled in. Use it where continuing would corrupt
// state or hide a safety violation.
//
// RAFT_DCHECK is for internal invariants that are too costly or too
// paranoid for the release engine. It is on in the asan, tsan and mutants
// presets. When off, the condition is not evaluated but still type-checked.

namespace raftdst::detail {

[[noreturn]] void check_failed(const char* file, int line, const char* expr,
                               const char* msg) noexcept;

}  // namespace raftdst::detail

#define RAFT_ASSERT(cond, msg)                                           \
  do {                                                                   \
    if (!(cond)) [[unlikely]] {                                          \
      ::raftdst::detail::check_failed(__FILE__, __LINE__, #cond, (msg)); \
    }                                                                    \
  } while (false)

#ifdef RAFTDST_DCHECKS
#define RAFT_DCHECK(cond, msg) RAFT_ASSERT(cond, msg)
#else
#define RAFT_DCHECK(cond, msg)          \
  do {                                  \
    static_cast<void>(sizeof(!(cond))); \
  } while (false)
#endif
