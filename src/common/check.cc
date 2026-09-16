#include "common/check.h"

#include <cstdio>
#include <cstdlib>

namespace raftdst::detail {

void check_failed(const char* file, int line, const char* expr, const char* msg) noexcept {
  std::fprintf(stderr, "%s:%d: check failed: %s\n  %s\n", file, line, expr, msg);
  std::fflush(stderr);
  std::abort();
}

}  // namespace raftdst::detail
