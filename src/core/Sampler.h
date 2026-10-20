#pragma once

// Collects one Snapshot per call from procfs/sysfs. Holds the previous
// counters so rates (CPU %, bytes/sec) are deltas between two Collect()
// calls. Not thread-safe: one Sampler belongs to one sampling thread.

#include "core/Snapshot.h"

#include <QHash>
#include <QSet>
#include <chrono>
#include <sys/types.h>
#include <unordered_map>

namespace td {

class Sampler {
public:
  Sampler();
  SnapshotPtr Collect();
  // Per-process extras (wakeups, GPU clients, window titles) cost a few ms per
  // sample; the bar's --stream does without them.
  void SetDetail(bool on) { detail_ = on; }

private:
  struct CpuTimes {
    quint64 total = 0, idle = 0, kernel = 0;
  };
  struct ProcPrev {
    quint64 ticks = 0, readBytes = 0, writeBytes = 0;
    bool io = false;
    QHash<int, quint64> switches;  // schedstat timeslices by thread id
    bool hasSwitches = false;
    std::chrono::steady_clock::time_point switchesAt;  // when switches was read
    double wakeups = -1;   // last rate, kept between the samples that read it
    int fds = -1;          // last count, kept between scans of /proc/<pid>/fd
    QString cmdline;       // kept between rereads
    QVector<int> drmFds;   // fds that point at /dev/dri/*, rescanned every few samples
    QHash<QString, quint64> engineNs;  // "pdev/client|engine" -> busy ns (or xe cycles)
  };
  struct DiskPrev {
    quint64 rd = 0, wr = 0, ticks = 0;
  };
  struct NetPrev {
    quint64 rx = 0, tx = 0;
  };

  void ReadStatic(Snapshot &s);
  void ReadCpu(Snapshot &s);
  void ReadMemory(Snapshot &s);
  void ReadDisks(Snapshot &s);
  void ReadNets(Snapshot &s);
  void ReadGpus(Snapshot &s);
  void ReadSensors(Snapshot &s);
  void ReadPower(Snapshot &s);
  void ReadProcesses(Snapshot &s);
  void MarkWindowedProcesses(Snapshot &s);
  void ReadThrottle(Snapshot &s);
  void ReadVolumes(Snapshot &s);
  void ReadGpuClients(ProcessInfo &p, ProcPrev &now, const ProcPrev *prev, double intervalSec);
  void FinishGpus(Snapshot &s);  // after the processes: per-GPU load from their DRM clients
  void ReadNvidiaSmi(Snapshot &s);
  QString UserName(uid_t uid);

  quint64 generation_ = 0;
  std::chrono::steady_clock::time_point last_;
  bool primed_ = false;
  long ticksPerSec_ = 100;
  long pageSize_ = 4096;

  QVector<CpuTimes> cpuPrev_;  // [0] is the aggregate line
  std::unordered_map<ProcessKey, ProcPrev, ProcessKeyHash> procPrev_;
  QHash<QString, DiskPrev> diskPrev_;
  QHash<QString, NetPrev> netPrev_;
  QHash<uid_t, QString> users_;
  struct WindowRef {
    QString cls, title, address, workspace;
  };
  QHash<int, WindowRef> windows_;  // by pid: the first window each process owns
  bool detail_ = true;
  int drmScanCountdown_ = 0;
  quint64 processPass_ = 0;  // ReadProcesses calls: staggers the costly per-process reads
  int volumeCountdown_ = 0;
  QVector<Snapshot::Volume> volumes_;
  quint64 raplEnergy_ = 0;
  std::chrono::steady_clock::time_point raplAt_;
  int windowPollCountdown_ = 0;

  // Read once: these do not change while we run.
  QString cpuModel_, kernel_, distro_, hostname_;
  int logical_ = 0, physical_ = 0, sockets_ = 0;
  double baseMhz_ = 0, maxMhz_ = 0;
  bool virt_ = false, vm_ = false;
  QVector<GpuInfo> gpuStatic_;
  QHash<QString, QHash<QString, double>> gpuEngines_;  // pdev (or driver:name) -> engine -> percent, this sample
  QSet<QString> gpuClientsSeen_;                       // device/client-id already counted this sample
  QString nvidiaSmi_;  // path, once looked up; empty when absent
  bool nvidiaLooked_ = false;
  std::chrono::steady_clock::time_point nvidiaNext_;  // next query; far off after a failure
  QHash<QString, QStringList> nvidia_;  // last answer, by bus id (the last 7 characters)
  QVector<pid_t> nvidiaStuck_;          // killed queries not yet reaped
};

}  // namespace td
