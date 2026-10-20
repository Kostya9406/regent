#include "core/Fs.h"

#include <QDir>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace td::fs {

std::string readRaw(const char *path, size_t limit) {
  std::string out;
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return out;
  char buf[8192];
  while (out.size() < limit) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    out.append(buf, size_t(n));
  }
  ::close(fd);
  return out;
}

QString readText(const QString &path) {
  return QString::fromUtf8(readRaw(path, 64 * 1024)).trimmed();
}

qint64 readInt(const QString &path, qint64 fallback) {
  std::string s = readRaw(path, 64);
  if (s.empty()) return fallback;
  char *end = nullptr;
  long long v = std::strtoll(s.c_str(), &end, 10);
  return end == s.c_str() ? fallback : v;
}

bool exists(const QString &path) {
  struct stat st;
  return ::stat(path.toLocal8Bit().constData(), &st) == 0;
}

QStringList list(const QString &dir) {
  QStringList out;
  DIR *d = ::opendir(dir.toLocal8Bit().constData());
  if (!d) return out;
  while (dirent *e = ::readdir(d)) {
    if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
      continue;
    out << QString::fromLocal8Bit(e->d_name);
  }
  ::closedir(d);
  return out;
}

QString humanBytes(double bytes, int precision) {
  static const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  if (bytes < 0) return QStringLiteral("—");
  int u = 0;
  while (bytes >= 1024 && u < 5) {
    bytes /= 1024;
    ++u;
  }
  return QString::number(bytes, 'f', u == 0 ? 0 : precision) + ' ' + units[u];
}

QString humanRate(double bps) {
  if (bps < 0) return QStringLiteral("—");
  if (bps < 1024) return QString::number(bps, 'f', 0) + " B/s";
  return humanBytes(bps, bps < 10 * 1024 * 1024 ? 1 : 0) + "/s";
}

QString humanDuration(double s) {
  qint64 t = qint64(s);
  qint64 d = t / 86400, h = (t / 3600) % 24, m = (t / 60) % 60, sec = t % 60;
  // "0:05:23:52" left the reader to guess that the first field is days.
  if (d > 0) return QString("%1 d %2 h %3 min").arg(d).arg(h).arg(m);
  if (h > 0) return QString("%1 h %2 min").arg(h).arg(m);
  if (m > 0) return QString("%1 min %2 s").arg(m).arg(sec);
  return QString("%1 s").arg(sec);
}

}  // namespace td::fs
