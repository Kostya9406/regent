#include "core/Startup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>

namespace td::startup {

namespace {

QString UserDir() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/autostart";
}

QStringList SystemDirs() {
  QStringList dirs;
  QString xdg = qEnvironmentVariable("XDG_CONFIG_DIRS", "/etc/xdg");
  for (const QString &d : xdg.split(':', Qt::SkipEmptyParts)) dirs << d + "/autostart";
  return dirs;
}

// Reads the [Desktop Entry] group only; localized keys are ignored.
QMap<QString, QString> ReadEntry(const QString &path) {
  QMap<QString, QString> m;
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return m;
  bool inGroup = false;
  QTextStream in(&f);
  while (!in.atEnd()) {
    QString l = in.readLine().trimmed();
    if (l.startsWith('[')) {
      inGroup = l == "[Desktop Entry]";
      continue;
    }
    if (!inGroup || l.startsWith('#')) continue;
    int eq = l.indexOf('=');
    if (eq > 0) m.insert(l.left(eq).trimmed(), l.mid(eq + 1).trimmed());
  }
  return m;
}

bool IsTrue(const QString &v) { return v.compare("true", Qt::CaseInsensitive) == 0; }

// Rewrites `keys` inside [Desktop Entry] of `text`, appending missing ones.
QString WithKeys(const QString &text, const QMap<QString, QString> &keys) {
  QStringList lines = text.split('\n');
  QMap<QString, QString> pending = keys;
  int groupEnd = -1;
  bool inGroup = false;
  for (int i = 0; i < lines.size(); ++i) {
    QString t = lines[i].trimmed();
    if (t.startsWith('[')) {
      if (inGroup && groupEnd < 0) groupEnd = i;
      inGroup = t == "[Desktop Entry]";
      continue;
    }
    if (!inGroup) continue;
    int eq = t.indexOf('=');
    if (eq <= 0) continue;
    QString k = t.left(eq).trimmed();
    if (pending.contains(k)) lines[i] = k + '=' + pending.take(k);
  }
  if (groupEnd < 0) {
    groupEnd = int(lines.size());
    while (groupEnd > 0 && lines[groupEnd - 1].trimmed().isEmpty()) --groupEnd;
  }
  for (auto it = pending.cbegin(); it != pending.cend(); ++it) lines.insert(groupEnd++, it.key() + '=' + it.value());
  return lines.join('\n');
}

}  // namespace

namespace {
const QString kOff = QStringLiteral("-- [regent off] ");

QString UserLua() { return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/hypr/autostart.lua"; }
QString OmarchyLua() { return QStringLiteral("/usr/share/omarchy/default/hypr/autostart.lua"); }

// The first Lua string literal from `from`: "…" or '…', with backslash escapes.
QString StringLiteral(const QString &s, int from, int *end) {
  int q = from;
  while (q < s.size() && s[q] != '"' && s[q] != '\'') ++q;
  if (q >= s.size()) return {};
  const QChar quote = s[q];
  QString out;
  for (int i = q + 1; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      out += s[++i];
    } else if (s[i] == quote) {
      *end = i + 1;
      return out;
    } else {
      out += s[i];
    }
  }
  return {};
}

void AddHyprland(const QString &path, bool user, QVector<Entry> *out) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
  const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
  for (int i = 0; i < lines.size(); ++i) {
    bool disabled = false;
    const QString command = HyprlandCommand(lines[i], &disabled);
    if (command.isEmpty()) continue;
    Entry e;
    e.source = Entry::Source::Hyprland;
    e.id = QString("%1:%2").arg(path).arg(i + 1);
    e.exec = command;
    // A readable name: without a leading "sleep N &&" or shell substitutions.
    QString name = command;
    static const QRegularExpression sleepFirst("^sleep\\s+[0-9.]+\\s*&&\\s*");
    name.remove(sleepFirst);
    for (const QString &cut : {QStringLiteral("$("), QStringLiteral("|"), QStringLiteral("&&"), QStringLiteral(";")})
      if (int at = name.indexOf(cut); at > 0) name = name.left(at);
    name = name.trimmed();
    e.name = name.size() > 48 ? name.left(47) + "…" : name;
    e.path = path;
    e.user = user;
    e.enabled = !disabled;
    e.line = i;
    e.lineText = lines[i];
    e.comment = user ? QObject::tr("Your Hyprland autostart, line %1").arg(i + 1)
                     : QObject::tr("Omarchy's own startup, line %1: change it with an override in your autostart.lua").arg(i + 1);
    *out << e;
  }
}
}  // namespace

QString HyprlandCommand(const QString &line, bool *disabled) {
  QString t = line.trimmed();
  *disabled = t.startsWith(kOff.trimmed());
  if (*disabled) t = t.mid(kOff.trimmed().size()).trimmed();
  if (t.startsWith("--")) return {};  // a comment: Omarchy's own examples among them
  for (const QString &call : {QStringLiteral("o.launch_on_start("), QStringLiteral("hl.exec_cmd(")}) {
    if (!t.startsWith(call)) continue;
    int end = 0;
    const QString command = StringLiteral(t, int(call.size()), &end);
    return command.trimmed();
  }
  return {};
}

QVector<Entry> List() {
  QVector<Entry> hyprland;
  AddHyprland(UserLua(), true, &hyprland);
  AddHyprland(OmarchyLua(), false, &hyprland);
  QMap<QString, QString> effective;  // id -> path; user dir wins
  for (const QString &dir : SystemDirs())
    for (const QString &f : QDir(dir).entryList({"*.desktop"}, QDir::Files))
      if (!effective.contains(f)) effective.insert(f, dir + '/' + f);
  for (const QString &f : QDir(UserDir()).entryList({"*.desktop"}, QDir::Files)) effective.insert(f, UserDir() + '/' + f);

  QVector<Entry> out = hyprland;
  QMap<QString, QString> systemPaths;
  for (const QString &dir : SystemDirs())
    for (const QString &f : QDir(dir).entryList({"*.desktop"}, QDir::Files))
      if (!systemPaths.contains(f)) systemPaths.insert(f, dir + '/' + f);
  for (auto it = effective.cbegin(); it != effective.cend(); ++it) {
    QMap<QString, QString> m = ReadEntry(it.value());
    // A mask (user file without Exec) keeps the system entry's name and command.
    if (!m.contains("Exec") && systemPaths.contains(it.key())) {
      QMap<QString, QString> sys = ReadEntry(systemPaths.value(it.key()));
      for (auto k = sys.cbegin(); k != sys.cend(); ++k)
        if (!m.contains(k.key())) m.insert(k.key(), k.value());
    }
    Entry e;
    e.id = it.key();
    e.path = it.value();
    e.user = e.path.startsWith(UserDir());
    e.name = m.value("Name", e.id);
    e.exec = m.value("Exec");
    e.comment = m.value("Comment");
    e.onlyShowIn = m.value("OnlyShowIn");
    e.enabled = !IsTrue(m.value("Hidden")) && m.value("X-GNOME-Autostart-enabled", "true") != "false";
    out << e;
  }
  return out;
}

bool SetEnabled(const Entry &e, bool enabled, QString *error) {
  if (e.source == Entry::Source::Hyprland) {
    if (!e.user) {
      *error = QObject::tr("This is Omarchy's own startup (%1). Regent changes only your ~/.config/hypr/autostart.lua.").arg(e.path);
      return false;
    }
    QFile in(e.path);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
      *error = QObject::tr("%1 could not be read").arg(e.path);
      return false;
    }
    QStringList lines = QString::fromUtf8(in.readAll()).split('\n');
    in.close();
    if (e.line < 0 || e.line >= lines.size() || lines[e.line] != e.lineText) {
      *error = QObject::tr("%1 changed since it was read: reload the page and try again.").arg(e.path);
      return false;
    }
    QString &l = lines[e.line];
    const int indent = int(l.size() - QString(l).remove(QRegularExpression("^\\s+")).size());
    const QString body = l.mid(indent);
    if (enabled && body.startsWith(kOff)) l = l.left(indent) + body.mid(kOff.size());
    else if (!enabled && !body.startsWith(kOff)) l = l.left(indent) + kOff + body;
    QSaveFile out(e.path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text) || out.write(lines.join('\n').toUtf8()) < 0 || !out.commit()) {
      *error = QObject::tr("%1 could not be written").arg(e.path);
      return false;
    }
    return true;
  }
  QDir().mkpath(UserDir());
  QString target = UserDir() + '/' + e.id;
  QString system;
  for (const QString &dir : SystemDirs())
    if (system.isEmpty() && QFileInfo::exists(dir + '/' + e.id)) system = dir + '/' + e.id;

  // A user file without Exec is only a mask over the system entry (GNOME Tweaks
  // writes those). Enabling then means removing the mask, not editing it.
  if (enabled && e.user && !system.isEmpty() && !ReadEntry(e.path).contains("Exec")) {
    if (!QFile::remove(e.path)) {
      *error = QObject::tr("%1 could not be removed").arg(e.path);
      return false;
    }
    QMap<QString, QString> sys = ReadEntry(system);
    bool systemEnabled = !IsTrue(sys.value("Hidden")) && sys.value("X-GNOME-Autostart-enabled", "true") != "false";
    if (systemEnabled) return true;
    // The system entry is off itself: fall through and write an enabled user copy of it.
  }

  // Edit the user copy when there is one, otherwise start from the system file.
  QString source = e.user && QFileInfo::exists(e.path) ? e.path : (system.isEmpty() ? e.path : system);
  QFile src(source);
  if (!src.open(QIODevice::ReadOnly | QIODevice::Text)) {
    *error = QObject::tr("%1 could not be read").arg(source);
    return false;
  }
  QString text = QString::fromUtf8(src.readAll());
  src.close();
  QMap<QString, QString> keys{{"Hidden", enabled ? "false" : "true"}};
  if (ReadEntry(source).contains("X-GNOME-Autostart-enabled")) keys.insert("X-GNOME-Autostart-enabled", enabled ? "true" : "false");
  QSaveFile out(target);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
    *error = QObject::tr("%1 could not be written").arg(target);
    return false;
  }
  out.write(WithKeys(text, keys).toUtf8());
  if (!out.commit()) {
    *error = QObject::tr("%1 could not be written").arg(target);
    return false;
  }
  return true;
}

}  // namespace td::startup
