#pragma once

// Hyprland's request socket ($XDG_RUNTIME_DIR/hypr/$SIG/.socket.sock): the
// window list, the focused window and dispatchers (Lua syntax, Hyprland 0.55+).
// Every call has a 300 ms timeout and returns empty/false outside Hyprland.

#include <QString>
#include <QVector>

namespace td::hypr {

struct Window {
  QString address;  // 0x55cd22546730
  int pid = 0;
  int workspaceId = 0;
  QString workspace;  // name: "3", "special:scratchpad"
  QString cls;
  QString title;
  bool floating = false;
};

bool Available();
QVector<Window> Clients();
Window ActiveWindow();  // pid 0 when nothing is focused
QVector<int> WorkspaceIds();  // existing regular workspaces, ascending

// A Lua dispatcher expression, e.g. hl.dsp.focus({ workspace = "3" }).
bool Dispatch(const QString &expression);
bool Focus(const QString &address);
bool MoveToWorkspace(const QString &address, const QString &workspace, bool follow);  // focuses it first
bool Close(const QString &address);

// Quotes a value for a Lua string literal in a dispatcher.
QString LuaString(const QString &value);

}  // namespace td::hypr
