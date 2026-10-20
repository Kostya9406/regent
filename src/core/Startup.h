#pragma once

// What starts at login, from the two places Omarchy has:
// - Hyprland: ~/.config/hypr/autostart.lua (the user's; o.launch_on_start / hl.exec_cmd lines)
//   and Omarchy's own /usr/share/omarchy/default/hypr/autostart.lua, shown but never edited.
//   Disabling comments the user's line out behind a "-- [regent off]" marker, so it can come back.
// - XDG autostart (~/.config/autostart over /etc/xdg/autostart). Disabling follows the
//   XDG spec: a user-level copy with Hidden=true shadows the system entry.

#include <QString>
#include <QVector>

namespace td::startup {

struct Entry {
  QString id;        // file name, e.g. foo.desktop
  QString name;
  QString exec;
  QString comment;
  QString path;      // the file that is in effect
  bool user = false; // path is under ~/.config/autostart
  bool enabled = true;
  QString onlyShowIn;
  enum class Source { Xdg, Hyprland } source = Source::Xdg;
  int line = -1;     // Hyprland: 0-based line in path
  QString lineText;  // Hyprland: that line as read, to refuse editing a file changed since
};

QVector<Entry> List();  // Hyprland entries first, then XDG
// The command of one autostart.lua line, if it starts something: o.launch_on_start("…"),
// hl.exec_cmd("…") or hl.exec_cmd(o.launch("…")). *disabled: commented out by Regent.
QString HyprlandCommand(const QString &line, bool *disabled);
bool SetEnabled(const Entry &e, bool enabled, QString *error);

}  // namespace td::startup
