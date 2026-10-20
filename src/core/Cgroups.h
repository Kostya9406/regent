#pragma once

// Resources per systemd unit, straight from cgroup v2 accounting
// (/sys/fs/cgroup): unlike per-process sums it includes children that have
// already exited, and a whole app (every Firefox process) is one row.

#include <QHash>
#include <QString>
#include <QVector>
#include <chrono>

namespace td::cgroups {

struct Node {
  QString path;    // relative to the cgroup root: user.slice/user-1000.slice/…
  QString name;    // last component: app-firefox@1.scope
  QString parent;  // parent's path; empty for top-level nodes
  double cpu = 0;  // percent of the whole machine
  quint64 memory = 0;  // memory.current less the file cache: what the unit itself holds
  quint64 cache = 0;   // its page cache (memory.stat file), freed under pressure
  double readBps = -1, writeBps = -1;  // -1: the io controller is not enabled here
  int tasks = -1;
  bool frozen = false;
  bool userManager = false;  // below user@UID.service: the user's own systemd manages it
  bool foreign = false;      // below another user's user@N.service: names there are theirs to choose
};

// Human name for a unit: app-firefox@a1b2.scope -> firefox, app-graphical-foot@x.service -> foot.
QString DisplayName(const QString &unit);
bool IsUnit(const QString &name);  // .service, .scope or .slice

class Sampler {
public:
  explicit Sampler(QString root = QStringLiteral("/sys/fs/cgroup"));
  QVector<Node> Collect();  // parents before children

private:
  struct Prev {
    quint64 usageUsec = 0, rbytes = 0, wbytes = 0;
    bool io = false;
  };
  void Walk(const QString &rel, const QString &parent, int depth, double interval, QVector<Node> &out,
            QHash<QString, Prev> &now);
  QString root_;
  QHash<QString, Prev> prev_;
  std::chrono::steady_clock::time_point last_;
  bool primed_ = false;
  int logical_ = 1;
};

}  // namespace td::cgroups
