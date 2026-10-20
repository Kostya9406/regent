#pragma once

#include <QString>
#include <QVector>
#include <atomic>

class QOffscreenSurface;

namespace td::benchmarks {

// New kinds go at the end: results and dashboards index by the enum value.
enum class Kind { Cpu, Memory, Disk, Gpu, Internet, CpuAll, DiskDirect, Sustained };

struct Result {
  Kind kind = Kind::Cpu;
  double seconds = 0;
  double score = 0;
  double secondaryScore = -1;  // disk read MiB/s; primary is write MiB/s
  double secondarySeconds = 0;
  quint64 checksum = 0;
  bool cancelled = false;
  QString error;
  QString detail;
};

// Runs on the caller's thread. Durations are clamped to 10 ms–10 s.
// CPU uses one thread; memory copies between two 16 MiB buffers.
// Cancellation is checked between small batches, including initialization.
Result Run(Kind kind, int durationMs, const std::atomic<bool> &cancel);

// A private, automatically removed file; at most 32 MiB written/read.
// Scores describe cached sequential I/O, without a blocking fsync or cache drop.
Result RunDisk(const QString &directory, int durationMs, const std::atomic<bool> &cancel);

// The caller must create/destroy the surface on the GUI thread and keep it
// alive until this worker-thread call returns. No CPU fallback is scored.
Result RunGpu(QOffscreenSurface *surface, int durationMs, const std::atomic<bool> &cancel);

// The single-thread CPU workload on `threads` threads at once (0: one per
// online CPU); the score is the sum. Detail names the thread count.
Result RunCpuAll(int durationMs, const std::atomic<bool> &cancel, int threads = 0);

// Direct I/O (O_DIRECT, then fdatasync): the page cache is bypassed, so the
// drive itself is measured. Up to 256 MiB in a private file that is removed
// afterwards; filesystems without O_DIRECT (tmpfs) report an error instead.
Result RunDiskDirect(const QString &directory, int durationMs, const std::atomic<bool> &cancel);

// All-core load for durationMs (up to 5 minutes), for watching the CPU slow
// down as it heats. `progress` (optional) grows with the iterations done so a
// caller can sample the rate every second from another thread.
Result RunSustained(int durationMs, const std::atomic<bool> &cancel, std::atomic<quint64> *progress);

// One second of a sustained run, with what the system reported meanwhile.
struct ThrottleSample {
  double t = 0;       // seconds since start
  double rate = 0;    // M iterations/s during that second
  double mhz = -1;    // average CPU clock
  double tempC = -1;
  qint64 throttle = -1;  // cumulative thermal throttle events
};
struct ThrottleVerdict {
  bool throttles = false;
  double dropPct = 0;     // last third against the first seconds
  double dropAtSec = -1;  // when the rate first stayed 10% below its start
  double maxTempC = -1;
  qint64 events = 0;      // throttle events during the run
  QString text;
};
ThrottleVerdict Judge(const QVector<ThrottleSample> &samples);

}  // namespace td::benchmarks
