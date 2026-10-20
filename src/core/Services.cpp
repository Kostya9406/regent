#include "core/Services.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QFileInfo>
#include <QHash>

namespace td::services {

namespace {

struct UnitRow {
  QString name, description, load, active, sub, following;
  QDBusObjectPath path;
  uint jobId = 0;
  QString jobType;
  QDBusObjectPath jobPath;
};

struct UnitFile {
  QString path, state;
};

}  // namespace
}  // namespace td::services

Q_DECLARE_METATYPE(td::services::UnitRow)
Q_DECLARE_METATYPE(td::services::UnitFile)

namespace td::services {

namespace {

const QDBusArgument &operator>>(const QDBusArgument &a, UnitRow &u) {
  a.beginStructure();
  a >> u.name >> u.description >> u.load >> u.active >> u.sub >> u.following >> u.path >> u.jobId >> u.jobType >> u.jobPath;
  a.endStructure();
  return a;
}
QDBusArgument &operator<<(QDBusArgument &a, const UnitRow &u) {
  a.beginStructure();
  a << u.name << u.description << u.load << u.active << u.sub << u.following << u.path << u.jobId << u.jobType << u.jobPath;
  a.endStructure();
  return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, UnitFile &u) {
  a.beginStructure();
  a >> u.path >> u.state;
  a.endStructure();
  return a;
}
QDBusArgument &operator<<(QDBusArgument &a, const UnitFile &u) {
  a.beginStructure();
  a << u.path << u.state;
  a.endStructure();
  return a;
}

void Register() {
  static bool done = [] {
    qDBusRegisterMetaType<UnitRow>();
    qDBusRegisterMetaType<QList<UnitRow>>();
    qDBusRegisterMetaType<UnitFile>();
    qDBusRegisterMetaType<QList<UnitFile>>();
    return true;
  }();
  (void)done;
}

QDBusConnection Connection(Bus bus) {
  return bus == Bus::System ? QDBusConnection::systemBus() : QDBusConnection::sessionBus();
}

QDBusMessage Manager(const QString &method) {
  return QDBusMessage::createMethodCall("org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                                        "org.freedesktop.systemd1.Manager", method);
}

}  // namespace

QVector<Service> List(Bus bus, QString *error) {
  Register();
  QDBusConnection c = Connection(bus);
  QVector<Service> out;
  QHash<QString, int> index;

  QDBusMessage reply = c.call(Manager("ListUnits"), QDBus::Block, 5000);
  if (reply.type() == QDBusMessage::ErrorMessage) {
    *error = reply.errorMessage();
    return out;
  }
  QList<UnitRow> rows;
  reply.arguments().value(0).value<QDBusArgument>() >> rows;
  QHash<QString, QDBusObjectPath> paths;
  for (const UnitRow &u : rows) {
    if (!u.name.endsWith(".service")) continue;
    Service s{u.name, u.description, u.load, u.active, u.sub, QString(), 0};
    index.insert(u.name, int(out.size()));
    paths.insert(u.name, u.path);
    out << s;
  }

  QDBusMessage files = c.call(Manager("ListUnitFiles"), QDBus::Block, 5000);
  if (files.type() != QDBusMessage::ErrorMessage) {
    QList<UnitFile> list;
    files.arguments().value(0).value<QDBusArgument>() >> list;
    for (const UnitFile &f : list) {
      QString name = QFileInfo(f.path).fileName();
      if (!name.endsWith(".service") || name.contains("@.")) continue;
      auto it = index.constFind(name);
      if (it != index.constEnd()) {
        out[*it].fileState = f.state;
      } else {
        index.insert(name, int(out.size()));
        out << Service{name, QString(), "not loaded", "inactive", "dead", f.state, 0};
      }
    }
  }

  // MainPID for whatever is running.
  for (Service &s : out) {
    if (s.active != "active" && s.active != "reloading") continue;
    QDBusMessage m = QDBusMessage::createMethodCall("org.freedesktop.systemd1", paths.value(s.name).path(),
                                                    "org.freedesktop.DBus.Properties", "Get");
    m << "org.freedesktop.systemd1.Service" << "MainPID";
    QDBusMessage r = c.call(m, QDBus::Block, 1000);
    if (r.type() == QDBusMessage::ReplyMessage)
      s.mainPid = r.arguments().value(0).value<QDBusVariant>().variant().toUInt();
  }
  std::sort(out.begin(), out.end(), [](const Service &a, const Service &b) { return a.name < b.name; });
  return out;
}

QDBusPendingCall Control(Bus bus, const QString &unit, const QString &action) {
  QDBusMessage m = Manager(action);
  m << unit << QStringLiteral("replace");
  m.setInteractiveAuthorizationAllowed(true);
  return Connection(bus).asyncCall(m, 120000);
}

QDBusPendingCall Freeze(Bus bus, const QString &unit, bool freeze) {
  QDBusMessage m = Manager(freeze ? "FreezeUnit" : "ThawUnit");
  m << unit;
  m.setInteractiveAuthorizationAllowed(true);
  return Connection(bus).asyncCall(m, 30000);
}

QStringList Failed(Bus bus, bool *ok) {
  Register();
  QDBusMessage m = Manager("ListUnitsFiltered");
  m << QStringList{"failed"};
  QDBusMessage reply = Connection(bus).call(m, QDBus::Block, 2000);
  QStringList out;
  if (ok) *ok = reply.type() != QDBusMessage::ErrorMessage;
  if (reply.type() == QDBusMessage::ErrorMessage) return out;
  QList<UnitRow> rows;
  reply.arguments().value(0).value<QDBusArgument>() >> rows;
  for (const UnitRow &u : rows) out << u.name;
  out.sort();
  return out;
}

}  // namespace td::services
