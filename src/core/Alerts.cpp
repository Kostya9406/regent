#include "core/Alerts.h"

#include "core/Fs.h"
#include "core/Settings.h"

#include <QObject>

#include <algorithm>

namespace td::alerts {

Thresholds FromSettings() {
  const Settings &st = Settings::Get();
  Thresholds t;
  t.enabled = st.value("alertsEnabled", true).toBool();
  t.cpuTempC = st.value("alertCpuTempC", t.cpuTempC).toDouble();
  t.memPressure = st.value("alertMemPressure", t.memPressure).toDouble();
  t.diskFreePct = st.value("alertDiskFreePct", t.diskFreePct).toDouble();
  t.throttling = st.value("alertThrottling", t.throttling).toBool();
  t.failedUnits = st.value("alertFailedUnits", t.failedUnits).toBool();
  t.cpuPercent = st.value("alertCpuPercent", t.cpuPercent).toDouble();
  return t;
}

bool Monitor::Held(const QString &id, bool condition, double seconds, double dt) {
  double &held = heldFor_[id];
  held = condition ? held + dt : 0;
  return condition && held >= seconds;
}

int Monitor::Severity() const {
  int level = 0;
  for (const Alert &a : active_) level = qMax(level, int(a.level));
  return level;
}

void Monitor::SetFailedUnits(const QStringList &units) {
  const QSet<QString> now(units.begin(), units.end());
  if (!unitsPrimed_) {
    baseline_ = now;
    unitsPrimed_ = true;
  }
  baseline_.intersect(now);  // a baseline unit that recovered counts again next time it fails
  failed_.clear();
  for (const QString &u : units)
    if (!baseline_.contains(u)) failed_ << u;
}

QVector<Alert> Monitor::Feed(const Snapshot &s) {
  QVector<Alert> now;
  if (!t_.enabled) {
    active_.clear();
    return {};
  }
  const double dt = qBound(0.0, s.intervalSec, 30.0);
  clock_ += dt;
  auto was = [this](const QString &id) {
    return std::any_of(active_.begin(), active_.end(), [&](const Alert &a) { return a.id == id; });
  };
  // Once on, a condition stays on until it is clearly over (hysteresis).
  if (s.cpuTemp > 0 && Held("temp", s.cpuTemp >= t_.cpuTempC - (was("temp") ? 5 : 0), 10, dt))
    now << Alert{"temp", Level::Critical, QObject::tr("CPU at %1").arg(Settings::Get().FormatTemp(s.cpuTemp)),
                 QObject::tr("The processor has been above %1 for a while.").arg(Settings::Get().FormatTemp(t_.cpuTempC)),
                 "power-and-freq", 0xF0E01};  // thermometer-alert

  if (t_.throttling && s.throttleEvents >= 0) {
    if (lastThrottle_ >= 0 && s.throttleEvents > lastThrottle_) sinceThrottle_ = 0;
    else sinceThrottle_ += dt;
    lastThrottle_ = s.throttleEvents;
    if (sinceThrottle_ < 60)
      now << Alert{"throttle", Level::Warn, QObject::tr("CPU is thermal throttling"),
                   QObject::tr("The processor slowed itself down to stay within its temperature limit."),
                   "power-and-freq", 0xF0F86};  // speedometer-slow
  }

  const double availPct = s.memTotal ? 100.0 * s.memAvailable / s.memTotal : 100;
  if (Held("memfull", availPct < (was("memfull") ? 6 : 3), 5, dt))
    now << Alert{"memfull", Level::Critical, QObject::tr("Memory almost full"),
                 QObject::tr("%1 available of %2.").arg(fs::humanBytes(s.memAvailable), fs::humanBytes(s.memTotal)),
                 "processes", 0xF035B};  // memory
  else if (Held("pressure", s.memPressure >= t_.memPressure * (was("pressure") ? 0.5 : 1), 10, dt))
    now << Alert{"pressure", Level::Warn, QObject::tr("Memory pressure %1%").arg(s.memPressure, 0, 'f', 0),
                 QObject::tr("Tasks are waiting for memory; the system may feel slow."), "processes", 0xF035B};

  for (const Snapshot::Volume &v : s.volumes) {
    if (!v.size) continue;
    const QString id = "disk:" + v.mount;
    const double freePct = 100.0 * v.avail / v.size;
    if (freePct < t_.diskFreePct + (was(id) ? 1 : 0))
      now << Alert{id, freePct < 1 ? Level::Critical : Level::Warn,
                   QObject::tr("%1 almost full").arg(v.mount),
                   QObject::tr("%1 free of %2.").arg(fs::humanBytes(v.avail), fs::humanBytes(v.size)), "disk-space",
                   0xF02CA};  // harddisk
  }

  if (t_.failedUnits)
    for (const QString &unit : failed_)
      now << Alert{"unit:" + unit, Level::Warn, QObject::tr("%1 failed").arg(unit),
                   QObject::tr("A systemd unit is in the failed state."), "services", 0xF0028};  // alert-circle

  if (t_.cpuPercent > 0 && Held("cpu", s.cpuUtil >= t_.cpuPercent - (was("cpu") ? 10 : 0), 60, dt))
    now << Alert{"cpu", Level::Warn, QObject::tr("CPU at %1% for a minute").arg(s.cpuUtil, 0, 'f', 0),
                 QObject::tr("Something keeps the processor busy."), "processes", 0xF0EE0};  // cpu-64-bit

  QVector<Alert> fresh;
  for (const Alert &a : now) {
    if (was(a.id)) continue;
    // A unit in a restart loop flips between failed and not: report it once per ten minutes.
    if (a.id.startsWith("unit:")) {
      auto last = notified_.constFind(a.id);
      if (last != notified_.constEnd() && clock_ - *last < 600) continue;
      notified_[a.id] = clock_;
    }
    fresh << a;
  }
  active_ = now;
  return fresh;
}

}  // namespace td::alerts
