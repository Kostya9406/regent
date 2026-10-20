#pragma once

// Vim-style keys in every list and card overview of the window: j/k down/up, h/l
// collapse/expand, gg/G first/last, Ctrl+D/Ctrl+U half a page, / the page's
// filter, and Esc in a filter returns to the list. Editors keep their letters.

#include <QElapsedTimer>
#include <QObject>
#include <QWidget>
#include <functional>

namespace td::ui {

class KeyNav : public QObject {
public:
  // Only widgets in `window` are handled; separate page windows keep plain keys.
  KeyNav(QWidget *window, std::function<void()> focusFilter, std::function<void()> focusList)
      : QObject(window), window_(window), focusFilter_(std::move(focusFilter)), focusList_(std::move(focusList)) {}

protected:
  bool eventFilter(QObject *o, QEvent *e) override;

private:
  QWidget *window_;
  std::function<void()> focusFilter_, focusList_;
  QElapsedTimer lastG_;
};

}  // namespace td::ui
