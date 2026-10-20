#include "core/Actions.h"

#include "core/Fs.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <functional>
#include <cerrno>
#include <signal.h>
#include <cstring>
#include <sched.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>

namespace td::actions {

namespace {

bool StillSame(const ProcessKey &key) {
  std::string st = fs::readRaw(QString("/proc/%1/stat").arg(key.pid), 4096);
  size_t rp = st.rfind(')');
  if (rp == std::string::npos) return false;
  // starttime is field 22; after ") " the state is field 3.
  const char *p = st.c_str() + rp + 2;
  for (int field = 3; field < 22 && *p; ++field) {
    p = std::strchr(p, ' ');
    if (!p) return false;
    ++p;
  }
  return std::strtoull(p, nullptr, 10) == key.start;
}

QString Errno(int e) {
  if (e == EPERM || e == EACCES) return QObject::tr("access denied");
  if (e == ESRCH) return QObject::tr("the process has exited");
  return QString::fromLocal8Bit(std::strerror(e));
}

// A pidfd names one task, not a number. An id is only freed when its task has
// died, so a task whose pidfd was open before a call by number and still shows
// it alive afterwards held that number throughout: the call reached it and
// nothing else. That is how the per-thread changes below avoid a reused PID.
class Pidfd {
public:
  Pidfd(int id, unsigned flags) : fd_(int(::syscall(SYS_pidfd_open, id, flags))), err_(fd_ < 0 ? errno : 0) {}
  ~Pidfd() {
    if (fd_ >= 0) ::close(fd_);
  }
  Pidfd(const Pidfd &) = delete;
  Pidfd &operator=(const Pidfd &) = delete;
  bool Valid() const { return fd_ >= 0; }
  int Error() const { return err_; }
  // Readable once the task has exited.
  bool Alive() const {
    pollfd p{fd_, POLLIN, 0};
    return fd_ >= 0 && ::poll(&p, 1, 0) == 0;
  }

private:
  int fd_, err_;
};

#ifndef PIDFD_THREAD
#define PIDFD_THREAD O_EXCL  // Linux 6.9: a pidfd for one thread
#endif

QVector<int> Threads(int pid);

// Runs op(tid) on every thread of the process, each call provably on that
// thread (see Pidfd). op returns 0 or an errno.
Result PerThread(const ProcessKey &key, const std::function<int(int)> &op) {
  Pidfd process(key.pid, 0);
  if (!process.Valid()) return {false, process.Error() == ESRCH ? QObject::tr("the process has exited") : Errno(process.Error())};
  if (!StillSame(key)) return {false, QObject::tr("the process has exited")};
  int done = 0, total = 0, lastErr = 0;
  QStringList strangers;
  for (int tid : Threads(key.pid)) {
    Pidfd thread(tid, PIDFD_THREAD);
    if (!thread.Valid()) continue;  // the thread ended before it was pinned
    // Pinned now; is it ours? Listed under our process, which is still alive
    // (so the number in that path still names our process).
    if (!fs::exists(QString("/proc/%1/task/%2").arg(key.pid).arg(tid)) || !process.Alive()) continue;
    ++total;
    const int err = op(tid);
    if (err == 0) ++done;
    else lastErr = err;
    if (!thread.Alive() && err == 0) {
      // The thread ended during the call. Only if its id now belongs to
      // another process could that process have been reached; say so.
      const std::string status = fs::readRaw(QString("/proc/%1/status").arg(tid), 4096);
      const size_t at = status.find("\nTgid:");
      if (at != std::string::npos && std::atoi(status.c_str() + at + 6) != key.pid) strangers << QString::number(tid);
    }
  }
  if (!process.Alive() && done == 0) return {false, QObject::tr("the process has exited")};
  if (done == 0) return {false, Errno(lastErr ? lastErr : ESRCH)};
  QString note = done == total ? QString() : QObject::tr("applied to %1 of %2 threads").arg(done).arg(total);
  if (!strangers.isEmpty())
    note = QObject::tr("a thread ended during the change; task %1 may have received it").arg(strangers.join(", "));
  return {true, note};
}

QVector<int> Threads(int pid) {
  QVector<int> out;
  for (const QString &t : fs::list(QString("/proc/%1/task").arg(pid))) out << t.toInt();
  if (out.isEmpty()) out << pid;
  return out;
}

}  // namespace

Result Signal(const ProcessKey &key, int sig) {
  // A pidfd pins the process: once open, a reused PID can no longer be hit.
  // Check the start time after opening it, then signal through it.
  int fd = int(::syscall(SYS_pidfd_open, key.pid, 0));
  if (fd < 0) {
    if (errno == ESRCH) return {false, QObject::tr("the process has exited")};
    // Kernel without pidfd: fall back to check-then-kill.
    if (!StillSame(key)) return {false, QObject::tr("the process has exited")};
    if (::kill(key.pid, sig) != 0) return {false, Errno(errno)};
    return {true, {}};
  }
  bool same = StillSame(key);
  int rc = same ? int(::syscall(SYS_pidfd_send_signal, fd, sig, nullptr, 0)) : -1;
  int err = errno;
  ::close(fd);
  if (!same) return {false, QObject::tr("the process has exited")};
  if (rc != 0) return {false, Errno(err)};
  return {true, {}};
}

QVector<int> OnlineCpus() {
  // "0-3,5,7-8"
  QVector<int> out;
  for (const QString &part : fs::readText("/sys/devices/system/cpu/online").split(',', Qt::SkipEmptyParts)) {
    QStringList r = part.split('-');
    int a = r.value(0).toInt(), b = r.size() > 1 ? r[1].toInt() : a;
    for (int i = a; i <= b && i < CPU_SETSIZE; ++i) out << i;
  }
  return out;
}

Result SetNice(const ProcessKey &key, int nice) {
  return PerThread(key, [nice](int tid) { return ::setpriority(PRIO_PROCESS, id_t(tid), nice) == 0 ? 0 : errno; });
}

Result SetAffinity(const ProcessKey &key, const QVector<int> &cpus) {
  if (cpus.isEmpty()) return {false, QObject::tr("choose at least one processor")};
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int c : cpus) CPU_SET(c, &set);
  return PerThread(key, [&set](int tid) { return ::sched_setaffinity(tid, sizeof set, &set) == 0 ? 0 : errno; });
}

QVector<int> Affinity(const ProcessKey &key) {
  cpu_set_t all;
  CPU_ZERO(&all);
  for (int tid : Threads(key.pid)) {
    cpu_set_t s;
    CPU_ZERO(&s);
    if (::sched_getaffinity(tid, sizeof s, &s) == 0) CPU_OR(&all, &all, &s);
  }
  QVector<int> out;
  for (int i = 0; i < CPU_SETSIZE; ++i)
    if (CPU_ISSET(i, &all)) out << i;
  return out;
}

Result SetIoPriority(const ProcessKey &key, IoPriority priority) {
  if (priority.ioClass < 1 || priority.ioClass > 3) return {false, QObject::tr("unknown I/O class")};
  const int level = priority.ioClass == 3 ? 0 : std::clamp(priority.level, 0, 7);
  const int value = (priority.ioClass << 13) | level;  // IOPRIO_PRIO_VALUE
  return PerThread(key, [value](int tid) {
    return ::syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, tid, value) == 0 ? 0 : errno;
  });
}

IoPriority GetIoPriority(const ProcessKey &key) {
  long v = ::syscall(SYS_ioprio_get, 1, key.pid);
  IoPriority p;
  if (v < 0) return p;
  int cls = int(v >> 13), level = int(v & 0x1fff);
  // Class 0 ("none") follows the CPU nice value: best-effort at (nice + 20) / 5.
  if (cls == 0) {
    errno = 0;
    int nice = ::getpriority(PRIO_PROCESS, id_t(key.pid));
    return {2, errno ? 4 : std::clamp((nice + 20) / 5, 0, 7)};
  }
  return {cls, level};
}

Result SetOomScoreAdj(const ProcessKey &key, int adj) {
  Pidfd process(key.pid, 0);
  if (!process.Valid() || !StillSame(key)) return {false, QObject::tr("the process has exited")};
  adj = std::clamp(adj, -1000, 1000);
  // An open /proc file stays bound to the task it was opened for. Opened while
  // our process is provably alive (still alive just after), it is our process's.
  QFile f(QString("/proc/%1/oom_score_adj").arg(key.pid));
  if (!f.open(QIODevice::WriteOnly | QIODevice::Unbuffered)) return {false, QObject::tr("access denied")};
  if (!process.Alive()) return {false, QObject::tr("the process has exited")};
  const QByteArray text = QByteArray::number(adj);
  if (f.write(text) != text.size())
    return {false, adj < 0 ? QObject::tr("lowering needs administrator rights (CAP_SYS_RESOURCE)") : f.errorString()};
  return {true, {}};
}

bool OomScoreAdj(const ProcessKey &key, int *adj) {
  std::string t = fs::readRaw(QString("/proc/%1/oom_score_adj").arg(key.pid), 32);
  if (t.empty()) return false;
  *adj = std::atoi(t.c_str());
  return true;
}

QString SystemdUnit(int pid, bool *userManager) {
  // cgroup v2: "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-foo@x.scope"
  std::string raw = fs::readRaw(QString("/proc/%1/cgroup").arg(pid), 4096);
  QString path;
  for (const QString &line : QString::fromUtf8(raw).split('\n'))
    if (line.startsWith("0::")) path = line.mid(3);
  const QStringList parts = path.split('/', Qt::SkipEmptyParts);
  const QString manager = QString("user@%1.service").arg(getuid());
  // Another user's manager delegates its subtree to that user, who can name
  // cgroups anything ("sshd.service"): such a name must never reach our system
  // or user systemd as if it were ours.
  for (const QString &part : parts)
    if (part.startsWith("user@") && part != manager) {
      if (userManager) *userManager = false;
      return {};
    }
  const int managerAt = parts.indexOf(manager);
  QString unit;
  int unitAt = -1;
  for (int i = 0; i < parts.size(); ++i)
    if (parts[i].endsWith(".service") || parts[i].endsWith(".scope")) {
      unit = parts[i];
      unitAt = i;
    }
  if (userManager) *userManager = managerAt >= 0 && unitAt > managerAt;
  return unit;
}

namespace {
// comm names of what a Wayland desktop session cannot lose without freezing or locking up.
const QStringList &DesktopComms() {
  static const QStringList names = {"Hyprland", "Xwayland", "quickshell", "qs", "sddm", "pipewire", "wireplumber",
                                    "hyprpolkitagent", "polkitd", "polkit-gnome-au", "xdg-desktop-por",
                                    "dbus-broker", "dbus-daemon", "systemd", "systemd-logind"};
  return names;
}
}  // namespace

bool CriticalProcess(int pid) {
  if (pid <= 1 || pid == int(::getpid())) return true;
  const QString comm = fs::readText(QString("/proc/%1/comm").arg(pid));
  return DesktopComms().contains(comm) || comm.startsWith("xdg-desktop-por");
}

bool CriticalUnit(const QString &unit) {
  if (unit.isEmpty() || unit.endsWith(".slice") || unit == "init.scope" || unit.startsWith("session-") ||
      unit.startsWith("user@") || unit.startsWith("dbus") || unit.startsWith("systemd-") ||
      unit.startsWith("polkit") || unit.startsWith("xdg-desktop-portal") || unit.startsWith("wayland-wm") ||
      unit.contains("polkit") || unit.startsWith("display-manager") || unit.startsWith("sddm"))
    return true;
  if (unit == SystemdUnit(int(::getpid()), nullptr)) return true;
  for (const QString &d : fs::list("/proc")) {
    if (d.isEmpty() || !d[0].isDigit()) continue;
    const QString comm = fs::readText("/proc/" + d + "/comm");
    if ((DesktopComms().contains(comm) || comm.startsWith("xdg-desktop-por")) && SystemdUnit(d.toInt(), nullptr) == unit)
      return true;
  }
  return false;
}

Result RunTask(const QString &commandLine) {
  QStringList argv = QProcess::splitCommand(commandLine.trimmed());
  if (argv.isEmpty()) return {false, QObject::tr("nothing to run")};
  QString program = argv.takeFirst();
  if (!QProcess::startDetached(program, argv, QString()))
    return {false, QObject::tr("%1 could not be started").arg(program)};
  return {true, {}};
}

const QVector<Launcher> &UserAccountLaunchers() {
  static const QVector<Launcher> l = {
      {"gnome-control-center", "user-accounts"},
      {"systemsettings", "kcm_users"},
      {"systemsettings5", "kcm_users"},
      {"cinnamon-settings", "user"},
      {"mate-users-admin", ""},
      {"lxqt-admin-user", ""},
      {"users-admin", ""},
  };
  return l;
}

Result OpenUserAccounts() {
  for (const Launcher &l : UserAccountLaunchers()) {
    QString exe = QStandardPaths::findExecutable(QString::fromUtf8(l.program));
    if (exe.isEmpty()) continue;
    QStringList args;
    if (*l.argument) args << QString::fromUtf8(l.argument);
    if (QProcess::startDetached(exe, args, QString())) return {true, {}};
    return {false, QObject::tr("%1 could not be started").arg(exe)};
  }
  return {false, QObject::tr("No account settings app is installed.")};
}

}  // namespace td::actions
