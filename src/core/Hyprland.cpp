#include "core/Hyprland.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace td::hypr {

namespace {

QByteArray SocketPath() {
  const QByteArray sig = qgetenv("HYPRLAND_INSTANCE_SIGNATURE");
  const QByteArray rt = qgetenv("XDG_RUNTIME_DIR");
  if (sig.isEmpty() || rt.isEmpty() || sig.contains('/')) return {};
  return rt + "/hypr/" + sig + "/.socket.sock";
}

// One request, one reply: Hyprland closes the connection after answering.
QByteArray Request(const QByteArray &request) {
  const QByteArray sock = SocketPath();
  sockaddr_un addr{};
  if (sock.isEmpty() || size_t(sock.size()) >= sizeof addr.sun_path) return {};
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return {};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, sock.constData(), size_t(sock.size()));
  timeval tv{0, 300000};
  // Both directions: a wedged compositor must never stall the caller.
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  QByteArray reply;
  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0 &&
      ::write(fd, request.constData(), size_t(request.size())) == request.size()) {
    char buf[16384];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0 && reply.size() < (8 << 20)) reply.append(buf, int(n));
  }
  ::close(fd);
  return reply;
}

Window FromJson(const QJsonObject &o) {
  Window w;
  w.address = o.value("address").toString();
  w.pid = o.value("pid").toInt();
  const QJsonObject ws = o.value("workspace").toObject();
  w.workspaceId = ws.value("id").toInt();
  w.workspace = ws.value("name").toString();
  w.cls = o.value("class").toString();
  w.title = o.value("title").toString();
  w.floating = o.value("floating").toBool();
  return w;
}

// Addresses come from Hyprland itself, but they go into a Lua expression:
// accept nothing but the 0x… hex form.
bool ValidAddress(const QString &a) {
  if (!a.startsWith("0x") || a.size() < 3 || a.size() > 18) return false;
  return std::all_of(a.begin() + 2, a.end(), [](QChar c) { return c.isDigit() || (c >= 'a' && c <= 'f'); });
}

}  // namespace

bool Available() { return !SocketPath().isEmpty(); }

QVector<Window> Clients() {
  QVector<Window> out;
  for (const QJsonValue &v : QJsonDocument::fromJson(Request("j/clients")).array()) {
    Window w = FromJson(v.toObject());
    if (w.pid > 0) out << w;
  }
  return out;
}

Window ActiveWindow() { return FromJson(QJsonDocument::fromJson(Request("j/activewindow")).object()); }

QVector<int> WorkspaceIds() {
  QVector<int> ids;
  for (const QJsonValue &v : QJsonDocument::fromJson(Request("j/workspaces")).array()) {
    int id = v.toObject().value("id").toInt();
    if (id > 0) ids << id;
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

QString LuaString(const QString &value) {
  QString s;
  for (QChar c : value) {
    if (c == '\\' || c == '"') s += '\\';
    if (c == '\n' || c == '\r' || c.unicode() < 0x20) continue;
    s += c;
  }
  return '"' + s + '"';
}

bool Dispatch(const QString &expression) {
  const QByteArray reply = Request("/dispatch " + expression.toUtf8()).trimmed();
  return reply == "ok";
}

bool Focus(const QString &address) {
  if (!ValidAddress(address)) return false;
  return Dispatch(QString("hl.dsp.focus({ window = \"address:%1\" })").arg(address));
}

bool MoveToWorkspace(const QString &address, const QString &workspace, bool follow) {
  if (!ValidAddress(address) || workspace.isEmpty()) return false;
  // hl.dsp.window.move acts on the focused window (unknown keys such as
  // `window` are ignored), so focus the target and make sure it took before moving.
  const QString previous = ActiveWindow().address;
  if (!Focus(address) || ActiveWindow().address != address) return false;
  const bool ok = Dispatch(QString("hl.dsp.window.move({ workspace = %1, follow = %2 })")
                               .arg(LuaString(workspace), follow ? "true" : "false"));
  // Focusing the target switched to its workspace: go back where the user was.
  if (!follow && ValidAddress(previous) && previous != address) Focus(previous);
  return ok;
}

bool Close(const QString &address) {
  if (!ValidAddress(address)) return false;
  // Same rule as moving: never trust a `window` key, close only what is verifiably focused.
  const QString previous = ActiveWindow().address;
  if (!Focus(address) || ActiveWindow().address != address) return false;
  const bool ok = Dispatch("hl.dsp.window.close()");
  if (ValidAddress(previous) && previous != address) Focus(previous);
  return ok;
}

}  // namespace td::hypr
