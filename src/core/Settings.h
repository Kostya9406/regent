#pragma once

// ~/.config/regent/settings.json: updateSpeed, temperatureUnit,
// processTreeEnabled, alert*, flightAutoRecord, … (see the accessors and the
// Settings dialog for each option's meaning).

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>

namespace td {

class Settings : public QObject {
  Q_OBJECT
public:
  static Settings &Get();

  QString updateSpeed() const;  // high | normal | low | paused
  int intervalMs() const;
  // Seconds per graph sample for sizing histories: the current interval, or
  // while paused the last running one, so a manual refresh after a long pause
  // does not shrink (and so truncate) the graphs.
  double historyIntervalSec();
  bool fahrenheit() const;
  bool smoothGraphCurves() const;
  bool processTreeEnabled() const;  // processGrouping() == "tree"
  QString processGrouping() const;  // Processes: "apps" (windows first), "user" or "tree" (by parent)
  bool alwaysOnTop() const;
  bool sidebarHidden() const;
  QString defaultPage() const;
  int graphSeconds() const;

  QVariant value(const QString &key, const QVariant &fallback = {}) const;
  void set(const QString &key, const QVariant &v);
  QString path() const;
  // Re-reads the file when another Regent process changed it (the bar's
  // --stream follows thresholds set in the window). Emits nothing.
  void Reload();

  QString FormatTemp(double celsius, int precision = 0) const;

signals:
  void Changed(const QString &key);

private:
  Settings();
  void Save() const;
  QJsonObject obj_;
  mutable QDateTime loadedMtime_;  // of the file as last read or written
  int lastRunningMs_ = 1000;
};

}  // namespace td
