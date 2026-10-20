#include "core/Cgroups.h"

#include "core/Fs.h"

#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <unistd.h>

namespace td::cgroups {

namespace {

inline quint64 Delta(quint64 now, quint64 before) { return now >= before ? now - before : 0; }

// systemd escapes '-' inside a name component as \x2d; '-' itself separates components.
QString Unescape(const QString &s) {
  QString out;
  for (int i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x') {
      bool ok = false;
      const int c = s.mid(i + 2, 2).toInt(&ok, 16);
      if (ok) {
        out += QChar(c);
        i += 3;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

bool LooksLikeId(const QString &part) {
  if (part.size() < 4) return false;
  for (QChar c : part)
    if (!c.isDigit() && !(c >= 'a' && c <= 'f')) return false;
  return true;
}

quint64 Field(const std::string &text, const char *key) {
  size_t p = text.find(key);
  if (p == std::string::npos) return 0;
  return std::strtoull(text.c_str() + p + std::strlen(key), nullptr, 10);
}

}  // namespace

bool IsUnit(const QString &name) {
  return name.endsWith(".service") || name.endsWith(".scope") || name.endsWith(".slice");
}

QString DisplayName(const QString &unit) {
  // Slices are the hierarchy itself (user-1000, system-getty): keep their names whole.
  if (unit.endsWith(".slice")) return Unescape(unit.chopped(6));
  QString base = unit.section('.', 0, -2);
  if (!IsUnit(unit)) base = unit;
  base = base.section('@', 0, 0);
  QStringList parts = base.split('-', Qt::SkipEmptyParts);
  if (parts.isEmpty()) return Unescape(unit);
  static const QStringList launchers = {"Hyprland", "graphical", "gnome", "kde", "flatpak", "uwsm", "niri", "sway"};
  if (parts.size() >= 2 && parts.first() == "app") {
    parts.removeFirst();
    if (parts.size() >= 2 && launchers.contains(parts.first())) parts.removeFirst();
  }
  while (parts.size() > 1 && LooksLikeId(parts.last())) parts.removeLast();
  QString name = Unescape(parts.join('-'));
  // Reverse-DNS app ids (com.google.Chrome) read better by their last part.
  if (name.count('.') >= 2 && !name.contains(' ') && !name.contains(':')) name = name.section('.', -1);
  return name.isEmpty() ? Unescape(unit) : name;
}

Sampler::Sampler(QString root) : root_(std::move(root)) { logical_ = qMax(1L, sysconf(_SC_NPROCESSORS_ONLN)); }

QVector<Node> Sampler::Collect() {
  const auto now = std::chrono::steady_clock::now();
  const double interval = primed_ ? std::chrono::duration<double>(now - last_).count() : 0;
  QVector<Node> out;
  QHash<QString, Prev> cur;
  for (const QString &top : fs::list(root_)) {
    if (!IsUnit(top) || !QFileInfo(root_ + '/' + top).isDir()) continue;
    Walk(top, QString(), 1, interval, out, cur);
  }
  prev_.swap(cur);
  last_ = now;
  primed_ = true;
  return out;
}

void Sampler::Walk(const QString &rel, const QString &parent, int depth, double interval, QVector<Node> &out,
                   QHash<QString, Prev> &now) {
  const QString dir = root_ + '/' + rel;
  Node n;
  n.path = rel;
  n.name = rel.section('/', -1);
  n.parent = parent;
  n.userManager = rel.contains(QString("/user@%1.service/").arg(getuid()));
  n.foreign = !n.userManager && rel.contains("/user@") && !rel.endsWith(QString("/user@%1.service").arg(getuid()));
  Prev p;
  p.usageUsec = Field(fs::readRaw(dir + "/cpu.stat", 4096), "usage_usec ");
  // memory.current counts the page cache of every file the unit touched. Only
  // the file LRUs are cache the kernel can drop: "file" would also take in
  // tmpfs and shared memory, which stay in use until swapped.
  const qint64 mem = fs::readInt(dir + "/memory.current");
  const std::string stat = mem > 0 ? fs::readRaw(dir + "/memory.stat", 8192) : std::string();
  n.cache = Field(stat, "\nactive_file ") + Field(stat, "\ninactive_file ");
  n.memory = mem > 0 ? quint64(mem) - std::min<quint64>(quint64(mem), n.cache) : 0;
  n.tasks = int(fs::readInt(dir + "/pids.current"));
  if (n.tasks < 0) {
    // Without the pids controller, count the cgroup's own processes.
    std::string procs = fs::readRaw(dir + "/cgroup.procs", 1 << 16);
    n.tasks = int(std::count(procs.begin(), procs.end(), '\n'));
  }
  n.frozen = fs::readRaw(dir + "/cgroup.events", 512).find("frozen 1") != std::string::npos;
  std::string io = fs::readRaw(dir + "/io.stat", 1 << 16);
  if (fs::exists(dir + "/io.stat")) {
    p.io = true;
    size_t pos = 0;
    while ((pos = io.find("rbytes=", pos)) != std::string::npos) p.rbytes += std::strtoull(io.c_str() + (pos += 7), nullptr, 10);
    pos = 0;
    while ((pos = io.find("wbytes=", pos)) != std::string::npos) p.wbytes += std::strtoull(io.c_str() + (pos += 7), nullptr, 10);
  }
  auto before = prev_.constFind(rel);
  if (before != prev_.constEnd() && interval > 0) {
    n.cpu = qBound(0.0, Delta(p.usageUsec, before->usageUsec) / (interval * 1e6 * logical_) * 100.0, 100.0);
    if (p.io && before->io) {
      n.readBps = Delta(p.rbytes, before->rbytes) / interval;
      n.writeBps = Delta(p.wbytes, before->wbytes) / interval;
    }
  } else if (p.io) {
    n.readBps = n.writeBps = 0;
  }
  now.insert(rel, p);
  out << n;
  if (depth >= 12) return;
  for (const QString &child : fs::list(dir)) {
    if (!IsUnit(child) || !QFileInfo(dir + '/' + child).isDir()) continue;
    Walk(rel + '/' + child, rel, depth + 1, interval, out, now);
  }
}

}  // namespace td::cgroups
