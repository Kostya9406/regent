#include "core/Settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace td {

Settings &Settings::Get() {
  static Settings s;
  return s;
}

Settings::Settings() {
  // Regent was called TaskDeck: carry its settings over once.
  const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
  const QString old = config + "/taskdeck/settings.json";
  if (!QFile::exists(path()) && QFile::exists(old) && QDir().mkpath(QFileInfo(path()).path()))
    QFile::copy(old, path());
  Reload();
}

void Settings::Reload() {
  const QDateTime mtime = QFileInfo(path()).lastModified();
  if (mtime.isValid() && mtime == loadedMtime_) return;
  QFile f(path());
  if (f.open(QIODevice::ReadOnly)) obj_ = QJsonDocument::fromJson(f.readAll()).object();
  loadedMtime_ = mtime;
}

QString Settings::path() const {
  return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
         "/regent/settings.json";
}

void Settings::Save() const {
  QDir().mkpath(QFileInfo(path()).absolutePath());
  QSaveFile f(path());
  if (!f.open(QIODevice::WriteOnly)) return;
  f.write(QJsonDocument(obj_).toJson());
  f.commit();
  loadedMtime_ = QFileInfo(path()).lastModified();
}

QVariant Settings::value(const QString &key, const QVariant &fallback) const {
  return obj_.contains(key) ? obj_.value(key).toVariant() : fallback;
}

void Settings::set(const QString &key, const QVariant &v) {
  if (obj_.value(key).toVariant() == v) return;
  obj_.insert(key, QJsonValue::fromVariant(v));
  Save();
  emit Changed(key);
}

QString Settings::updateSpeed() const { return value("updateSpeed", "normal").toString(); }

double Settings::historyIntervalSec() {
  if (int ms = intervalMs(); ms > 0) lastRunningMs_ = ms;
  return lastRunningMs_ / 1000.0;
}

int Settings::intervalMs() const {
  QString s = updateSpeed();
  if (s == "high") return 500;
  if (s == "low") return 2000;
  if (s == "paused") return 0;
  return 1000;
}

bool Settings::fahrenheit() const { return value("temperatureUnit", "celsius") == "fahrenheit"; }
bool Settings::smoothGraphCurves() const { return value("smoothGraphCurves", true).toBool(); }
bool Settings::processTreeEnabled() const { return processGrouping() == "tree"; }
QString Settings::processGrouping() const {
  const QString g = value("processGrouping").toString();
  if (g == "apps" || g == "user" || g == "tree") return g;
  return value("processTreeEnabled", false).toBool() ? "tree" : "apps";  // the older switch
}
bool Settings::alwaysOnTop() const { return value("alwaysOnTop", false).toBool(); }
bool Settings::sidebarHidden() const { return value("sidebarHidden", false).toBool(); }
QString Settings::defaultPage() const { return value("defaultPage", "summary").toString(); }
int Settings::graphSeconds() const { return value("graphSeconds", 60).toInt(); }

QString Settings::FormatTemp(double c, int precision) const {
  if (c < 0) return QStringLiteral("—");
  if (fahrenheit()) return QString::number(c * 9 / 5 + 32, 'f', precision) + " °F";
  return QString::number(c, 'f', precision) + " °C";
}

}  // namespace td
