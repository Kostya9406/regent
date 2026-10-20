#pragma once

// The current Omarchy theme's colours from
// ~/.local/state/omarchy/current/theme/colors.toml, re-read when the theme
// changes. Falls back to Regent's own palette elsewhere.

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QTimer>

namespace td::ui {

class OmarchyPalette : public QObject {
  Q_OBJECT
public:
  static OmarchyPalette &Get();
  // Keys as in colors.toml: accent, background, foreground, muted, red, green,
  // yellow, orange, blue, cyan, magenta, bright_* …
  QColor color(const QString &key, const QColor &fallback) const;
  bool fromTheme() const { return !colors_.isEmpty(); }

signals:
  void Changed();

private:
  OmarchyPalette();
  bool Load();  // true when the colours changed
  static void Complete(QHash<QString, QColor> &colors);
  QHash<QString, QColor> colors_;
  QFileSystemWatcher watcher_;
  QTimer reload_;
};

}  // namespace td::ui
