#pragma once

// One look for the whole app: the current Omarchy theme's colours
// (colors.toml), its monospace font, square corners and pixel-grid graphics —
// the language of the Connections dashboard, applied everywhere.

#include <QColor>
#include <QFont>
#include <QString>

namespace td::theme {

// Surfaces
QColor Bg();        // background
QColor Base();      // darker_background: panels, graphs, cards
QColor Deep();      // darkest: behind pixel art
QColor Raised();    // lighter_background: buttons, inputs
QColor Border();    // muted
QColor Text();      // foreground
QColor Bright();    // bright_foreground
QColor Dim();       // dark_foreground
QColor Accent();
QColor Selection();

// Meanings, one colour each across every page
QColor Cpu();
QColor Kernel();
QColor Memory();
QColor Disk();
QColor Net();
QColor Gpu();
QColor Temp();
QColor Clock();
QColor Energy();
QColor Good();
QColor Warn();
QColor Bad();

QColor Mix(const QColor &a, const QColor &b, double t);

// A colour that follows the theme: a role such as theme::Cpu, looked up each
// time it is painted, or a fixed colour. Pass theme::Cpu, not theme::Cpu().
class Ink {
public:
  Ink() = default;
  Ink(QColor (*role)()) : role_(role) {}
  Ink(const QColor &fixed) : fixed_(fixed) {}
  QColor operator()() const {
    QColor c = role_ ? role_() : fixed_;
    return lighter_ == 100 ? c : c.lighter(lighter_);
  }
  Ink Lighter(int factor) const {
    Ink i = *this;
    i.lighter_ = factor;
    return i;
  }
  bool isValid() const { return role_ || fixed_.isValid(); }

private:
  QColor (*role_)() = nullptr;
  QColor fixed_;
  int lighter_ = 100;
};
QFont Mono(int pixelSize, bool bold = false);  // the Omarchy monospace font
QString StyleSheet();

}  // namespace td::theme
