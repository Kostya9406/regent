#pragma once

// One immutable picture of the machine, published by the SamplingEngine.
// Pages only ever read a Snapshot; nothing in the UI touches procfs directly.

#include <QDateTime>
#include <QHashFunctions>
#include <QString>
#include <QVector>
#include <memory>
#include <sys/types.h>

namespace td {

struct ProcessKey {
  int pid = 0;
  quint64 start = 0;  // starttime from /proc/<pid>/stat, guards against PID reuse
  bool operator==(const ProcessKey &o) const { return pid == o.pid && start == o.start; }
};

// Hash the full pair; folding it into one 64-bit word could collide.
inline size_t qHash(const ProcessKey &k, size_t seed = 0) noexcept { return qHashMulti(seed, k.pid, k.start); }
struct ProcessKeyHash {
  size_t operator()(const ProcessKey &k) const noexcept { return qHash(k); }
};

struct ProcessInfo {
  ProcessKey key;
  int ppid = 0;
  uid_t uid = 0;
  QString user;
  QString name;
  QString cmdline;
  QChar state;
  double cpu = 0;          // percent of the whole machine
  quint64 rss = 0;         // bytes, resident: shared libraries count in every process mapping them
  quint64 anon = 0;        // bytes, RssAnon: memory only this process holds
  quint64 vsize = 0;       // bytes
  double readBps = -1;     // -1: /proc/<pid>/io is not readable
  double writeBps = -1;
  int threads = 0;
  int nice = 0;
  int fds = -1;            // -1: not ours to count
  bool hasWindow = false;  // set from the compositor (Hyprland IPC)
  // Detail sampling only (the window, not --stream):
  double wakeups = -1;     // times scheduled in per second, all threads; -1: not sampled
  double gpu = -1;         // busiest DRM engine, percent; -1: no GPU client
  quint64 gpuMemory = 0;   // resident GPU buffers, bytes
  QString windowClass;     // the process's own top-level window, if any
  QString windowTitle;
  QString windowAddress;   // Hyprland address, 0x…
  QString workspace;       // workspace name
};

struct CpuCore {
  double util = 0;
  double mhz = 0;
};

struct DiskInfo {
  QString name;    // nvme0n1
  QString model;
  quint64 size = 0;
  double readBps = 0;
  double writeBps = 0;
  double busy = 0;  // 0..100, from io_ticks
  bool rotational = false;
};

struct NetInfo {
  QString name;
  QString ipv4;
  bool up = false;
  bool physical = false;
  bool wireless = false;
  qint64 speedMbps = -1;
  double rxBps = 0;
  double txBps = 0;
};

struct GpuInfo {
  QString name;
  QString driver;
  QString card;          // card0
  QString pciAddress;    // 0000:00:02.0, or the platform device's name
  double util = -1;      // -1: no source for it (see utilSource)
  // Where util comes from: "sysfs" (amdgpu gpu_busy_percent), "clients" (the
  // busiest engine summed over the DRM fdinfo of our own processes: i915, xe,
  // amdgpu, msm, panfrost…), "nvidia-smi" (NVIDIA's proprietary driver).
  QString utilSource;
  bool asleep = false;   // runtime-suspended: nothing was read, so it is not woken up
  double vramUsed = -1;  // bytes
  double vramTotal = -1;
  double tempC = -1;
  double mhz = -1;
};

struct Sensor {
  QString chip;
  QString label;
  QString kind;  // temp, fan, power, in
  double value = 0;
};

struct PowerInfo {
  bool hasBattery = false;
  bool onAc = true;
  int batteryPct = -1;
  QString batteryStatus;
  double batteryW = -1;
  double cpuPackageW = -1;  // RAPL, -1 when energy_uj is root-only
  bool raplRestricted = false;
  QString governor;
  QString epp;
  QString driver;
};

struct Snapshot {
  quint64 generation = 0;
  QDateTime time;
  double intervalSec = 1;

  QString cpuModel;
  int logical = 0;
  int physicalCores = 0;
  int sockets = 0;
  double baseMhz = 0;
  double maxMhz = 0;
  double cpuMhz = 0;
  double cpuUtil = 0;
  double cpuKernel = 0;
  double cpuTemp = -1;
  QVector<CpuCore> cores;
  bool virtualization = false;
  bool virtualMachine = false;

  quint64 memTotal = 0, memAvailable = 0, memCached = 0, memBuffers = 0;
  quint64 swapTotal = 0, swapFree = 0;
  double memPressure = 0;  // PSI some avg10
  double cpuPressure = 0;

  QVector<DiskInfo> disks;
  QVector<NetInfo> nets;
  QVector<GpuInfo> gpus;
  QVector<Sensor> sensors;
  PowerInfo power;
  qint64 throttleEvents = -1;  // package + core thermal throttle count summed over CPUs; -1: no counters
  struct Volume {
    QString mount;
    quint64 size = 0, avail = 0;
  };
  QVector<Volume> volumes;     // real filesystems; refreshed every few samples

  QVector<ProcessInfo> processes;
  int threadCount = 0;
  double uptimeSec = 0;
  double load[3] = {0, 0, 0};
  QString primaryIp;
  QString kernel;
  QString distro;
  QString hostname;

  quint64 memUsed() const { return memTotal > memAvailable ? memTotal - memAvailable : 0; }
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

}  // namespace td
