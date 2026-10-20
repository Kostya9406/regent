#pragma once

// systemd services over D-Bus (org.freedesktop.systemd1), no systemctl.
// Loaded units come from ListUnits; installed-but-never-loaded ones from
// ListUnitFiles, so disabled services show up too.

#include <QDBusPendingCall>
#include <QString>
#include <QVector>

namespace td::services {

struct Service {
  QString name;
  QString description;
  QString load;     // loaded, not-found, masked, ...
  QString active;   // active, inactive, failed, activating
  QString sub;      // running, exited, dead, ...
  QString fileState;  // enabled, disabled, static, masked, ...
  uint mainPid = 0;
};

enum class Bus { System, User };

QVector<Service> List(Bus bus, QString *error);  // blocking; call off the GUI thread
// action: StartUnit, StopUnit, RestartUnit. Allows polkit's interactive
// prompt; asynchronous, so the GUI never blocks while polkit asks.
QDBusPendingCall Control(Bus bus, const QString &unit, const QString &action);
// FreezeUnit / ThawUnit (systemd 246+): the whole cgroup stops being scheduled, keeping its memory.
QDBusPendingCall Freeze(Bus bus, const QString &unit, bool freeze);
// Units in the failed state, any type; blocking (short timeout), empty when systemd is unreachable.
QStringList Failed(Bus bus, bool *ok = nullptr);  // ok: systemd answered

}  // namespace td::services
