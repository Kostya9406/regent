#pragma once

#include "core/Alerts.h"
#include "core/Notify.h"
#include "core/SamplingEngine.h"
#include "ui/Page.h"

#include <QHash>
#include <QMainWindow>
#include <QElapsedTimer>
#include <memory>

class QLabel;
class QListWidget;
class QStackedWidget;
class QHBoxLayout;
class QAction;
class QMenu;
class QToolButton;
class QPushButton;
class QSystemTrayIcon;
class QCloseEvent;
class QTimer;
class QTimer;
class QVBoxLayout;

namespace td::ui {

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget *parent = nullptr);
  void ShowPage(const QString &id);
  void Summon();  // raise from anywhere, as a second launch does
  void SelectProcess(int pid);  // Processes, with that process selected
  QStringList PageIds() const;     // sidebar pages
  QStringList AllPageIds() const;  // every page that can be shown, parts included

protected:
  void resizeEvent(QResizeEvent *e) override;
  void changeEvent(QEvent *e) override;
  void closeEvent(QCloseEvent *e) override;
  bool eventFilter(QObject *o, QEvent *e) override;  // the sidebar's arrows around the hidden Flight Recorder row

private:
  void SyncPageCommands(Page *p);  // the page menu and the V key follow the page (or part) on screen
  QAction *rowsAction_ = nullptr;  // Rows or cards: only where the page has a list
  void BuildMenus();
  void BuildShell();
  void OnSnapshot(const SnapshotPtr &s);
  void Activate(int index);
  void Step(int delta);
  void ToggleSidebar();
  void RunNewTask();
  void ApplyAlwaysOnTop();
  void ApplyIcons();  // icons are pixmaps in the theme's colours
  void SyncMenuChecks();
  void OpenPopout();
  void InitializeTray();
  void UpdateTray(const SnapshotPtr &s);
  void ToggleRecording();
  void BuildFlightDock(QVBoxLayout *layout, QHBoxLayout *bar);
  void RefreshFlightDock();
  void UpdateStatusLine(const Snapshot &s);
  void StopFlightCapture();
  void NotifyCaptureSaved(const QString &path);
  void OpenPalette();
  void CheckAlerts(const Snapshot &s);
  QString HealthText() const;

  SamplingEngine engine_;
  QVector<Page *> pages_;
  QListWidget *nav_ = nullptr;
  QWidget *sidebar_ = nullptr;
  QStackedWidget *stack_ = nullptr;
  QLabel *title_ = nullptr;
  QWidget *headerSlot_ = nullptr;
  QHBoxLayout *headerSlotLayout_ = nullptr;
  QLabel *statusIcons_[5] = {};  // health, processes, uptime, IP, generation: values live in tooltips
  QLabel *transient_ = nullptr;
  QMenu *mainMenu_ = nullptr;        // the "⋯" commands; there is no menu bar
  QAction *pageSection_ = nullptr;   // the current page's menu is inserted before this
  QAction *pageMenuAction_ = nullptr;
  QToolButton *moreButton_ = nullptr;
  QToolButton *settingsButton_ = nullptr;
  QPushButton *refreshButton_ = nullptr;
  QToolButton *burger_ = nullptr;
  QAction *alwaysOnTop_ = nullptr;           // menu checkmarks mirror Settings
  QHash<QString, QAction *> speedActions_;  // by updateSpeed value
  bool compactSidebar_ = false;
  QVector<QWidget *> headerWidgets_;  // per page, index-aligned with pages_
  QSystemTrayIcon *tray_ = nullptr;
  class PageRecorder *recorder_ = nullptr;
  QAction *recordAction_ = nullptr;
  QLabel *recordStatus_ = nullptr;
  Page *recordedPage_ = nullptr;
  bool quitAfterRecording_ = false;
  QTimer *recordTimer_ = nullptr;
  QElapsedTimer recordElapsed_;
  Page *flightPage_ = nullptr;
  QToolButton *flightStart_ = nullptr, *flightStop_ = nullptr, *flightHistory_ = nullptr;
  QToolButton *flightSave_ = nullptr, *flightImport_ = nullptr, *flightClear_ = nullptr;
  QToolButton *flightLive_ = nullptr;
  QLabel *flightTime_ = nullptr, *flightCount_ = nullptr;
  QTimer *flightTimer_ = nullptr;
  int flightDockRunning_ = -1;  // state the dock clock is coloured for; -1 = none yet
  QWidget *saveNotice_ = nullptr;
  QLabel *savePath_ = nullptr;
  QString savedCapturePath_;
  class CommandPalette *palette_ = nullptr;
  int paletteGeneration_ = 0;  // services loaded for an older opening are dropped
  alerts::Monitor alerts_;
  std::unique_ptr<notify::Speaker> speaker_;
  QElapsedTimer unitsChecked_;
  bool unitsLoading_ = false;
};

}  // namespace td::ui
