#pragma once

// Threshold alerts shared by the window and `regent --stream`: a condition
// must hold for a while before it fires and clear with some margin, so a
// short spike never turns into a burst of notifications.

#include "core/Snapshot.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace td::alerts {

struct Thresholds {
  bool enabled = true;
  double cpuTempC = 90;     // for 10 s
  double memPressure = 20;  // PSI "some" avg10 percent, for 10 s
  double diskFreePct = 5;   // any real filesystem
  bool throttling = true;   // the CPU's thermal throttle counters move
  bool failedUnits = true;  // systemd units in the failed state
  double cpuPercent = 0;    // 0: off; otherwise whole-machine load held for 60 s
};
Thresholds FromSettings();  // the alert* keys of settings.json

enum class Level { Warn = 1, Critical = 2 };

struct Alert {
  QString id;     // stable: "temp", "disk:/home", "unit:foo.service"
  Level level = Level::Warn;
  QString title;  // "CPU at 96 °C"
  QString body;
  QString page;   // Regent page that explains it
  char32_t glyph = 0xF0026;  // Nerd Font alert
};

class Monitor {
public:
  explicit Monitor(Thresholds t = {}) : t_(t) {}
  void SetThresholds(const Thresholds &t) { t_ = t; }
  // A fresh list of failed units. The first one is the baseline: units that had
  // already failed (often at boot) are not news on every login; only units
  // that fail later, or fail again after recovering, raise an alert.
  void SetFailedUnits(const QStringList &units);
  // Feeds one snapshot; returns the alerts that became active now, for
  // notifications. Active() lists all current ones.
  QVector<Alert> Feed(const Snapshot &s);
  const QVector<Alert> &Active() const { return active_; }
  int Severity() const;  // 0 none, 1 warn, 2 critical

private:
  bool Held(const QString &id, bool condition, double seconds, double dt);
  Thresholds t_;
  QHash<QString, double> heldFor_;
  QVector<Alert> active_;
  qint64 lastThrottle_ = -1;
  QStringList failed_;
  QSet<QString> baseline_;
  double clock_ = 0;                  // seconds fed so far
  QHash<QString, double> notified_;   // unit alert id -> clock_ when last reported
  bool unitsPrimed_ = false;
  double sinceThrottle_ = 1e9;
};

}  // namespace td::alerts
