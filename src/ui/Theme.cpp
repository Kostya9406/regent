#include "ui/Theme.h"

#include "ui/OmarchyPalette.h"

#include <QFontDatabase>
#include <QFontInfo>

namespace td::theme {

namespace {
QColor P(const char *key, const char *fallback) { return ui::OmarchyPalette::Get().color(key, QColor(fallback)); }
}  // namespace

// Fallbacks are Omarchy's default dark theme, so Regent looks the same off Omarchy.
QColor Bg() { return P("background", "#1a1b26"); }
QColor Base() { return P("dark_background", "#13141c"); }
QColor Deep() { return P("darker_background", "#0e0e14"); }
QColor Raised() { return P("lighter_background", "#24283b"); }
QColor Border() { return P("muted", "#414868"); }
QColor Text() { return P("foreground", "#a9b1d6"); }
QColor Bright() { return P("bright_foreground", "#c0caf5"); }
QColor Dim() { return P("dark_foreground", "#565f89"); }
QColor Accent() { return P("accent", "#7aa2f7"); }
QColor Selection() { return P("selection", "#292e42"); }

QColor Cpu() { return P("cyan", "#449dab"); }
QColor Kernel() { return P("red", "#f7768e"); }
QColor Memory() { return P("magenta", "#ad8ee6"); }
QColor Disk() { return P("green", "#9ece6a"); }
QColor Net() { return P("blue", "#7aa2f7"); }
QColor Gpu() { return P("bright_cyan", "#0db9d7"); }
QColor Temp() { return P("yellow", "#e0af68"); }
QColor Clock() { return P("orange", "#eb927b"); }
QColor Energy() { return P("bright_yellow", "#ff9e64"); }
QColor Good() { return P("green", "#9ece6a"); }
QColor Warn() { return P("yellow", "#e0af68"); }
QColor Bad() { return P("red", "#f7768e"); }

QColor Mix(const QColor &a, const QColor &b, double t) {
  t = std::clamp(t, 0.0, 1.0);
  return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                          float(a.blueF() + (b.blueF() - a.blueF()) * t));
}

QFont Mono(int px, bool bold) {
  // fontconfig "monospace" is the font `omarchy font set` chose. Name it
  // explicitly: a family left unset is replaced by the default UI font.
  static const QString family = QFontInfo(QFontDatabase::systemFont(QFontDatabase::FixedFont)).family();
  QFont f;
  f.setFamilies({family, QStringLiteral("monospace")});
  f.setStyleHint(QFont::Monospace);
  f.setPixelSize(px);
  f.setBold(bold);
  return f;
}

QString StyleSheet() {
  QString bg = Bg().name(), base = Base().name(), deep = Deep().name(), raised = Raised().name(),
          border = Border().name(), text = Text().name(), bright = Bright().name(), dim = Dim().name(),
          accent = Accent().name(), sel = Selection().name();
  QString alt = Mix(Base(), Bg(), 0.5).name();
  QString s = R"(
    QMainWindow, QDialog { background: @bg; color: @text; }
    QWidget { color: @text; }
    QToolTip { background: @deep; color: @bright; border: 1px solid @border; padding: 4px; }
    QMenu { background: @deep; border: 1px solid @border; padding: 4px 0; }
    QMenu::item { padding: 5px 28px 5px 18px; }
    QMenu::item:selected { background: @sel; color: @bright; }
    QMenu::separator { height: 1px; background: @border; margin: 4px 10px; }
    QMenu::indicator { width: 12px; height: 12px; left: 4px; }
    #Sidebar { background: @deep; border-right: 1px solid @border; }
    #Sidebar QListView { background: transparent; border: none; outline: none; }
    #Sidebar QListView::item { padding: 9px 12px; border-left: 3px solid transparent; }
    #Sidebar QListView::item:selected { background: @sel; color: @bright; border-left: 3px solid @accent; }
    #Sidebar QListView::item:hover:!selected { background: @base; }
    #Sidebar QToolButton { border: none; background: transparent; padding: 8px 14px; color: @dim; }
    #Sidebar QToolButton:hover { color: @bright; }
    #PageTitle { color: @bright; font-weight: 700; letter-spacing: 1px; }
    #Header { background: @bg; border-bottom: 1px solid @border; }
    #StatusBar { background: @deep; border-top: 1px solid @border; color: @dim; }
    QLineEdit { background: @base; border: 1px solid @border; padding: 4px 8px; selection-background-color: @sel; }
    QLineEdit:focus { border-color: @accent; }
    QPushButton, QToolButton { background: @raised; border: 1px solid @border; padding: 4px 12px; }
    QPushButton:hover, QToolButton:hover { border-color: @accent; color: @bright; }
    QPushButton:pressed, QToolButton:pressed { background: @sel; }
    QPushButton:disabled { color: @dim; border-color: @raised; }
    QToolButton::menu-indicator { image: none; width: 0; }
    QTreeView, QTableView, QListWidget, QTreeWidget, QPlainTextEdit {
      background: @base; alternate-background-color: @alt; border: 1px solid @border;
      selection-background-color: @sel; selection-color: @bright; outline: none; }
    QTreeView::item:selected, QListWidget::item:selected { background: @sel; color: @bright; }
    QHeaderView::section { background: @deep; color: @dim; border: none;
      border-right: 1px solid @border; border-bottom: 1px solid @border; padding: 4px 6px; }
    QScrollBar:vertical { background: @base; width: 9px; border: none; }
    QScrollBar::handle:vertical { background: @border; min-height: 30px; }
    QScrollBar:horizontal { background: @base; height: 9px; border: none; }
    QScrollBar::handle:horizontal { background: @border; min-width: 30px; }
    QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
    QScrollBar::add-page, QScrollBar::sub-page { background: none; }
    QComboBox { background: @raised; border: 1px solid @border; padding: 3px 8px; }
    QComboBox:hover { border-color: @accent; }
    QComboBox::drop-down { border: none; width: 16px; }
    QComboBox QAbstractItemView { background: @deep; border: 1px solid @border; selection-background-color: @sel; }
    QCheckBox::indicator { width: 12px; height: 12px; border: 1px solid @border; background: @base; }
    QCheckBox::indicator:checked { background: @accent; border-color: @accent; }
    QSplitter::handle { background: @border; }
    QSplitter::handle:horizontal { width: 1px; }
    QSplitter::handle:vertical { height: 1px; }
    QTabWidget::pane { border: 1px solid @border; }
    QTabBar::tab { background: transparent; padding: 5px 14px; color: @dim; border-bottom: 2px solid transparent; }
    QTabBar::tab:selected { color: @bright; border-bottom: 2px solid @accent; }
    QProgressBar { background: @base; border: none; height: 6px; }
    QProgressBar::chunk { background: @accent; }
    QScrollArea, QScrollArea > #qt_scrollarea_viewport, QScrollArea > #qt_scrollarea_viewport > QWidget {
      background: @bg; border: none; }
    #Card { background: @base; border: 1px solid @border; }
    #FlightDock { background: transparent; }
    #FlightDock QToolButton { padding: 2px; background: @base; }
    #FlightDock QToolButton:checked { background: @sel; border-color: @accent; }
    #PartStrip { background: @deep; border-bottom: 1px solid @border; }
    QToolButton#PartSwitch { background: transparent; border: 1px solid transparent; padding: 3px 12px; color: @dim; }
    QToolButton#PartSwitch:hover { color: @bright; border-color: @border; }
    QToolButton#PartSwitch:checked { background: @sel; color: @bright; border-color: @accent; }
    #FlightDock QToolButton:disabled { color: @dim; border-color: @base; }
    #SaveNotice { background: @base; border-left: 3px solid @accent; }
    #TableFold { background: @deep; border: 1px solid @border; text-align: left; padding: 5px 10px; }
    #Dim { color: @dim; }
    #Big { color: @bright; font-weight: 700; }
  )";
  for (auto [k, v] : {std::pair{"@bg", bg}, {"@base", base}, {"@deep", deep}, {"@raised", raised}, {"@border", border},
                      {"@text", text}, {"@bright", bright}, {"@dim", dim}, {"@accent", accent}, {"@sel", sel}, {"@alt", alt}})
    s.replace(k, v);
  return s;
}

}  // namespace td::theme
