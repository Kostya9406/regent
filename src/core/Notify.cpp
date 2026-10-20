#include "core/Notify.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QVariantMap>

#include <unistd.h>

namespace td::notify {

void Send(const alerts::Alert &a) {
  // Notification daemons (mako) read the body as markup; titles hold unit names and mount points.
  const QString body = a.body.toHtmlEscaped(), title = a.title.toHtmlEscaped();
  const QString urgency = a.level == alerts::Level::Critical ? "critical" : "normal";
  const QString self = QCoreApplication::applicationFilePath();
  const QString omarchy = QStandardPaths::findExecutable("omarchy-notification-send");
  if (!omarchy.isEmpty()) {
    // Clicking the notification opens Regent on the page that explains it.
    QProcess::startDetached(omarchy, {"--app-name", "Regent", "-g", QString::fromUcs4(&a.glyph, 1), "-u", urgency,
                                      title, body, "--exec", self, "--page", a.page});
    return;
  }
  QDBusMessage m = QDBusMessage::createMethodCall("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                                  "org.freedesktop.Notifications", "Notify");
  QVariantMap hints{{"urgency", QVariant::fromValue(uchar(a.level == alerts::Level::Critical ? 2 : 1))}};
  m << QString("Regent") << uint(0) << QString("regent") << a.title << body << QStringList() << hints << -1;
  QDBusConnection::sessionBus().call(m, QDBus::NoBlock);
}

Speaker::Speaker() {
  // Only in the private runtime dir: in a shared /tmp another user could hold
  // the lock first and silence every Regent alert.
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  if (dir.isEmpty()) return;
  lock_ = std::make_unique<QLockFile>(dir + QString("/regent-alerts-%1.lock").arg(getuid()));
  lock_->setStaleLockTime(0);  // stale only when the holder is gone, never by age
}

bool Speaker::Holds() {
  if (!held_ && lock_) held_ = lock_->tryLock(0);
  return held_;
}

}  // namespace td::notify
