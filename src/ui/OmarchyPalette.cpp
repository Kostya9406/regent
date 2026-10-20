#include "ui/OmarchyPalette.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

namespace td::ui {

namespace {
QString StateDir() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/omarchy/current";
}
}  // namespace

OmarchyPalette &OmarchyPalette::Get() {
  // Owned by the application, so its file watcher dies while the event loop still exists.
  static OmarchyPalette *p = [] {
    auto *x = new OmarchyPalette;
    x->setParent(qApp);
    return x;
  }();
  return *p;
}

OmarchyPalette::OmarchyPalette() {
  Load();
  // `omarchy theme set` swaps the theme directory, so watch the parent too.
  watcher_.addPath(StateDir());
  watcher_.addPath(StateDir() + "/theme");
  watcher_.addPath(StateDir() + "/theme/colors.toml");
  // `omarchy theme set` removes and renames directories in a burst: coalesce
  // into one reload after it settles, so no half-applied theme is ever shown.
  reload_.setSingleShot(true);
  reload_.setInterval(400);
  connect(&reload_, &QTimer::timeout, this, [this] {
    bool changed = Load();
    for (const QString &p : {StateDir(), StateDir() + "/theme", StateDir() + "/theme/colors.toml"})
      if (!watcher_.files().contains(p) && !watcher_.directories().contains(p)) watcher_.addPath(p);
    // A new wallpaper also touches current/: restyle only when the colours moved.
    if (changed) emit Changed();
  });
  auto reload = [this] { reload_.start(); };
  connect(&watcher_, &QFileSystemWatcher::fileChanged, this, reload);
  connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, reload);
}

bool OmarchyPalette::Load() {
  QHash<QString, QColor> loaded;
  QFile f(StateDir() + "/theme/colors.toml");
  if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    // key = "#rrggbb" or '#rrggbbaa'; the alpha is dropped.
    static const QRegularExpression line(R"(^\s*([a-z0-9_]+)\s*=\s*["'](#[0-9a-fA-F]{6})(?:[0-9a-fA-F]{2})?["'])");
    QTextStream in(&f);
    while (!in.atEnd()) {
      auto m = line.match(in.readLine());
      if (m.hasMatch()) loaded.insert(m.captured(1), QColor(m.captured(2)));
    }
  } else if (!QDir(StateDir() + "/theme").exists()) {
    return false;  // mid-switch: the theme directory is being replaced, keep the old colours
  }
  Complete(loaded);
  if (loaded == colors_) return false;
  colors_ = loaded;
  return true;
}

void OmarchyPalette::Complete(QHash<QString, QColor> &c) {
  // Third-party themes often give only background/foreground/accent and the
  // sixteen terminal colours: derive the named keys from those, so a theme is
  // never mixed with the built-in fallback palette.
  if (!c.contains("background") || !c.contains("foreground")) return;  // too little to build on
  auto mix = [](const QColor &a, const QColor &b, double t) {
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t));
  };
  auto alias = [&c](const char *key, const char *from) {
    if (!c.contains(key) && c.contains(from)) c.insert(key, c.value(from));
  };
  static const char *const ansi[] = {"red", "green", "yellow", "blue", "magenta", "cyan"};
  for (int i = 0; i < 6; ++i) {
    alias(ansi[i], qPrintable(QString("color%1").arg(i + 1)));
    alias(qPrintable(QString("bright_") + ansi[i]), qPrintable(QString("color%1").arg(i + 9)));
    alias(qPrintable(QString("bright_") + ansi[i]), ansi[i]);
  }
  alias("muted", "color8");
  alias("selection", "selection_background");
  alias("accent", "blue");
  const QColor bg = c.value("background"), fg = c.value("foreground"), black(Qt::black);
  auto derive = [&c](const char *key, const QColor &v) {
    if (!c.contains(key)) c.insert(key, v);
  };
  derive("dark_background", mix(bg, black, 0.06));
  derive("darker_background", mix(bg, black, 0.12));
  derive("lighter_background", mix(bg, fg, 0.08));
  derive("muted", mix(bg, fg, 0.35));
  derive("selection", mix(bg, fg, 0.15));
  derive("bright_foreground", fg);
  derive("dark_foreground", mix(fg, bg, 0.4));
  if (c.contains("red") && c.contains("yellow")) derive("orange", mix(c.value("red"), c.value("yellow"), 0.5));
}

QColor OmarchyPalette::color(const QString &key, const QColor &fallback) const {
  return colors_.value(key, fallback);
}

}  // namespace td::ui
