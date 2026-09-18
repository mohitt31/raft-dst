// Fixed-rate load generator for a real cluster.
//
//   load_gen --peers 127.0.0.1:7401,... --rate 2000 --seconds 60 --conns 32
//
// Honest-latency rules this follows:
//   - open loop: request i is due at start + i/rate whether or not earlier
//     requests have finished, so a stalled cluster does not slow the offered
//     load down
//   - latency is measured from the DUE time, not from when a worker got to
//     it, which is what keeps coordinated omission from hiding the tail
//   - every sample is kept and sorted, so percentiles are exact
//   - a warmup period is measured separately and thrown away
//
// It also reports the longest gap between successful commits, which is the
// client-visible failover time when a leader is killed during the run.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "real/client.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Sample {
  double latency_us = 0;  // from when the request was DUE: includes queueing
  double service_us = 0;  // from when it was actually sent
  double finished_at_s = 0;
  bool ok = false;
};

struct Options {
  std::vector<std::pair<std::string, uint16_t>> peers;
  double rate = 1000;
  double seconds = 30;
  double warmup_seconds = 5;
  int conns = 32;
  int value_bytes = 32;
  int keys = 10000;
};

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == sep) {
      parts.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return parts;
}

double percentile(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0;
  const size_t i = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
  return sorted[i];
}

}  // namespace

int main(int argc, char** argv) {
  using namespace raftdst;
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
    if (std::strcmp(a, "--peers") == 0) {
      for (const std::string& hp : split(next(), ',')) {
        const auto parts = split(hp, ':');
        if (parts.size() == 2) {
          opt.peers.emplace_back(parts[0], static_cast<uint16_t>(std::atoi(parts[1].c_str())));
        }
      }
    } else if (std::strcmp(a, "--rate") == 0) {
      opt.rate = std::atof(next());
    } else if (std::strcmp(a, "--seconds") == 0) {
      opt.seconds = std::atof(next());
    } else if (std::strcmp(a, "--warmup") == 0) {
      opt.warmup_seconds = std::atof(next());
    } else if (std::strcmp(a, "--conns") == 0) {
      opt.conns = std::atoi(next());
    } else if (std::strcmp(a, "--value-bytes") == 0) {
      opt.value_bytes = std::atoi(next());
    } else {
      std::fprintf(stderr,
                   "usage: %s --peers host:port,... [--rate N] [--seconds N] [--warmup N] "
                   "[--conns N] [--value-bytes N]\n",
                   argv[0]);
      return 2;
    }
  }
  if (opt.peers.empty()) {
    std::fprintf(stderr, "need --peers\n");
    return 2;
  }

  const double total_seconds = opt.warmup_seconds + opt.seconds;
  const auto total_requests = static_cast<uint64_t>(opt.rate * total_seconds);
  std::atomic<uint64_t> next_request{0};
  std::atomic<uint64_t> not_leader{0}, lost{0}, errors{0};
  std::vector<std::vector<Sample>> per_worker(static_cast<size_t>(opt.conns));

  const std::string value(static_cast<size_t>(opt.value_bytes), 'x');
  const auto start = Clock::now();

  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(opt.conns));
  for (int w = 0; w < opt.conns; ++w) {
    workers.emplace_back([&, w] {
      size_t target = static_cast<size_t>(w) % opt.peers.size();
      std::unique_ptr<real::Client> client;
      std::vector<Sample>& samples = per_worker[static_cast<size_t>(w)];
      samples.reserve(total_requests / static_cast<uint64_t>(opt.conns) + 16);

      for (;;) {
        const uint64_t i = next_request.fetch_add(1, std::memory_order_relaxed);
        if (i >= total_requests) return;
        // When this request was due, independent of how busy we were.
        const auto due = start + std::chrono::duration_cast<Clock::duration>(
                                     std::chrono::duration<double>(
                                         static_cast<double>(i) / opt.rate));
        std::this_thread::sleep_until(due);

        if (!client) {
          std::string error;
          client = real::Client::connect(opt.peers[target].first, opt.peers[target].second,
                                         &error);
          if (!client) {
            errors.fetch_add(1, std::memory_order_relaxed);
            target = (target + 1) % opt.peers.size();
            continue;
          }
        }
        const std::string key = "k" + std::to_string(i % static_cast<uint64_t>(opt.keys));
        real::ClientResponse resp;
        std::string error;
        Sample sample;
        const auto sent_at = Clock::now();
        const bool sent =
            client->request({real::ClientOp::kPut, key, value}, resp, Duration::sec(5), &error);
        const auto done = Clock::now();
        sample.latency_us = std::chrono::duration<double, std::micro>(done - due).count();
        sample.service_us = std::chrono::duration<double, std::micro>(done - sent_at).count();
        sample.finished_at_s = std::chrono::duration<double>(done - start).count();
        if (!sent) {
          client.reset();
          errors.fetch_add(1, std::memory_order_relaxed);
          target = (target + 1) % opt.peers.size();
        } else if (resp.status == real::ClientStatus::kOk) {
          sample.ok = true;
        } else if (resp.status == real::ClientStatus::kNotLeader) {
          not_leader.fetch_add(1, std::memory_order_relaxed);
          target = resp.leader_hint.valid() && resp.leader_hint.get() < opt.peers.size()
                       ? resp.leader_hint.get()
                       : (target + 1) % opt.peers.size();
          client.reset();
        } else if (resp.status == real::ClientStatus::kLost) {
          lost.fetch_add(1, std::memory_order_relaxed);
        }
        samples.push_back(sample);
      }
    });
  }
  for (auto& t : workers) t.join();

  // Drop the warmup, then sort what is left.
  std::vector<double> latencies;
  std::vector<double> service;
  std::vector<double> success_times;
  uint64_t after_warmup = 0, ok = 0;
  for (const auto& worker : per_worker) {
    for (const Sample& s : worker) {
      if (s.finished_at_s < opt.warmup_seconds) continue;
      ++after_warmup;
      if (!s.ok) continue;
      ++ok;
      latencies.push_back(s.latency_us);
      service.push_back(s.service_us);
      success_times.push_back(s.finished_at_s);
    }
  }
  std::sort(latencies.begin(), latencies.end());
  std::sort(service.begin(), service.end());
  std::sort(success_times.begin(), success_times.end());

  double longest_gap_s = 0;
  for (size_t i = 1; i < success_times.size(); ++i) {
    longest_gap_s = std::max(longest_gap_s, success_times[i] - success_times[i - 1]);
  }

  std::printf("offered_rate_per_s %.0f\n", opt.rate);
  std::printf("measured_seconds %.1f connections %d value_bytes %d\n", opt.seconds, opt.conns,
              opt.value_bytes);
  std::printf("requests_after_warmup %llu\n", static_cast<unsigned long long>(after_warmup));
  std::printf("committed %llu not_leader %llu lost %llu errors %llu\n",
              static_cast<unsigned long long>(ok),
              static_cast<unsigned long long>(not_leader.load()),
              static_cast<unsigned long long>(lost.load()),
              static_cast<unsigned long long>(errors.load()));
  const double span_s = success_times.size() >= 2
                            ? success_times.back() - success_times.front()
                            : opt.seconds;
  std::printf("achieved_rate_per_s %.0f (over %.1f s of successes)\n",
              span_s > 0 ? static_cast<double>(ok) / span_s : 0.0, span_s);
  std::printf("commit_latency_us p50 %.0f p99 %.0f p99.9 %.0f max %.0f\n",
              percentile(latencies, 0.50), percentile(latencies, 0.99),
              percentile(latencies, 0.999), latencies.empty() ? 0.0 : latencies.back());
  std::printf("service_time_us p50 %.0f p99 %.0f p99.9 %.0f\n", percentile(service, 0.50),
              percentile(service, 0.99), percentile(service, 0.999));
  std::printf("longest_gap_between_commits_ms %.1f\n", longest_gap_s * 1000.0);
  // Latency far above service time means the offered rate exceeded what the
  // cluster could commit: the run measured a queue, not the cluster.
  if (percentile(latencies, 0.50) > 2 * percentile(service, 0.50) + 1000) {
    std::printf("SATURATED: latency is dominated by queueing, so the offered rate of %.0f/s is "
                "above what this cluster commits; lower --rate before quoting these latencies\n",
                opt.rate);
  }
  return ok > 0 ? 0 : 1;
}
