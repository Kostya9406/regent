#include "ui/KeyNav.h"

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QLineEdit>

namespace td::ui {

namespace {
void Send(QWidget *to, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
  QKeyEvent press(QEvent::KeyPress, key, mods);
  QCoreApplication::sendEvent(to, &press);
}
}  // namespace

bool KeyNav::eventFilter(QObject *o, QEvent *e) {
  if (e->type() != QEvent::KeyPress || !o->isWidgetType()) return false;
  auto *w = static_cast<QWidget *>(o);
  if (w->window() != window_) return false;
  auto *k = static_cast<QKeyEvent *>(e);
  const Qt::KeyboardModifiers mods = k->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier);

  if (auto *edit = qobject_cast<QLineEdit *>(w)) {
    // Esc leaves a page's filter for its list, keeping what was typed.
    if (k->key() == Qt::Key_Escape && mods == Qt::NoModifier && !edit->window()->isModal() && focusList_) {
      focusList_();
      return true;
    }
    return false;
  }
  // Lists, and the card overviews that stand in for tables.
  auto *view = qobject_cast<QAbstractItemView *>(w);
  if (!view && w->objectName() != QLatin1String("TableAtlas")) return false;
  if (mods == Qt::ControlModifier && (k->key() == Qt::Key_D || k->key() == Qt::Key_U)) {
    // Half a page: send the arrows a page's worth of rows halves to.
    if (!view) {
      Send(w, k->key() == Qt::Key_D ? Qt::Key_PageDown : Qt::Key_PageUp);
      return true;
    }
    const int rows = qMax(1, view->viewport()->height() / qMax(1, view->sizeHintForRow(0)) / 2);
    for (int i = 0; i < rows; ++i) Send(view, k->key() == Qt::Key_D ? Qt::Key_Down : Qt::Key_Up);
    return true;
  }
  if (mods != Qt::NoModifier) return false;
  const QString t = k->text();
  if (t == "j") Send(w, Qt::Key_Down);
  else if (t == "k") Send(w, Qt::Key_Up);
  else if (t == "h") Send(w, Qt::Key_Left);
  else if (t == "l") Send(w, Qt::Key_Right);
  else if (t == "G") Send(w, Qt::Key_End);
  else if (t == "g") {
    if (lastG_.isValid() && lastG_.elapsed() < 600) {
      Send(w, Qt::Key_Home);
      lastG_.invalidate();
    } else {
      lastG_.start();
    }
  } else if (t == "/" && focusFilter_) focusFilter_();
  else return false;
  return true;
}

}  // namespace td::ui
