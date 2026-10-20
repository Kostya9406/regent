#pragma once

// Runs blocking work on the global thread pool and hands the result back on
// the GUI thread, if `owner` still exists. main() drains the pool before the
// application object goes away, so no worker outlives QApplication.

#include <QCoreApplication>
#include <QPointer>
#include <QThreadPool>
#include <functional>

namespace td::ui {

template <typename T, typename Owner>
void RunAsync(Owner *owner, std::function<T()> work, std::function<void(Owner *, const T &)> done) {
  QPointer<Owner> guard(owner);
  QThreadPool::globalInstance()->start([guard, work = std::move(work), done = std::move(done)] {
    T result = work();
    QMetaObject::invokeMethod(qApp, [guard, result = std::move(result), done] {
      if (guard) done(guard.data(), result);
    }, Qt::QueuedConnection);
  });
}

}  // namespace td::ui
