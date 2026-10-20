#pragma once

// Text that comes from outside Regent (process names and command lines,
// window titles, file names, unit names) must never be read as rich text:
// Qt renders a tooltip, a QLabel or a QMessageBox as HTML as soon as the text
// merely looks like it, and HTML can load <img src="/dev/zero">.

#include <QMessageBox>
#include <QString>

namespace td::ui::safe {

// A tooltip that always shows `text` literally, line breaks kept.
inline QString Tip(const QString &text) {
  if (text.isEmpty()) return {};
  return QStringLiteral("<p style='white-space:pre-wrap'>") + text.toHtmlEscaped() + QStringLiteral("</p>");
}

// QMessageBox's static helpers with the text forced to plain.
inline QMessageBox::StandardButton Box(QMessageBox::Icon icon, QWidget *parent, const QString &title, const QString &text,
                                       QMessageBox::StandardButtons buttons, QMessageBox::StandardButton byDefault) {
  QMessageBox box(icon, title, text, buttons, parent);
  box.setTextFormat(Qt::PlainText);
  if (byDefault != QMessageBox::NoButton) box.setDefaultButton(byDefault);
  return QMessageBox::StandardButton(box.exec());
}
inline QMessageBox::StandardButton question(QWidget *parent, const QString &title, const QString &text,
                                            QMessageBox::StandardButtons buttons = QMessageBox::Yes | QMessageBox::No,
                                            QMessageBox::StandardButton byDefault = QMessageBox::NoButton) {
  return Box(QMessageBox::Question, parent, title, text, buttons, byDefault);
}
inline QMessageBox::StandardButton warning(QWidget *parent, const QString &title, const QString &text,
                                           QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                           QMessageBox::StandardButton byDefault = QMessageBox::NoButton) {
  return Box(QMessageBox::Warning, parent, title, text, buttons, byDefault);
}
inline QMessageBox::StandardButton information(QWidget *parent, const QString &title, const QString &text,
                                               QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                               QMessageBox::StandardButton byDefault = QMessageBox::NoButton) {
  return Box(QMessageBox::Information, parent, title, text, buttons, byDefault);
}
inline QMessageBox::StandardButton critical(QWidget *parent, const QString &title, const QString &text,
                                            QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                            QMessageBox::StandardButton byDefault = QMessageBox::NoButton) {
  return Box(QMessageBox::Critical, parent, title, text, buttons, byDefault);
}

}  // namespace td::ui::safe
