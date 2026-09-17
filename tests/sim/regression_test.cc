// Replays every committed fuzz case in tests/regressions/*.repro. Each one
// found a real defect once; none may fail again.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "harness/fuzz_config.h"
#include "harness/scenario.h"

namespace raftdst::harness {
namespace {

std::filesystem::path regressions_dir() {
  const char* from_env = std::getenv("RAFTDST_REGRESSIONS");
  return from_env ? std::filesystem::path{from_env}
                  : std::filesystem::path{RAFTDST_REGRESSIONS_DIR};
}

TEST(Regressions, EveryRecordedCasePasses) {
  const auto dir = regressions_dir();
  ASSERT_TRUE(std::filesystem::exists(dir)) << dir;
  uint64_t cases = 0;
  for (const auto& entry : std::filesystem::directory_iterator{dir}) {
    if (entry.path().extension() != ".repro") continue;
    std::ifstream in{entry.path()};
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      FuzzCase c;
      ASSERT_TRUE(FuzzCase::parse(line, c)) << entry.path().filename() << ": " << line;
      const ScenarioResult r = run_scenario(c.to_scenario());
      ++cases;
      for (const auto& v : r.violations) {
        ADD_FAILURE() << entry.path().filename().string() << " failed again: " << v << "\n  "
                      << c.to_line();
      }
    }
  }
  std::printf("replayed %llu recorded case(s)\n", static_cast<unsigned long long>(cases));
}

}  // namespace
}  // namespace raftdst::harness
