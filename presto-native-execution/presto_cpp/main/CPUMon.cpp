/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "presto_cpp/main/CPUMon.h"
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include "fmt/format.h"
#include "folly/Conv.h"
#include "folly/File.h"
#include "folly/FileUtil.h"
#include "folly/Optional.h"
#include "folly/String.h"
#include "glog/logging.h"

namespace facebook::presto {

namespace {

// cgroup v2 keeps the CPU accounting of a cgroup in these two files, relative
// to the cgroup's own directory under the mount root.
constexpr const char* kCgroupV2UsageFile = "cpu.stat";
constexpr const char* kCgroupV2QuotaFile = "cpu.max";
constexpr const char* kCgroupMountRoot = "/sys/fs/cgroup";

// cgroup v1 splits the accounting across the 'cpuacct' and 'cpu' controllers,
// which may be mounted separately or together.
constexpr const char* const kCgroupV1UsageFiles[] = {
    "/sys/fs/cgroup/cpuacct/cpuacct.usage",
    "/sys/fs/cgroup/cpu,cpuacct/cpuacct.usage"};
constexpr const char* const kCgroupV1QuotaFiles[] = {
    "/sys/fs/cgroup/cpu/cpu.cfs_quota_us",
    "/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us"};
constexpr const char* const kCgroupV1PeriodFiles[] = {
    "/sys/fs/cgroup/cpu/cpu.cfs_period_us",
    "/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_period_us"};

bool fileExists(const std::string& path) {
  struct stat buffer;
  return !path.empty() && stat(path.c_str(), &buffer) == 0;
}

// Returns the first of 'candidates' that exists, or an empty string.
template <size_t N>
std::string firstExisting(const char* const (&candidates)[N]) {
  for (const auto* candidate : candidates) {
    if (fileExists(candidate)) {
      return candidate;
    }
  }
  return "";
}

// Reads a small pseudo-file in full. False when it cannot be read.
bool readSmallFile(const std::string& path, std::string& out) {
  return !path.empty() && folly::readFile(path.c_str(), out);
}

// Parses a pseudo-file holding a single number, ignoring trailing whitespace.
// Returns 'folly::none' when the contents are not a number.
folly::Optional<int64_t> parseSingleValue(const std::string& content) {
  const auto value =
      folly::tryTo<int64_t>(folly::trimWhitespace(folly::StringPiece(content)));
  return value.hasValue() ? folly::Optional<int64_t>(value.value())
                          : folly::none;
}

int64_t nowMonotonicUsec() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

static bool readProcStat(std::vector<uint64_t>& counters) {
  auto cpuStatFile = folly::File("/proc/stat", O_RDONLY);
  // Enough storage for the /proc/stat CPU data needed below
  std::array<char, 320> buf;
  if (folly::readNoInt(cpuStatFile.fd(), buf.data(), buf.size()) !=
      static_cast<ssize_t>(buf.size())) {
    return false;
  }

  const static char* fmt = "cpu %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                           " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64;
  if (sscanf(
          buf.data(),
          fmt,
          &counters[0],
          &counters[1],
          &counters[2],
          &counters[3],
          &counters[4],
          &counters[5],
          &counters[6],
          &counters[7]) != static_cast<int>(counters.size())) {
    return false;
  }

  return true;
}

std::string CPUMon::parseCgroupV2RelativePath(const std::string& procSelf) {
  // Lines are '<hierarchy-id>:<controllers>:<path>'. The cgroup v2 entry is the
  // one with an empty controller list. A cgroup path may itself contain a
  // colon, so the fields are split by position rather than by counting
  // separators.
  std::vector<folly::StringPiece> lines;
  folly::split('\n', procSelf, lines);
  for (const auto& line : lines) {
    const auto firstColon = line.find(':');
    if (firstColon == std::string::npos) {
      continue;
    }
    if (line.find(':', firstColon + 1) != firstColon + 1) {
      continue;
    }
    const auto path = folly::trimWhitespace(line.subpiece(firstColon + 2));
    // The root cgroup adds no prefix, and is what a container started with
    // 'cgroupns=private' sees for its own cgroup.
    return path == "/" ? "" : path.str();
  }
  return "";
}

void CPUMon::detectCgroupFiles() {
  // cgroup v2 first: a host running it has no v1 controller directories, while
  // a host running v1 has no 'cpu.max' at the mount root.
  const std::string v2Root = kCgroupMountRoot;
  if (fileExists(fmt::format("{}/{}", v2Root, kCgroupV2QuotaFile))) {
    cgroupV2_ = true;
    // With 'cgroupns=host' the mount root describes the whole machine rather
    // than this container, so prefer our own cgroup's directory when the
    // kernel tells us where it is. With 'cgroupns=private' the process already
    // sees its own cgroup as the root and there is no prefix to add.
    std::string base = v2Root;
    std::string procSelf;
    if (folly::readFile("/proc/self/cgroup", procSelf)) {
      const auto relative = parseCgroupV2RelativePath(procSelf);
      if (!relative.empty() &&
          fileExists(
              fmt::format("{}{}/{}", v2Root, relative, kCgroupV2QuotaFile))) {
        base = v2Root + relative;
      }
    }
    const auto usageFile = fmt::format("{}/{}", base, kCgroupV2UsageFile);
    if (fileExists(usageFile)) {
      cgroupUsageFile_ = usageFile;
      cgroupQuotaFile_ = fmt::format("{}/{}", base, kCgroupV2QuotaFile);
    }
  } else {
    cgroupV2_ = false;
    cgroupUsageFile_ = firstExisting(kCgroupV1UsageFiles);
    cgroupQuotaFile_ = firstExisting(kCgroupV1QuotaFiles);
    cgroupPeriodFile_ = firstExisting(kCgroupV1PeriodFiles);
    if (cgroupUsageFile_.empty() || cgroupQuotaFile_.empty() ||
        cgroupPeriodFile_.empty()) {
      cgroupUsageFile_.clear();
      cgroupQuotaFile_.clear();
      cgroupPeriodFile_.clear();
    }
  }

  if (cgroupUsageFile_.empty()) {
    LOG(INFO) << "No cgroup CPU accounting found. Reporting host-wide CPU load "
                 "as the process CPU load.";
    return;
  }
  LOG(INFO) << "Using cgroup " << (cgroupV2_ ? "v2" : "v1")
            << " CPU accounting from " << cgroupUsageFile_ << " (quota from "
            << cgroupQuotaFile_ << ").";
}

int64_t CPUMon::readCgroupCpuUsageUsec() const {
  std::string content;
  if (!readSmallFile(cgroupUsageFile_, content)) {
    return -1;
  }

  if (cgroupV2_) {
    // 'cpu.stat' holds one 'key value' pair per line. 'usage_usec' is the
    // cgroup's cumulative CPU time, in microseconds.
    constexpr folly::StringPiece kUsageKey{"usage_usec "};
    std::vector<folly::StringPiece> lines;
    folly::split('\n', content, lines);
    for (const auto& line : lines) {
      if (!line.startsWith(kUsageKey)) {
        continue;
      }
      const auto value = folly::trimWhitespace(line.subpiece(kUsageKey.size()));
      const auto usageUsec = folly::tryTo<int64_t>(value);
      if (!usageUsec.hasValue() || usageUsec.value() < 0) {
        return -1;
      }
      return usageUsec.value();
    }
    return -1;
  }

  // cgroup v1's 'cpuacct.usage' is a single number, in nanoseconds.
  const auto usageNsec = parseSingleValue(content);
  if (!usageNsec.hasValue() || usageNsec.value() < 0) {
    return -1;
  }
  return usageNsec.value() / 1000;
}

double CPUMon::readCgroupCpuQuotaCores() const {
  std::string content;
  if (!readSmallFile(cgroupQuotaFile_, content)) {
    return 0;
  }

  int64_t quotaUsec = 0;
  int64_t periodUsec = 0;
  if (cgroupV2_) {
    // 'cpu.max' holds '<quota> <period>', both in microseconds, where a quota
    // of the literal 'max' means no limit is set.
    std::vector<folly::StringPiece> parts;
    folly::split(
        ' ', folly::trimWhitespace(folly::StringPiece(content)), parts);
    if (parts.size() != 2) {
      return 0;
    }
    const auto quota = folly::tryTo<int64_t>(parts[0]);
    const auto period = folly::tryTo<int64_t>(parts[1]);
    if (!quota.hasValue() || !period.hasValue()) {
      return 0;
    }
    quotaUsec = quota.value();
    periodUsec = period.value();
  } else {
    // cgroup v1 keeps the quota and the period in separate files, and uses -1
    // for 'no limit set'.
    const auto quota = parseSingleValue(content);
    std::string periodContent;
    if (!quota.hasValue() ||
        !readSmallFile(cgroupPeriodFile_, periodContent)) {
      return 0;
    }
    const auto period = parseSingleValue(periodContent);
    if (!period.hasValue()) {
      return 0;
    }
    quotaUsec = quota.value();
    periodUsec = period.value();
  }

  if (quotaUsec <= 0 || periodUsec <= 0) {
    return 0;
  }
  // Kept fractional: a Kubernetes '500m' limit is half a core, and rounding it
  // to zero would silently fall back to the host-wide load.
  return static_cast<double>(quotaUsec) / static_cast<double>(periodUsec);
}

double CPUMon::computeCgroupCpuLoadPct(
    int64_t prevUsageUsec,
    int64_t usageUsec,
    int64_t prevElapsedUsec,
    int64_t elapsedUsec,
    double quotaCores) {
  if (quotaCores <= 0 || prevUsageUsec < 0 || usageUsec < 0 ||
      prevElapsedUsec < 0 || elapsedUsec < 0) {
    return -1;
  }

  const auto usageDiff = usageUsec - prevUsageUsec;
  const auto elapsedDiff = elapsedUsec - prevElapsedUsec;
  // Both counters are monotonic, so anything else means the cgroup was replaced
  // underneath us. Wait for the next window instead of reporting a spike.
  if (usageDiff < 0 || elapsedDiff <= 0) {
    return -1;
  }

  const double loadPct = static_cast<double>(usageDiff) /
      (static_cast<double>(elapsedDiff) * quotaCores) * 100;
  // CFS throttling is applied per period rather than instantaneously, so a
  // cgroup can slightly exceed its quota within a single window.
  return std::min(loadPct, 100.0);
}

void CPUMon::setCgroupFilesForTest(
    bool useV2,
    const std::string& usageFile,
    const std::string& quotaFile,
    const std::string& periodFile) {
  cgroupDetected_ = true;
  cgroupV2_ = useV2;
  cgroupUsageFile_ = usageFile;
  cgroupQuotaFile_ = quotaFile;
  cgroupPeriodFile_ = periodFile;
}

void CPUMon::updateCgroupCpuLoad(double hostLoadPct) {
  if (!cgroupDetected_) {
    detectCgroupFiles();
    cgroupDetected_ = true;
  }

  const double quotaCores = readCgroupCpuQuotaCores();
  const auto usageUsec = readCgroupCpuUsageUsec();
  const auto elapsedUsec = nowMonotonicUsec();
  const double loadPct = computeCgroupCpuLoadPct(
      prevCgroupUsageUsec_,
      usageUsec,
      prevCgroupElapsedUsec_,
      elapsedUsec,
      quotaCores);
  prevCgroupUsageUsec_ = usageUsec;
  prevCgroupElapsedUsec_ = elapsedUsec;

  if (quotaCores <= 0) {
    // No CPU limit applies, so the cgroup may use every core the machine has
    // and the host-wide load already answers 'how busy is this worker'. This is
    // the bare-metal case, and keeps the reported value unchanged there.
    cgroupCpuLoadPct_.store(hostLoadPct);
    return;
  }

  // A quota exists but this window produced no usable delta: the first sample,
  // or a counter that went backwards. Report idle rather than the host-wide
  // number, which is on a different scale and would misreport a busy worker.
  cgroupCpuLoadPct_.store(loadPct < 0 ? 0.0 : loadPct);
}

void CPUMon::update() {
  // We do this only for linux, other OS don't have this mechanism.
  // If needed, another mechanism can be added for other OS.
#ifdef __linux__
  double cpuUtil = 0.0;

  // Corner case: When parsing /proc/stat fails, set the cpuUtil to 0.
  std::vector<uint64_t> cur(8);
  if (readProcStat(cur)) {
    if (not firstTime_) {
      /**
       * The values in the /proc/stat is the CPU time since boot.
       * Columns [0, 1, ... 9] map to [user, nice, system, idle, iowait, irq,
       * softirq, steal, guest, guest_nice]. Guest related fields are not used
       * for the cpu util calculation. The total CPU time in the last
       * window is delta busy time over delta total time.
       */
      auto curUtil =
          cur[0] + cur[1] + cur[2] + cur[4] + cur[5] + cur[6] + cur[7];
      auto prevUtil = prev_[0] + prev_[1] + prev_[2] + prev_[4] + prev_[5] +
          prev_[6] + prev_[7];
      auto utilDiff = static_cast<double>(curUtil - prevUtil);
      auto totalDiff = utilDiff + cur[3] - prev_[3];

      /**
       * Corner case: If CPU didn't change or the proc/stat didn't get
       * updated or ticks didn't increase, set the cpuUtil to 0.
       */
      if (totalDiff < 0.001 || curUtil < prevUtil) {
        cpuUtil = 0.0;
      } else {
        // Corner case: The max of CPU utilization can be at most 100%.
        cpuUtil = std::min((utilDiff / totalDiff) * 100, 100.0);
      }
    } else {
      firstTime_ = false;
    }
    prev_ = std::move(cur);
  }
  cpuLoadPct_.store(cpuUtil);

  // '/proc/stat' is not namespaced, so the value above covers the whole machine
  // no matter how little of it this worker is allowed to use. Derive the
  // cgroup-scoped load as well, and let callers pick the one they need.
  updateCgroupCpuLoad(cpuUtil);
#endif
}

} // namespace facebook::presto
