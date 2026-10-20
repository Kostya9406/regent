#pragma once

#include "core/Snapshot.h"

#include <QWidget>

class QMenu;
class QHBoxLayout;

namespace td::ui {

// One entry of the sidebar. The shell owns the header (title, Refresh) and
// calls Update() with every published snapshot while the page is visible,
// and once on show so a hidden page never displays stale numbers.
class Page : public QWidget {
  Q_OBJECT
public:
  using QWidget::QWidget;
  virtual QString Id() const = 0;
  virtual QString Title() const = 0;
  virtual void Update(const SnapshotPtr &s) = 0;
  // Optional controls placed in the header between the title and Refresh.
  virtual void BuildHeader(QHBoxLayout *) {}
  // Optional page menu shown in the menu bar while the page is current.
  virtual QMenu *PageMenu() { return nullptr; }
  // Pages that keep history (graphs) want every snapshot, even when hidden.
  virtual bool WantsBackgroundUpdates() const { return false; }
  // A page may offer to save unsaved work before the application exits.
  virtual bool CanClose() { return true; }
  virtual void FocusFilter() {}
  // Select a process by PID (Processes); returns false when the page has no processes.
  virtual bool SelectProcess(int) { return false; }
  // Pages made of parts (Performance: live graphs and sensors) show one at a time.
  virtual QStringList PartIds() const { return {}; }
  virtual bool ShowPart(const QString &) { return false; }
  virtual Page *CurrentPart() { return this; }

signals:
  void Status(const QString &message);
  void CaptureStateChanged(bool recording);
  void CaptureSaved(const QString &path);
  void AutoCaptureFinished();  // Flight Recorder: an automatic recording ended and wants saving
  void PartChanged();          // another part is shown: its menu and keys apply now
};

}  // namespace td::ui
