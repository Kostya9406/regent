#pragma once

// Small, allocation-light readers for procfs and sysfs. Every function
// returns an empty/zero value when the file is missing or unreadable;
// callers decide what "unavailable" looks like.

#include <QString>
#include <QStringList>
#include <string>

namespace td::fs {

std::string readRaw(const char *path, size_t limit = 1 << 20);
inline std::string readRaw(const QString &path, size_t limit = 1 << 20) {
  return readRaw(path.toLocal8Bit().constData(), limit);
}
QString readText(const QString &path);           // trimmed
qint64 readInt(const QString &path, qint64 fallback = -1);
bool exists(const QString &path);
QStringList list(const QString &dir);            // entry names, no . or ..
QString humanBytes(double bytes, int precision = 1);
QString humanRate(double bytesPerSec);
QString humanDuration(double seconds);

}  // namespace td::fs
