#pragma once

// Everything that changes the system. Each process action pins the process
// with a pidfd, then re-reads /proc/<pid>/stat and refuses when the start time
// no longer matches the snapshot. Signals go through the pidfd; nice,
// affinity, ionice and OOM (calls that only take a number) pin every thread
// too and count a call as landed only if that thread was alive both before
// and after it, so a reused PID is never reached (see PerThread).

#include "core/Snapshot.h"

#include <QString>
#include <QVector>

namespace td::actions {

struct Result {
  bool ok = false;
  QString message;
};

Result Signal(const ProcessKey &key, int sig);
Result SetNice(const ProcessKey &key, int nice);       // every thread of the process
Result SetAffinity(const ProcessKey &key, const QVector<int> &cpus);
QVector<int> Affinity(const ProcessKey &key);         // combined over threads
QVector<int> OnlineCpus();                            // from /sys/devices/system/cpu/online
// I/O scheduling, as ionice: class 1 realtime, 2 best-effort (level 0 highest … 7), 3 idle.
struct IoPriority {
  int ioClass = 2;
  int level = 4;
};
Result SetIoPriority(const ProcessKey &key, IoPriority priority);  // every thread
IoPriority GetIoPriority(const ProcessKey &key);
// /proc/<pid>/oom_score_adj, -1000 (never kill) … 1000 (kill first). Raising is
// always allowed for one's own processes; lowering needs CAP_SYS_RESOURCE.
Result SetOomScoreAdj(const ProcessKey &key, int adj);
bool OomScoreAdj(const ProcessKey &key, int *adj);
// The systemd unit (service or scope) holding the process, from /proc/<pid>/cgroup.
// userManager: the unit belongs to the user's own systemd (user@UID.service).
QString SystemdUnit(int pid, bool *userManager);
// Units that must never be frozen or stopped from Regent: the session, the
// compositor and the shell (a frozen screen cannot be thawed from it), system
// plumbing, and Regent's own unit.
bool CriticalUnit(const QString &unit);
// Processes the desktop stands on (compositor, shell, polkit agent, portals) or
// Regent itself: never suspended, killed only after an explicit warning.
bool CriticalProcess(int pid);
Result RunTask(const QString &commandLine);           // QProcess::splitCommand + startDetached
Result OpenUserAccounts();                            // the desktop's account settings

// Desktop account-settings apps, tried in order: {program, argument}.
struct Launcher {
  const char *program;
  const char *argument;
};
const QVector<Launcher> &UserAccountLaunchers();

}  // namespace td::actions
