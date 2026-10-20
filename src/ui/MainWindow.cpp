#include "ui/Safe.h"
#include "ui/MainWindow.h"

#include "core/Actions.h"
#include "core/Hyprland.h"
#include "core/Services.h"
#include "ui/Async.h"
#include "ui/CommandPalette.h"
#include "ui/KeyNav.h"
#include "core/Fs.h"
#include "core/Settings.h"
#include "ui/OmarchyPalette.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "ui/PageRecorder.h"
#include "ui/ResponsiveTables.h"
#include "ui/TableAtlas.h"
#include "ui/pages/FlightRecorderControls.h"
#include <QKeyEvent>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include "ui/pages/Pages.h"

#include <QAbstractButton>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDBusPendingCallWatcher>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QFontMetrics>
#include <QFileDialog>
#include <QCloseEvent>
#include <QPainter>
#include <QPointer>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <csignal>

namespace td::ui {

namespace {

// Status-line icons: the Material Design glyphs of the Nerd Font that the Omarchy
// bar itself draws with, so the line reads like the rest of the desktop.
QFont IconFont(int px) {
  QFont f;
  f.setFamilies({QStringLiteral("JetBrainsMono Nerd Font"), QStringLiteral("Symbols Nerd Font"),
                 QStringLiteral("monospace")});
  f.setPixelSize(px);
  return f;
}

QPixmap StatusGlyph(char32_t glyph, QColor color, qreal dpr) {
  const QFont font = IconFont(16);
  const QFontMetrics fm(font);
  const QString text = QString::fromUcs4(&glyph, 1);
  const QSize size(18, 18);
  QPixmap pm(size * dpr);
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::TextAntialiasing);
  p.setFont(font);
  p.setPen(color);
  // Without a Nerd Font the glyph would be a missing-character box: draw a plain dot.
  if (fm.inFontUcs4(glyph)) p.drawText(QRect(QPoint(), size), Qt::AlignCenter, text);
  else p.fillRect(6, 6, 6, 6, color);
  return pm;
}

// Header buttons tagged with a "compactText" property switch to it when the page's
// controls no longer fit beside the title, so the filter keeps its room; the full
// label stays as the tooltip.
class HeaderFit : public QObject {
public:
  explicit HeaderFit(QWidget *header) : QObject(header), header_(header) { header->installEventFilter(this); }

protected:
  bool eventFilter(QObject *, QEvent *e) override {
    if (e->type() != QEvent::Resize && e->type() != QEvent::Show) return false;
    const auto buttons = header_->findChildren<QAbstractButton *>();
    if (!compact_) need_ = header_->layout()->sizeHint().width();  // measured with the full labels
    const bool compact = header_->width() < need_;
    if (compact == compact_) return false;
    compact_ = compact;
    for (auto *b : buttons) {
      const QString brief = b->property("compactText").toString();
      if (brief.isEmpty()) continue;
      if (!b->property("fullText").isValid()) b->setProperty("fullText", b->text());
      const QString full = b->property("fullText").toString();
      b->setText(compact ? brief : full);
      if (b->toolTip().isEmpty() || b->toolTip() == QString(full).remove('&'))
        b->setToolTip(compact ? QString(full).remove('&') : QString());
    }
    return false;
  }

private:
  QWidget *header_;
  int need_ = 0;
  bool compact_ = false;
};

void ShowSettingsDialog(QWidget *parent, const QStringList &pageIds, const QStringList &pageTitles) {
  Settings &st = Settings::Get();
  QDialog d(parent);
  d.setWindowTitle(QObject::tr("Settings"));
  auto *form = new QFormLayout;

  auto *speed = new QComboBox;
  speed->addItem(QObject::tr("High (0.5 s)"), "high");
  speed->addItem(QObject::tr("Normal (1 s)"), "normal");
  speed->addItem(QObject::tr("Low (2 s)"), "low");
  speed->addItem(QObject::tr("Paused"), "paused");
  speed->setCurrentIndex(speed->findData(st.updateSpeed()));
  form->addRow(QObject::tr("Update speed"), speed);

  auto *unit = new QComboBox;
  unit->addItem(QObject::tr("Celsius (°C)"), "celsius");
  unit->addItem(QObject::tr("Fahrenheit (°F)"), "fahrenheit");
  unit->setCurrentIndex(st.fahrenheit() ? 1 : 0);
  form->addRow(QObject::tr("Temperature"), unit);

  auto *history = new QComboBox;
  for (int s : {30, 60, 120, 300}) history->addItem(QObject::tr("%1 seconds").arg(s), s);
  history->setCurrentIndex(std::max(0, history->findData(st.graphSeconds())));
  form->addRow(QObject::tr("Graph history"), history);

  auto *page = new QComboBox;
  for (int i = 0; i < pageIds.size(); ++i) page->addItem(pageTitles[i], pageIds[i]);
  page->setCurrentIndex(std::max(0, page->findData(st.defaultPage())));
  form->addRow(QObject::tr("Default page"), page);

  auto *smooth = new QCheckBox(QObject::tr("Smooth graph curves"));
  smooth->setChecked(st.smoothGraphCurves());
  form->addRow(QString(), smooth);
  auto *tree = new QCheckBox(QObject::tr("Show processes as a tree"));
  tree->setChecked(st.processTreeEnabled());
  form->addRow(QString(), tree);
  auto *top = new QCheckBox(QObject::tr("Always on top"));
  top->setChecked(st.alwaysOnTop());
  form->addRow(QString(), top);
  auto *tray = new QCheckBox(QObject::tr("Show CPU and memory in the system tray"));
  tray->setChecked(st.value("showTrayIcon", true).toBool());
  form->addRow(QString(), tray);
  auto *hide = new QCheckBox(QObject::tr("Hide to tray when minimized"));
  hide->setChecked(st.value("hideWhenMinimized", false).toBool());
  form->addRow(QString(), hide);

  // Alerts: the window and the bar's --stream read the same thresholds.
  auto *alertBox = new QGroupBox(QObject::tr("Alerts"));
  auto *af = new QFormLayout(alertBox);
  auto *alertsOn = new QCheckBox(QObject::tr("Watch for trouble (status line and bar turn red)"));
  alertsOn->setChecked(st.value("alertsEnabled", true).toBool());
  af->addRow(alertsOn);
  auto *notifyOn = new QCheckBox(QObject::tr("Desktop notifications"));
  notifyOn->setChecked(st.value("alertNotifications", true).toBool());
  af->addRow(notifyOn);
  auto *temp = new QSpinBox;
  temp->setRange(50, 110);
  temp->setSuffix(QObject::tr(" °C"));
  temp->setValue(st.value("alertCpuTempC", 90).toInt());
  af->addRow(QObject::tr("CPU temperature at or above"), temp);
  auto *pressure = new QSpinBox;
  pressure->setRange(1, 100);
  pressure->setSuffix(QObject::tr(" % (PSI)"));
  pressure->setValue(st.value("alertMemPressure", 20).toInt());
  af->addRow(QObject::tr("Memory pressure at or above"), pressure);
  auto *disk = new QSpinBox;
  disk->setRange(1, 50);
  disk->setSuffix(QObject::tr(" % free"));
  disk->setValue(st.value("alertDiskFreePct", 5).toInt());
  af->addRow(QObject::tr("A filesystem below"), disk);
  auto *cpuLoad = new QSpinBox;
  cpuLoad->setRange(0, 100);
  cpuLoad->setSpecialValueText(QObject::tr("Off"));
  cpuLoad->setSuffix(QObject::tr(" % for a minute"));
  cpuLoad->setValue(st.value("alertCpuPercent", 0).toInt());
  af->addRow(QObject::tr("CPU load at or above"), cpuLoad);
  auto *throttle = new QCheckBox(QObject::tr("Thermal throttling"));
  throttle->setChecked(st.value("alertThrottling", true).toBool());
  af->addRow(throttle);
  auto *failed = new QCheckBox(QObject::tr("Failed systemd units"));
  failed->setChecked(st.value("alertFailedUnits", true).toBool());
  af->addRow(failed);
  form->addRow(alertBox);
  auto *autoRecord = new QCheckBox(QObject::tr("Flight Recorder: record by itself when something goes wrong"));
  autoRecord->setChecked(st.value("flightAutoRecord", false).toBool());
  autoRecord->setToolTip(QObject::tr("Saved to the recordings folder with every process's command line"));
  form->addRow(QString(), autoRecord);

  auto *where = new QLabel(QObject::tr("Stored in %1").arg(st.path()));
  where->setObjectName("Dim");
  where->setTextInteractionFlags(Qt::TextSelectableByMouse);
  form->addRow(where);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  auto *v = new QVBoxLayout(&d);
  v->addLayout(form);
  v->addWidget(buttons);
  if (d.exec() != QDialog::Accepted) return;
  st.set("updateSpeed", speed->currentData());
  st.set("temperatureUnit", unit->currentData());
  st.set("graphSeconds", history->currentData());
  st.set("defaultPage", page->currentData());
  st.set("smoothGraphCurves", smooth->isChecked());
  if (tree->isChecked()) st.set("processGrouping", "tree");
  else if (st.processTreeEnabled()) st.set("processGrouping", "apps");
  st.set("alwaysOnTop", top->isChecked());
  st.set("showTrayIcon", tray->isChecked());
  st.set("hideWhenMinimized", hide->isChecked());
  st.set("alertsEnabled", alertsOn->isChecked());
  st.set("alertNotifications", notifyOn->isChecked());
  st.set("alertCpuTempC", temp->value());
  st.set("alertMemPressure", pressure->value());
  st.set("alertDiskFreePct", disk->value());
  st.set("alertCpuPercent", cpuLoad->value());
  st.set("alertThrottling", throttle->isChecked());
  st.set("alertFailedUnits", failed->isChecked());
  st.set("flightAutoRecord", autoRecord->isChecked());
}

QString NavIconId(const QString &pageId) {
  static const QHash<QString, QString> m = {
      {"summary", "summary"},   {"performance", "performance"}, {"processes", "processes"},
      {"system-info", "systeminfo"}, {"system", "systeminfo"}, {"startup-apps", "startup"}, {"users", "users"},
      {"services", "services"}, {"units", "units"}, {"power-and-freq", "power"},   {"connections", "connections"},   {"installed-apps", "apps"},
      {"drivers", "drivers"},   {"disk-space", "diskspace"},
      {"flight-recorder", "flight"}, {"benchmarks", "benchmarks"}};
  return m.value(pageId);
}

}  // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(tr("Regent"));
  resize(1280, 820);

  // Hardware lives in two places (live graphs and sensors; the machine and its
  // devices); who runs what is one page, grouped by app, user or parent.
  pages_ = {CreateSummaryPage(this),
            CreateCompositePage("performance", tr("Performance"),
                                {{tr("&Live"), CreatePerformancePage(this)},
                                 {tr("&Sensors && power"), CreatePowerFreqPage(this)}}, this),
            CreateProcessesPage(this),
            CreateCompositePage("system", tr("System"),
                                {{tr("&Overview"), CreateSystemInfoPage(this)},
                                 {tr("&Devices"), CreateDriversPage(this)}}, this),
            CreateStartupPage(this),      CreateServicesPage(this),    CreateUnitsPage(this),
            CreateConnectionsPage(this),  CreateInstalledAppsPage(this), CreateDiskSpacePage(this),
            CreateFlightRecorderPage(this), CreateBenchmarksPage(this)};
  for (Page *page : pages_) {
    page->setProperty("pageId", page->Id());
    if (page->PartIds().isEmpty()) InstallResponsiveTables(page);  // parts install their own
    if (page->Id() == "flight-recorder") flightPage_ = page;
    connect(page, &Page::PartChanged, this, [this, page] {
      if (stack_->currentWidget() == page) SyncPageCommands(page);
    });
  }
  BuildShell();
  BuildMenus();
  recorder_ = new PageRecorder(this);
  recordTimer_ = new QTimer(this);
  recordTimer_->setInterval(250);
  connect(recordTimer_, &QTimer::timeout, this, [this] {
    qint64 seconds = recordElapsed_.elapsed() / 1000;
    QString time = QString("%1:%2:%3").arg(seconds / 3600, 2, 10, QChar('0'))
        .arg(seconds / 60 % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0'));
    recordStatus_->setText(tr("<a href='stop'>● Stop recording · %1</a>").arg(time));
  });
  connect(recorder_, &PageRecorder::Started, this, [this](const QString &) {
    recordElapsed_.start();
    recordTimer_->start();
    recordAction_->setText(tr("Stop recording"));
    recordStatus_->setText(tr("● Recording page"));
  });
  connect(recorder_, &PageRecorder::Finished, this, [this](const QString &path) {
    recordTimer_->stop();
    recordedPage_ = nullptr;
    recordAction_->setText(tr("Record page video…"));
    recordStatus_->clear();
    transient_->setText(tr("Video saved: %1").arg(path));
    const bool quit = quitAfterRecording_;
    quitAfterRecording_ = false;
    if (quit) close();
  });
  connect(recorder_, &PageRecorder::Error, this, [this](const QString &error) {
    recordTimer_->stop();
    recordedPage_ = nullptr;
    quitAfterRecording_ = false;
    recordAction_->setText(tr("Record page video…"));
    recordStatus_->clear();
    safe::warning(this, tr("Video recording"), error);
  });

  connect(&engine_, &SamplingEngine::Published, this, &MainWindow::OnSnapshot);
  connect(flightPage_, &Page::AutoCaptureFinished, this, [this] { StopFlightCapture(); });
  alerts_.SetThresholds(alerts::FromSettings());
  // Offscreen runs (tests, page captures) must never reach the real desktop.
  if (QGuiApplication::platformName() != "offscreen") speaker_ = std::make_unique<notify::Speaker>();
  palette_ = new CommandPalette(this);
  qApp->installEventFilter(new KeyNav(
      this, [this] { if (auto *p = qobject_cast<Page *>(stack_->currentWidget())) p->FocusFilter(); },
      [this] {
        // Back from a filter to the page's main list.
        auto *p = qobject_cast<Page *>(stack_->currentWidget());
        if (!p) return;
        for (auto *a : p->findChildren<TableAtlas *>())  // the cards stand in for every table
          if (a->isVisible()) {
            a->setFocus();
            return;
          }
        for (auto *v : p->findChildren<QAbstractItemView *>())
          if (v->isVisible() && v->focusPolicy() != Qt::NoFocus) {
            v->setFocus();
            return;
          }
      }));
  ApplyIcons();
  InitializeTray();
  connect(&OmarchyPalette::Get(), &OmarchyPalette::Changed, this, [this] {
    ApplyIcons();
    if (auto s = engine_.Latest()) {
      UpdateTray(s);
      UpdateStatusLine(*s);  // its glyphs are baked in the palette colours
    }
    flightDockRunning_ = -1;  // re-colour the dock clock
    RefreshFlightDock();
    update();
  });
  connect(&Settings::Get(), &Settings::Changed, this, [this](const QString &key) {
    if (key == "updateSpeed") engine_.SetInterval(Settings::Get().intervalMs());
    if (key == "alwaysOnTop") ApplyAlwaysOnTop();
    if (key == "showTrayIcon") {
      tray_->setVisible(Settings::Get().value("showTrayIcon", true).toBool());
      if (!tray_->isVisible() && !isVisible()) Summon();
    }
    if (key == "updateSpeed" || key == "alwaysOnTop") SyncMenuChecks();  // set from the Settings dialog too
    if (key.startsWith("alert")) alerts_.SetThresholds(alerts::FromSettings());
    if (key == "temperatureUnit" || key == "smoothGraphCurves" || key == "processGrouping")
      if (auto s = engine_.Latest()) OnSnapshot(s);
  });
  engine_.Start(Settings::Get().intervalMs());
  ShowPage(Settings::Get().defaultPage());
  if (Settings::Get().sidebarHidden()) sidebar_->hide();
  ApplyAlwaysOnTop();
  QByteArray geometry = QByteArray::fromBase64(Settings::Get().value("windowGeometry").toByteArray());
  if (!geometry.isEmpty()) restoreGeometry(geometry);
}

void MainWindow::ApplyIcons() {
  setWindowIcon(AppIcon());
  for (int r = 0; r < nav_->count(); ++r) {
    int idx = nav_->item(r)->data(Qt::UserRole).toInt();
    if (idx >= 0) nav_->item(r)->setIcon(NavIcon(NavIconId(pages_[idx]->Id()), theme::Text()));
    else nav_->item(r)->setForeground(theme::Accent());
  }
  settingsButton_->setIcon(NavIcon("settings", theme::Dim()));
  for (auto [button, id] : {std::pair{flightStart_, "record"}, {flightStop_, "stop"}, {flightHistory_, "history"},
      {flightSave_, "save"}, {flightImport_, "import"}, {flightClear_, "clear"}, {flightLive_, "live"}})
    if (button) button->setIcon(PixelIcon(id, button == flightStart_ ? theme::Bad() : theme::Accent()));
  burger_->setIcon(NavIcon("menu", theme::Text()));
  refreshButton_->setIcon(NavIcon("refresh", theme::Text()));
}

void MainWindow::SyncMenuChecks() {
  QSignalBlocker b(alwaysOnTop_);
  alwaysOnTop_->setChecked(Settings::Get().alwaysOnTop());
  if (QAction *a = speedActions_.value(Settings::Get().updateSpeed())) a->setChecked(true);
}

QStringList MainWindow::PageIds() const {
  QStringList ids;
  for (Page *p : pages_) ids << p->Id();
  return ids;
}

void MainWindow::BuildShell() {
  auto *central = new QWidget;
  auto *root = new QHBoxLayout(central);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // Sidebar: the page list, then the settings key at the bottom.
  sidebar_ = new QWidget;
  sidebar_->setObjectName("Sidebar");
  sidebar_->setFixedWidth(230);
  auto *sv = new QVBoxLayout(sidebar_);
  sv->setContentsMargins(0, 8, 0, 8);
  nav_ = new QListWidget;
  nav_->setIconSize(QSize(18, 18));
  nav_->setFont(theme::Mono(14));
  nav_->setFocusPolicy(Qt::StrongFocus);
  nav_->installEventFilter(this);
  for (int i = 0; i < pages_.size(); ++i) {
    // Flight Recorder lives in the bottom dock: its row is hidden, but being
    // current it keeps the list from selecting its first page when focused
    // (QAbstractItemView picks a row on focus-in when none is current).
    if (pages_[i]->Id() == "flight-recorder") {
      auto *hidden = new QListWidgetItem(pages_[i]->Title());
      hidden->setData(Qt::UserRole, i);
      nav_->addItem(hidden);
      hidden->setHidden(true);
      continue;
    }
    if (pages_[i]->Id() == "connections") {
      auto *sep = new QListWidgetItem(tr("▪▪ MORE"));
      sep->setFlags(Qt::NoItemFlags);
      sep->setFont(theme::Mono(11, true));
      sep->setData(Qt::UserRole, -1);
      nav_->addItem(sep);
    }
    auto *it = new QListWidgetItem(pages_[i]->Title());
    it->setData(Qt::UserRole, i);
    it->setToolTip(i < 9 ? tr("%1 · Ctrl+%2").arg(pages_[i]->Title()).arg(i + 1) : pages_[i]->Title());
    nav_->addItem(it);
  }
  connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
    if (row < 0) return;
    int idx = nav_->item(row)->data(Qt::UserRole).toInt();
    if (idx >= 0) Activate(idx);
  });
  sv->addWidget(nav_, 1);
  auto *settingsBtn = settingsButton_ = new QToolButton;
  settingsBtn->setText(tr("Settings"));
  settingsBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  settingsBtn->setAutoRaise(true);
  connect(settingsBtn, &QToolButton::clicked, this, [this] {
    QStringList titles;
    for (Page *p : pages_) titles << p->Title();
    ShowSettingsDialog(this, PageIds(), titles);
  });
  sv->addWidget(settingsBtn, 0, Qt::AlignLeft);
  root->addWidget(sidebar_);

  // Content: header, the page stack, then the status bar.
  auto *content = new QWidget;
  auto *cv = new QVBoxLayout(content);
  cv->setContentsMargins(0, 0, 0, 0);
  cv->setSpacing(0);
  auto *header = new QWidget;
  header->setObjectName("Header");
  auto *hl = new QHBoxLayout(header);
  hl->setContentsMargins(12, 8, 12, 8);
  auto *burger = burger_ = new QToolButton;
  burger->setToolTip(tr("Hide or show the sidebar (Ctrl+B)"));
  burger->setAutoRaise(true);
  connect(burger, &QToolButton::clicked, this, &MainWindow::ToggleSidebar);
  hl->addWidget(burger);
  title_ = new QLabel;
  title_->setObjectName("PageTitle");
  title_->setFont(theme::Mono(22, true));
  title_->setMinimumWidth(QFontMetrics(title_->font()).horizontalAdvance("Performance") + 8);
  hl->addWidget(title_);
  auto *headerStack = new QStackedWidget;
  // Takes what the title and Refresh leave: the widest page's controls must not set the window's minimum.
  headerStack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  headerStack->setMinimumWidth(0);
  for (Page *p : pages_) {
    auto *w = new QWidget;
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->addStretch(1);  // page controls sit right-aligned next to Refresh
    p->BuildHeader(l);
    new HeaderFit(w);
    headerStack->addWidget(w);
    headerWidgets_ << w;
  }
  headerSlot_ = headerStack;
  hl->addWidget(headerStack, 1);
  auto *refresh = refreshButton_ = new QPushButton(tr("&Refresh"));
  refresh->setToolTip(tr("Sample now (F5)"));
  connect(refresh, &QPushButton::clicked, this, [this] { engine_.RefreshNow(); });
  hl->addWidget(refresh);
  moreButton_ = new QToolButton;
  moreButton_->setText("⋯");
  moreButton_->setToolTip(tr("All commands (F10)"));
  moreButton_->setPopupMode(QToolButton::InstantPopup);
  hl->addWidget(moreButton_);
  cv->addWidget(header);

  stack_ = new QStackedWidget;
  // A hidden page's wide controls must not impose their minimum size on every page.
  stack_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  stack_->setMinimumSize(0, 0);
  for (Page *p : pages_) {
    stack_->addWidget(p);
    connect(p, &Page::Status, this, [this](const QString &m) {
      transient_->setText(m);
      QTimer::singleShot(6000, transient_, [this, m] {
        if (transient_->text() == m) transient_->clear();
      });
    });
  }
  cv->addWidget(stack_, 1);

  auto *bar = new QWidget;
  bar->setObjectName("StatusBar");
  auto *bl = new QHBoxLayout(bar);
  bl->setContentsMargins(10, 4, 10, 4);
  bl->setSpacing(8);
  for (auto &icon : statusIcons_) {
    icon = new QLabel;
    icon->setAlignment(Qt::AlignCenter);
    icon->setFocusPolicy(Qt::NoFocus);
    bl->addWidget(icon);
  }
  transient_ = new QLabel;
  transient_->setObjectName("Dim");
  transient_->setTextFormat(Qt::PlainText);  // messages name processes, units and files
  transient_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  bl->addWidget(transient_, 1);
  recordStatus_ = new QLabel;
  recordStatus_->setObjectName("Dim");
  recordStatus_->setTextFormat(Qt::RichText);
  connect(recordStatus_, &QLabel::linkActivated, this, [this] { ToggleRecording(); });
  bl->addWidget(recordStatus_);
  BuildFlightDock(cv, bl);
  cv->addWidget(bar);
  root->addWidget(content, 1);
  setCentralWidget(central);
}

void MainWindow::BuildMenus() {
  // No menu bar: every command is a window-wide shortcut, and the "⋯" key in
  // the header (F10) lists them, with the current page's own commands on top.
  mainMenu_ = new QMenu(this);
  auto add = [this](QMenu *m, const QString &text, const QList<QKeySequence> &keys, auto fn) {
    QAction *a = new QAction(text, this);
    a->setShortcuts(keys);
    a->setShortcutContext(Qt::WindowShortcut);  // includes focused child widgets, excludes separate windows
    connect(a, &QAction::triggered, this, fn);
    addAction(a);
    m->addAction(a);
    return a;
  };
  pageSection_ = mainMenu_->addSeparator();
  add(mainMenu_, tr("&Command palette…"), {QKeySequence(Qt::CTRL | Qt::Key_K), QKeySequence(Qt::CTRL | Qt::Key_P)},
      [this] { OpenPalette(); });
  add(mainMenu_, tr("Run &new task…"), {QKeySequence(Qt::CTRL | Qt::Key_N)}, [this] { RunNewTask(); });
  recordAction_ = add(mainMenu_, tr("Record page video…"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R)},
                     [this] { ToggleRecording(); });
  add(mainMenu_, tr("Open page in a separate window"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P)},
      [this] { OpenPopout(); });
  add(mainMenu_, tr("&Find"), {QKeySequence::Find}, [this] {
    if (auto *p = qobject_cast<Page *>(stack_->currentWidget())) p->FocusFilter();
  });
  // F5 only: the platform's Refresh also means Ctrl+R, which a page may use (Disk Space: Start Scan).
  add(mainMenu_, tr("&Refresh"), {QKeySequence(Qt::Key_F5)}, [this] { engine_.RefreshNow(); });
  mainMenu_->addSeparator();
  add(mainMenu_, tr("Ne&xt page"), {QKeySequence(Qt::CTRL | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageDown)},
      [this] { Step(1); });
  add(mainMenu_, tr("Pre&vious page"),
      {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab),
       QKeySequence(Qt::CTRL | Qt::Key_PageUp)},
      [this] { Step(-1); });
  add(mainMenu_, tr("&Hide sidebar"), {QKeySequence(Qt::CTRL | Qt::Key_B)}, [this] { ToggleSidebar(); });
  add(mainMenu_, tr("Sort the &list…"), {QKeySequence(Qt::CTRL | Qt::Key_G)}, [this] {
    for (auto *atlas : stack_->currentWidget()->findChildren<TableAtlas *>())
      if (atlas->isVisible()) {
        atlas->ShowSortMenu();
        return;
      }
  });
  // Window-wide, so it works with the sidebar or a button focused, not only the list;
  // a filter field takes the letter as text (line edits claim printable keys first).
  rowsAction_ = add(mainMenu_, tr("Rows or cards"), {QKeySequence(Qt::Key_V)}, [this] {
    auto *page = qobject_cast<Page *>(stack_->currentWidget());
    if (!page) return;
    const auto atlases = page->CurrentPart()->findChildren<TableAtlas *>();
    TableAtlas *first = nullptr;
    for (auto *atlas : atlases)
      if (atlas->isVisible() && !first) first = atlas;
    if (!first) return;
    const bool dense = !first->Dense();
    for (auto *atlas : atlases) atlas->SetDense(dense);
  });
  // Flight Recorder lives in the dock and has no sidebar entry: reach it from the keyboard here.
  QMenu *flight = mainMenu_->addMenu(tr("F&light Recorder"));
  auto flightControls = [this] { return dynamic_cast<FlightRecorderControls *>(flightPage_); };
  add(flight, tr("&Start or stop recording"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F)},
      [this, flightControls] {
        if (!flightControls()) return;
        if (flightControls()->IsCapturing()) {
          StopFlightCapture();
        } else {
          flightControls()->StartCapture();
          RefreshFlightDock();
        }
      });
  add(flight, tr("Show &history"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H)}, [this] {
    if (flightPage_) ShowPage(flightPage_->Id());
  });
  add(flight, tr("Save &as…"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S)}, [flightControls] {
    if (flightControls()) flightControls()->SaveCapture();
  });
  add(flight, tr("&Open a saved session…"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O)}, [this, flightControls] {
    if (!flightControls()) return;
    flightControls()->ImportCapture();
    RefreshFlightDock();
    ShowPage(flightPage_->Id());
  });
  add(mainMenu_, tr("&Full screen"), {QKeySequence::FullScreen}, [this] { isFullScreen() ? showNormal() : showFullScreen(); });
  QMenu *speed = mainMenu_->addMenu(tr("&Update speed"));
  auto *group = new QActionGroup(speed);
  for (auto [key, label] : {std::pair{"high", tr("&High · 0.5 s")}, std::pair{"normal", tr("&Normal · 1 s")},
                            std::pair{"low", tr("&Low · 2 s")}, std::pair{"paused", tr("&Paused")}}) {
    QAction *a = speed->addAction(label);
    a->setCheckable(true);
    group->addAction(a);
    speedActions_.insert(key, a);
    QString k = key;
    connect(a, &QAction::triggered, this, [k] { Settings::Get().set("updateSpeed", k); });
  }
  QAction *top = alwaysOnTop_ = mainMenu_->addAction(tr("Always on &top"));
  top->setCheckable(true);
  SyncMenuChecks();
  connect(top, &QAction::toggled, this, [](bool on) { Settings::Get().set("alwaysOnTop", on); });
  mainMenu_->addSeparator();
  add(mainMenu_, tr("Se&ttings…"), {QKeySequence(Qt::CTRL | Qt::Key_Comma)}, [this] {
    QStringList titles;
    for (Page *p : pages_) titles << p->Title();
    ShowSettingsDialog(this, PageIds(), titles);
  });
  add(mainMenu_, tr("&About Regent"), {QKeySequence(Qt::Key_F1)}, [this] {
    QMessageBox about(this);
    about.setWindowTitle(tr("About Regent"));
    about.setIconPixmap(AppIcon().pixmap(64, 64));
    about.setTextFormat(Qt::RichText);
    about.setText(tr("<b>Regent %1</b><br>A keyboard-first system monitor and task manager for Linux, "
                     "made for Omarchy and Hyprland.<br><br>"
                     "<b>Ctrl+K</b> runs anything: pages, commands, processes, services.<br>"
                     "<b>j k h l / gg G</b> move through every list; <b>x</b> ends a process, <b>f</b> focuses its window.<br>"
                     "<b>regent --active-window</b> on a key shows the process behind the focused window; "
                     "<b>regent menu</b> picks a process from the Omarchy menu.<br><br>"
                     "For bars and scripts: <code>regent --stream 2000 --notify</code>, <code>regent --json --fields "
                     "cpu,mem,temp</code>, <code>regent top</code>, <code>regent kill NAME</code>.")
                      .arg(QApplication::applicationVersion()));
    about.exec();
  });
  add(mainMenu_, tr("E&xit"), {QKeySequence::Quit}, [this] { close(); });
  moreButton_->setMenu(mainMenu_);
  // Not a hidden menu action: hiding a QAction also disables its shortcut.
  auto *f10 = new QShortcut(QKeySequence(Qt::Key_F10), this);
  f10->setContext(Qt::WindowShortcut);
  connect(f10, &QShortcut::activated, moreButton_, &QToolButton::showMenu);

  // Ctrl+1 … Ctrl+9 jump straight to a page: everything is reachable from the keyboard.
  for (int i = 0; i < std::min<int>(9, int(pages_.size())); ++i) {
    auto *sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_1 + i)), this);
    connect(sc, &QShortcut::activated, this, [this, i] { Activate(i); });
  }
  // A page's own commands work by shortcut while it is on screen, wherever the
  // focus is (the sidebar, or the page's controls in the header): a window
  // shortcut on a hidden widget is inactive, and pages are hidden in the stack.
  for (Page *p : pages_)
    if (QMenu *m = p->PageMenu())
      for (QAction *a : m->actions()) {
        a->setShortcutContext(Qt::WindowShortcut);
        p->addAction(a);
      }
}

void MainWindow::Activate(int index) {
  if (index < 0 || index >= pages_.size()) return;
  Page *p = pages_[index];
  if (flightHistory_) flightHistory_->setChecked(p == flightPage_);
  stack_->setCurrentWidget(p);
  static_cast<QStackedWidget *>(headerSlot_)->setCurrentIndex(index);
  title_->setText(p->Title());
  for (int r = 0; r < nav_->count(); ++r)
    if (nav_->item(r)->data(Qt::UserRole).toInt() == index && nav_->currentRow() != r) {
      QSignalBlocker b(nav_);
      nav_->setCurrentRow(r);
    }
  SyncPageCommands(p);
  if (auto s = engine_.Latest()) p->Update(s);
}

void MainWindow::SyncPageCommands(Page *p) {
  // The page's own commands head the "⋯" menu.
  if (pageMenuAction_) mainMenu_->removeAction(pageMenuAction_);
  pageMenuAction_ = nullptr;
  if (QMenu *m = p->PageMenu()) pageMenuAction_ = mainMenu_->insertMenu(pageSection_, m);
  // Off where there is no list, so the letter reaches the focused widget there.
  if (rowsAction_) rowsAction_->setEnabled(!p->CurrentPart()->findChildren<TableAtlas *>().isEmpty());
}

bool MainWindow::eventFilter(QObject *o, QEvent *e) {
  // From the hidden Flight Recorder row QListView would jump to the top: step
  // to its visible neighbours instead.
  if (o != nav_ || e->type() != QEvent::KeyPress) return QMainWindow::eventFilter(o, e);
  const int key = static_cast<QKeyEvent *>(e)->key();
  QListWidgetItem *cur = nav_->currentItem();
  if ((key != Qt::Key_Up && key != Qt::Key_Down) || !cur || !cur->isHidden()) return false;
  const int step = key == Qt::Key_Down ? 1 : -1;
  for (int r = nav_->currentRow() + step; r >= 0 && r < nav_->count(); r += step)
    if (!nav_->item(r)->isHidden() && nav_->item(r)->data(Qt::UserRole).toInt() >= 0) {
      nav_->setCurrentRow(r);
      break;
    }
  return true;
}

void MainWindow::Step(int delta) {
  int i = pages_.indexOf(qobject_cast<Page *>(stack_->currentWidget()));
  for (int step = 0; step < pages_.size(); ++step) {
    i = (i + delta + int(pages_.size())) % int(pages_.size());
    if (pages_[i] != flightPage_) { Activate(i); return; }
  }
}

void MainWindow::ShowPage(const QString &id) {
  for (int i = 0; i < pages_.size(); ++i)
    if (pages_[i]->Id() == id) {
      Activate(i);
      return;
    }
  // A part of a page (power-and-freq, drivers…), named by alerts, --page and older settings.
  for (int i = 0; i < pages_.size(); ++i)
    if (pages_[i]->PartIds().contains(id)) {
      pages_[i]->ShowPart(id);
      Activate(i);
      return;
    }
  // Users became a grouping of Processes.
  if (id == "users") {
    Settings::Get().set("processGrouping", "user");
    ShowPage("processes");
    return;
  }
  Activate(0);
}

QStringList MainWindow::AllPageIds() const {
  QStringList ids;
  for (Page *p : pages_) ids << (p->PartIds().isEmpty() ? QStringList{p->Id()} : p->PartIds());
  return ids;
}

void MainWindow::resizeEvent(QResizeEvent *e) {
  QMainWindow::resizeEvent(e);
  // Tiled to half a screen, the sidebar folds to icons; the names stay as tooltips.
  // 1100: a window tiled to half of a 1920 px screen folds too, leaving the header its room.
  bool compact = width() < 1100;
  if (compact == compactSidebar_) return;
  compactSidebar_ = compact;
  sidebar_->setFixedWidth(compact ? 58 : 230);
  for (int r = 0; r < nav_->count(); ++r) {
    QListWidgetItem *it = nav_->item(r);
    if (!it->data(Qt::UserRole + 1).isValid()) it->setData(Qt::UserRole + 1, it->text());
    QString full = it->data(Qt::UserRole + 1).toString();
    bool sep = it->data(Qt::UserRole).toInt() < 0;
    // Folded, the "More" heading becomes a plain rule between the two groups.
    it->setText(compact ? (sep ? QStringLiteral("───") : QString()) : full);
    if (sep) it->setTextAlignment(compact ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter);
  }
  settingsButton_->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
  // Leave the header's room to the page's own filter and buttons.
  title_->setFont(theme::Mono(compact ? 16 : 22, true));
  title_->setMinimumWidth(compact ? 0 : QFontMetrics(title_->font()).horizontalAdvance("Performance") + 8);
  refreshButton_->setText(compact ? QString() : tr("&Refresh"));
}

void MainWindow::Summon() {
  if (isMinimized()) showNormal();
  show();
  raise();
  activateWindow();
  if (QWindow *w = windowHandle()) w->requestActivate();
}

void MainWindow::ToggleSidebar() {
  sidebar_->setVisible(!sidebar_->isVisible());
  Settings::Get().set("sidebarHidden", !sidebar_->isVisible());
}

void MainWindow::ApplyAlwaysOnTop() {
  bool on = Settings::Get().alwaysOnTop();
  if (bool(windowFlags() & Qt::WindowStaysOnTopHint) == on) return;
  setWindowFlag(Qt::WindowStaysOnTopHint, on);
  if (isVisible()) show();
}

void MainWindow::RunNewTask() {
  bool ok = false;
  QString cmd = QInputDialog::getText(this, tr("Run new task"),
                                      tr("Type the name of a program, with any arguments:"),
                                      QLineEdit::Normal, QString(), &ok);
  if (!ok || cmd.trimmed().isEmpty()) return;
  actions::Result r = actions::RunTask(cmd);
  if (!r.ok) safe::warning(this, tr("Run new task"), r.message);
}

void MainWindow::OnSnapshot(const SnapshotPtr &s) {
  UpdateTray(s);
  Page *current = qobject_cast<Page *>(stack_->currentWidget());
  for (Page *p : pages_)
    if (p == current || p == recordedPage_ || p->WantsBackgroundUpdates()) p->Update(s);
  RefreshFlightDock();  // the sample count moves with each snapshot
  CheckAlerts(*s);
  UpdateStatusLine(*s);
}

void MainWindow::CheckAlerts(const Snapshot &s) {
  // Failed units come from systemd every half minute, off the GUI thread.
  if (!unitsLoading_ && (!unitsChecked_.isValid() || unitsChecked_.elapsed() > 30000)) {
    unitsChecked_.start();
    if (alerts::FromSettings().failedUnits) {
      unitsLoading_ = true;
      using Answer = QPair<bool, QStringList>;
      RunAsync<Answer, MainWindow>(
          this,
          [] {
            bool systemOk = false, userOk = false;
            QStringList failed = services::Failed(services::Bus::System, &systemOk) +
                                 services::Failed(services::Bus::User, &userOk);
            return Answer{systemOk && userOk, failed};
          },
          [](MainWindow *w, const Answer &a) {
            w->unitsLoading_ = false;
            if (a.first) w->alerts_.SetFailedUnits(a.second);  // a D-Bus hiccup is not "nothing failed"
          });
    }
  }
  const auto fresh = alerts_.Feed(s);
  if (fresh.isEmpty() || !speaker_ || !Settings::Get().value("alertNotifications", true).toBool()) return;
  if (!speaker_->Holds()) return;  // the bar's `regent --stream --notify` speaks for this session
  for (const auto &a : fresh) notify::Send(a);
}

void MainWindow::SelectProcess(int pid) {
  ShowPage("processes");
  if (auto *p = qobject_cast<Page *>(stack_->currentWidget())) p->SelectProcess(pid);
}

QString MainWindow::HealthText() const {
  if (alerts_.Active().isEmpty()) return tr("No alerts · native providers healthy");
  QStringList lines;
  for (const auto &a : alerts_.Active()) lines << (a.level == alerts::Level::Critical ? "● " : "○ ") + a.title;
  return lines.join('\n');
}

void MainWindow::UpdateStatusLine(const Snapshot &s) {
  const qreal dpr = devicePixelRatioF();
  const struct {
    char32_t glyph;
    QColor color;
    QString tip;
  } items[5] = {{alerts_.Severity() ? char32_t(0xF0026) : char32_t(0xF05F6),                    // alert / heart-pulse
                 alerts_.Severity() == 2 ? theme::Bad() : alerts_.Severity() == 1 ? theme::Warn() : theme::Good(),
                 HealthText()},
                {0xF0279, theme::Text(), tr("%1 processes").arg(s.processes.size())},           // list
                {0xF0150, theme::Text(), tr("Up %1").arg(fs::humanDuration(s.uptimeSec))},      // clock
                {0xF0A60, theme::Text(),                                                        // ip-network
                 s.primaryIp.isEmpty() ? tr("No IP address") : tr("IP %1").arg(s.primaryIp)},
                {0xF0199, theme::Dim(), tr("Snapshot generation %1").arg(s.generation)}};      // counter
  for (int i = 0; i < 5; ++i) {
    // Rebuild the glyph only when its colour or the screen scale changes.
    const QString key = items[i].color.name() + QString::number(dpr);
    if (statusIcons_[i]->property("glyphKey").toString() != key) {
      statusIcons_[i]->setPixmap(StatusGlyph(items[i].glyph, items[i].color, dpr));
      statusIcons_[i]->setProperty("glyphKey", key);
    }
    statusIcons_[i]->setToolTip(safe::Tip(items[i].tip));  // alert titles carry unit names and mount points
    statusIcons_[i]->setAccessibleName(items[i].tip);
  }
}

}  // namespace td::ui

namespace td::ui {

namespace {
// A page's separate window asks the page before closing, as the main window
// does (Disk Space with a scan running, unsaved state).
class PopoutWindow : public QWidget {
public:
  explicit PopoutWindow(QWidget *parent) : QWidget(parent, Qt::Window) {}
  Page *page = nullptr;

protected:
  void closeEvent(QCloseEvent *e) override {
    if (page && !page->CanClose()) e->ignore();
    else e->accept();
  }
};
}  // namespace

void MainWindow::OpenPopout() {
  Page *current = qobject_cast<Page *>(stack_->currentWidget());
  if (!current) return;
  current = current->CurrentPart();
  // A second view shares the sampling engine; closing it never reparents the main page.
  using Factory = Page *(*)(QWidget *);
  static const QHash<QString, Factory> factories = {
      {"summary", CreateSummaryPage}, {"performance", CreatePerformancePage},
      {"processes", CreateProcessesPage}, {"system-info", CreateSystemInfoPage},
      {"startup-apps", CreateStartupPage},
      {"services", CreateServicesPage}, {"units", CreateUnitsPage}, {"power-and-freq", CreatePowerFreqPage},
      {"connections", CreateConnectionsPage}, {"installed-apps", CreateInstalledAppsPage},
      {"drivers", CreateDriversPage}, {"disk-space", CreateDiskSpacePage}};
  Factory create = factories.value(current->Id(), nullptr);
  if (!create) {
    transient_->setText(tr("This page is available in the main window only."));
    return;
  }
  auto *window = new PopoutWindow(this);
  window->setAttribute(Qt::WA_DeleteOnClose);
  window->setWindowTitle(tr("Regent · %1").arg(current->Title()));
  window->setWindowIcon(windowIcon());
  window->resize(900, 650);
  auto *layout = new QVBoxLayout(window);
  Page *page = window->page = create(window);
  page->setProperty("pageId", page->Id());
  InstallResponsiveTables(page);
  auto *header = new QHBoxLayout;
  auto *top = new QCheckBox(tr("Always on top"));
  header->addWidget(top);
  connect(top, &QCheckBox::toggled, window, [window](bool on) {
    window->setWindowFlag(Qt::WindowStaysOnTopHint, on);
    window->show();
  });
  header->addStretch();
  page->BuildHeader(header);
  auto *find = new QShortcut(QKeySequence::Find, window);
  connect(find, &QShortcut::activated, page, [page] { page->FocusFilter(); });
  auto *refresh = new QShortcut(QKeySequence(Qt::Key_F5), window);
  connect(refresh, &QShortcut::activated, this, [this] { engine_.RefreshNow(); });
  auto *close = new QShortcut(QKeySequence::Close, window);
  connect(close, &QShortcut::activated, window, &QWidget::close);
  layout->addLayout(header);
  layout->addWidget(page, 1);
  auto *status = new QLabel;  // the page's messages (scan finished, signal sent, errors)
  status->setObjectName("Dim");
  status->setTextFormat(Qt::PlainText);
  status->setWordWrap(true);
  status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  layout->addWidget(status);
  connect(page, &Page::Status, status, [status](const QString &m) {
    status->setText(m);
    QTimer::singleShot(6000, status, [status, m] {
      if (status->text() == m) status->clear();
    });
  });
  if (QMenu *menu = page->PageMenu()) {
    auto *commands = new QToolButton;
    commands->setText(tr("Commands"));
    commands->setPopupMode(QToolButton::InstantPopup);
    commands->setMenu(menu);
    header->addWidget(commands);
    for (QAction *action : menu->actions()) {
      action->setShortcutContext(Qt::WindowShortcut);
      page->addAction(action);
    }
  }
  connect(&engine_, &SamplingEngine::Published, page, [page](const SnapshotPtr &s) {
    if (page->isVisible() || page->WantsBackgroundUpdates()) page->Update(s);
  });
  if (auto s = engine_.Latest()) page->Update(s);
  window->show();
}

void MainWindow::InitializeTray() {
  tray_ = new QSystemTrayIcon(windowIcon(), this);
  tray_->setToolTip(tr("Regent"));
  auto *menu = new QMenu(this);
  menu->addAction(tr("Show Regent"), this, &MainWindow::Summon);
  menu->addAction(tr("Processes"), this, [this] { ShowPage("processes"); Summon(); });
  menu->addAction(tr("Performance"), this, [this] { ShowPage("performance"); Summon(); });
  menu->addSeparator();
  menu->addAction(tr("Exit"), this, [this] { close(); });
  tray_->setContextMenu(menu);
  connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) Summon();
  });
  tray_->setVisible(Settings::Get().value("showTrayIcon", true).toBool());
}

void MainWindow::UpdateTray(const SnapshotPtr &s) {
  if (!tray_ || !tray_->isVisible()) return;
  double memory = s->memTotal ? 100.0 * s->memUsed() / s->memTotal : 0;
  QPixmap pixmap(32, 32);
  pixmap.fill(theme::Bg());
  QPainter painter(&pixmap);
  painter.fillRect(3, 3, 11, 26, theme::Raised());
  painter.fillRect(18, 3, 11, 26, theme::Raised());
  int cpuHeight = qRound(qBound(0.0, s->cpuUtil, 100.0) * 0.26);
  int memoryHeight = qRound(qBound(0.0, memory, 100.0) * 0.26);
  painter.fillRect(3, 29 - cpuHeight, 11, cpuHeight, theme::Cpu());
  painter.fillRect(18, 29 - memoryHeight, 11, memoryHeight, theme::Memory());
  painter.end();
  tray_->setIcon(QIcon(pixmap));
  tray_->setToolTip(tr("Regent\nCPU: %1%\nMemory: %2% (%3 / %4)\n%5 processes")
      .arg(s->cpuUtil, 0, 'f', 1).arg(memory, 0, 'f', 1)
      .arg(fs::humanBytes(s->memUsed()), fs::humanBytes(s->memTotal)).arg(s->processes.size()));
}

void MainWindow::changeEvent(QEvent *event) {
  QMainWindow::changeEvent(event);
  if (event->type() == QEvent::WindowStateChange && isMinimized() && tray_ && tray_->isVisible() &&
      QSystemTrayIcon::isSystemTrayAvailable() && Settings::Get().value("hideWhenMinimized", false).toBool()) {
    QTimer::singleShot(0, this, [this] { if (isMinimized()) hide(); });
  }
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (recorder_ && recorder_->IsRecording()) {
    quitAfterRecording_ = true;
    recordTimer_->stop();
    recorder_->Stop();
    transient_->setText(tr("Finishing the video before exiting…"));
    event->ignore();
    return;
  }
  for (Page *page : pages_)
    if (!page->CanClose()) {
      event->ignore();
      return;
    }
  Settings::Get().set("windowGeometry", saveGeometry().toBase64());
  QMainWindow::closeEvent(event);
  if (event->isAccepted()) QTimer::singleShot(0, qApp, &QApplication::quit);
}

void MainWindow::ToggleRecording() {
  if (recorder_->IsRecording()) {
    recordTimer_->stop();
    recorder_->Stop();
    recordStatus_->setText(tr("Finishing video…"));
    return;
  }
  QStringList titles;
  for (Page* page : pages_) titles << page->Title();
  bool ok = false;
  const QString title = QInputDialog::getItem(this, tr("Record page video"), tr("Page to record:"), titles,
                                              stack_->currentIndex(), false, &ok);
  if (!ok) return;
  int index = titles.indexOf(title);
  QString directory = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
  if (directory.isEmpty()) directory = QDir::homePath();
  QString path = QFileDialog::getSaveFileName(
      this, tr("Record page video"),
      directory + "/regent-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".webm",
      tr("WebM video (*.webm)"));
  if (path.isEmpty()) return;
  // The dialog already asked before returning an existing name; the recorder
  // itself never overwrites, so replace that file here. A name we extend was not confirmed.
  bool confirmedOverwrite = QFileInfo::exists(path);
  if (!path.endsWith(".webm", Qt::CaseInsensitive)) {
    path += ".webm";
    confirmedOverwrite = false;
  }
  if (confirmedOverwrite && !QFile::remove(path)) {
    safe::warning(this, tr("Record page video"), tr("Cannot replace %1.").arg(path));
    return;
  }
  Activate(index);
  recordedPage_ = pages_[index];
  if (!recorder_->Start(recordedPage_, path)) recordedPage_ = nullptr;
}

}  // namespace td::ui

namespace td::ui {
void MainWindow::BuildFlightDock(QVBoxLayout* layout, QHBoxLayout* bar) {
  // A compact strip at the right end of the status line.
  auto* dock = new QWidget;
  dock->setObjectName("FlightDock");
  auto* row = new QHBoxLayout(dock);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(3);
  auto* label = new QLabel("FLIGHT");
  label->setObjectName("Dim");
  label->setFont(theme::Mono(9, true));
  row->addWidget(label);
  auto button = [&](QToolButton** out, const QString& name, const QString& tip, auto callback) {
    *out = new QToolButton;
    (*out)->setObjectName(name);
    (*out)->setToolTip(tip);
    (*out)->setAccessibleName(tip);
    (*out)->setIconSize(QSize(14, 14));
    (*out)->setFixedSize(24, 24);
    connect(*out, &QToolButton::clicked, this, callback);
    row->addWidget(*out);
  };
  auto controls = [this] { return dynamic_cast<FlightRecorderControls*>(flightPage_); };
  button(&flightStart_, "FlightStart", tr("Start or resume Flight Recorder"), [this, controls] {
    controls()->StartCapture();
    RefreshFlightDock();
  });
  button(&flightStop_, "FlightStop", tr("Stop and save Flight Recorder"), [this] { StopFlightCapture(); });
  button(&flightHistory_, "FlightHistory", tr("View Flight Recorder history"),
         [this] { ShowPage(flightPage_->Id()); });
  flightHistory_->setCheckable(true);
  button(&flightLive_, "FlightLive", tr("Follow the latest recorded sample"), [this, controls] {
    controls()->FollowLatest();
    ShowPage(flightPage_->Id());
  });
  button(&flightSave_, "FlightSave", tr("Save Flight Recorder as…"),
         [controls] { controls()->SaveCapture(); });
  button(&flightImport_, "FlightImport", tr("Open a saved Flight Recorder session"), [this, controls] {
    controls()->ImportCapture();
    RefreshFlightDock();
    ShowPage(flightPage_->Id());
  });
  button(&flightClear_, "FlightClear", tr("Clear Flight Recorder history"), [this, controls] {
    controls()->ClearCapture();
    RefreshFlightDock();
  });
  row->addSpacing(6);
  flightTime_ = new QLabel("00:00:00");
  flightTime_->setObjectName("FlightTime");
  flightTime_->setFont(theme::Mono(11, true));
  row->addWidget(flightTime_);
  flightCount_ = new QLabel;
  flightCount_->setObjectName("Dim");
  flightCount_->setFont(theme::Mono(10));
  row->addWidget(flightCount_);
  bar->addWidget(dock);
  saveNotice_ = new QWidget;
  saveNotice_->setObjectName("SaveNotice");
  auto* notice = new QHBoxLayout(saveNotice_);
  notice->setContentsMargins(12, 6, 12, 6);
  savePath_ = new QLabel;
  savePath_->setObjectName("CaptureSavePath");
  savePath_->setTextFormat(Qt::PlainText);
  savePath_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  savePath_->setWordWrap(true);
  QSizePolicy noticePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  noticePolicy.setHeightForWidth(true);
  savePath_->setSizePolicy(noticePolicy);
  notice->addWidget(savePath_, 1);
  auto* folder = new QToolButton;
  folder->setObjectName("CaptureFolder");
  folder->setText(tr("Open folder"));
  folder->setToolTip(tr("Open the folder containing the saved recording"));
  connect(folder, &QToolButton::clicked, this, [this] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(savedCapturePath_).absolutePath()));
  });
  notice->addWidget(folder);
  auto* dismiss = new QToolButton;
  dismiss->setText("×");
  dismiss->setAccessibleName(tr("Dismiss save notification"));
  connect(dismiss, &QToolButton::clicked, saveNotice_, &QWidget::hide);
  notice->addWidget(dismiss);
  saveNotice_->hide();
  layout->addWidget(saveNotice_);
  connect(flightPage_, &Page::CaptureSaved, this, &MainWindow::NotifyCaptureSaved);
  // The clock ticks only while recording, and only repaints while the window is on screen;
  // sample counts follow the snapshots (OnSnapshot).
  flightTimer_ = new QTimer(this);
  flightTimer_->setInterval(500);
  connect(flightTimer_, &QTimer::timeout, this, [this] {
    if (isVisible() && !isMinimized()) RefreshFlightDock();
  });
  connect(flightPage_, &Page::CaptureStateChanged, this, [this](bool running) {
    running ? flightTimer_->start() : flightTimer_->stop();
    RefreshFlightDock();
  });
  RefreshFlightDock();
}
void MainWindow::RefreshFlightDock() {
  auto* controls = dynamic_cast<FlightRecorderControls*>(flightPage_);
  if (!controls) return;
  const bool running = controls->IsCapturing();
  flightStart_->setEnabled(!running);
  flightStop_->setEnabled(running);
  flightSave_->setEnabled(controls->SampleCount() > 0);
  flightClear_->setEnabled(controls->SampleCount() > 0);
  const qint64 seconds = controls->CaptureElapsedMs() / 1000;
  flightTime_->setText(QString("%1:%2:%3")
                           .arg(seconds / 3600, 2, 10, QChar('0'))
                           .arg(seconds / 60 % 60, 2, 10, QChar('0'))
                           .arg(seconds % 60, 2, 10, QChar('0')));
  if (flightDockRunning_ != int(running)) {  // a style sheet repolishes: set it on changes only
    flightDockRunning_ = int(running);
    flightTime_->setStyleSheet(QString("color:%1").arg(running ? theme::Bad().name() : theme::Text().name()));
  }
  flightCount_->setText(tr("%1 · %2").arg(running ? tr("REC") : tr("STOP")).arg(controls->SampleCount()));
  flightCount_->setToolTip(tr("Flight Recorder %1 · %2 samples")
                               .arg(running ? tr("recording") : tr("stopped"))
                               .arg(controls->SampleCount()));
}
void MainWindow::StopFlightCapture() {
  auto* controls = dynamic_cast<FlightRecorderControls*>(flightPage_);
  controls->StopCapture();
  RefreshFlightDock();
  if (controls->SampleCount() == 0) return;
  QString directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  if (directory.isEmpty()) directory = QDir::homePath();
  directory =
      Settings::Get().value("flightRecordingDirectory", directory + "/Regent/recordings").toString();
  if (!QDir().mkpath(directory)) {
    safe::warning(this, tr("Flight Recorder"),
                         tr("Cannot create %1. Choose another destination.").arg(directory));
    controls->SaveCapture();
    return;
  }
  const QString path = directory + "/flight-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") +
                       ".regent.json";
  controls->SaveCapture(path);
}
void MainWindow::NotifyCaptureSaved(const QString& path) {
  savedCapturePath_ = QFileInfo(path).absoluteFilePath();
  savePath_->setText(tr("Flight Recorder saved to %1").arg(savedCapturePath_));
  savePath_->setToolTip(safe::Tip(savedCapturePath_));
  saveNotice_->show();
  if (tray_ && QSystemTrayIcon::isSystemTrayAvailable())
    tray_->showMessage(tr("Flight Recorder saved"), savedCapturePath_, QSystemTrayIcon::Information, 8000);
  RefreshFlightDock();
}
}  // namespace td::ui

namespace td::ui {

void MainWindow::OpenPalette() {
  QVector<CommandPalette::Entry> entries;
  auto plain = [](QString t) { return t.remove('&').remove(QStringLiteral("…")).trimmed(); };
  for (int i = 0; i < pages_.size(); ++i) {
    Page *p = pages_[i];
    entries.push_back({tr("Go to %1").arg(p->Title()), i < 9 ? tr("Page · Ctrl+%1").arg(i + 1) : tr("Page"),
                       [this, i] { Activate(i); }});
  }
  // Every command of the "⋯" menu, and every page's own commands (they switch to the page first).
  std::function<void(QMenu *, const QString &, Page *)> walk = [&](QMenu *menu, const QString &prefix, Page *page) {
    for (QAction *a : menu->actions()) {
      if (a->isSeparator() || !a->isVisible()) continue;
      if (a->menu()) {
        if (a == pageMenuAction_) continue;  // listed below with the other page menus
        walk(a->menu(), prefix + plain(a->text()) + ": ", page);
        continue;
      }
      if (!a->isEnabled()) continue;
      QPointer<QAction> action(a);
      QString hint = a->shortcut().toString(QKeySequence::NativeText);
      if (a->isCheckable()) hint = (a->isChecked() ? tr("On") : tr("Off")) + (hint.isEmpty() ? "" : " · " + hint);
      entries.push_back({prefix + plain(a->text()), hint, [this, action, page] {
                           if (page) ShowPage(page->Id());
                           if (action) action->trigger();
                         }});
    }
  };
  walk(mainMenu_, QString(), nullptr);
  for (Page *p : pages_)
    if (QMenu *m = p->PageMenu()) walk(m, p->Title() + ": ", p);

  // Processes: everything with a window, then the busiest others.
  if (SnapshotPtr s = engine_.Latest()) {
    QVector<const ProcessInfo *> list;
    for (const ProcessInfo &p : s->processes)
      if (p.hasWindow) list << &p;
    QVector<const ProcessInfo *> busy;
    for (const ProcessInfo &p : s->processes)
      if (!p.hasWindow) busy << &p;
    std::sort(busy.begin(), busy.end(), [](auto *a, auto *b) { return a->cpu > b->cpu; });
    list += busy.mid(0, 30);
    for (const ProcessInfo *pp : list) {
      const ProcessInfo p = *pp;
      const QString what = QString("%1 (%2)").arg(p.name).arg(p.key.pid);
      const QString load = tr("%1% CPU · %2").arg(p.cpu, 0, 'f', 1).arg(fs::humanBytes(p.rss));
      entries.push_back({what, tr("Process · %1").arg(load), [this, p] { SelectProcess(p.key.pid); }});
      if (!p.windowAddress.isEmpty())
        entries.push_back({tr("Focus %1 · %2").arg(p.name, p.windowTitle.left(60)),
                           tr("Window · workspace %1").arg(p.workspace), [this, p] {
                             if (!hypr::Focus(p.windowAddress)) transient_->setText(tr("The window is gone"));
                           }});
      if (actions::CriticalProcess(p.key.pid)) continue;  // shown, focusable, never ended from here
      entries.push_back({tr("End task %1").arg(what), tr("SIGTERM · %1").arg(load), [this, p] {
                           actions::Result r = actions::Signal(p.key, SIGTERM);
                           transient_->setText(r.ok ? tr("Ended %1").arg(p.name) : tr("End task failed: %1").arg(r.message));
                         }});
      entries.push_back({tr("Kill %1").arg(what), tr("SIGKILL · %1").arg(load), [this, p] {
                           if (safe::question(this, tr("Force terminate"),
                                                     tr("Kill %1 (%2) immediately? Unsaved data will be lost.")
                                                         .arg(p.name).arg(p.key.pid)) != QMessageBox::Yes)
                             return;
                           actions::Result r = actions::Signal(p.key, SIGKILL);
                           transient_->setText(r.ok ? tr("Killed %1").arg(p.name) : tr("Kill failed: %1").arg(r.message));
                         }});
    }
  }
  palette_->SetEntries(entries);
  palette_->Popup();

  // Services arrive a moment later: listing them asks systemd over D-Bus.
  using Lists = QPair<QVector<services::Service>, QVector<services::Service>>;
  const int generation = ++paletteGeneration_;
  RunAsync<Lists, MainWindow>(
      this,
      [] {
        QString e;
        return Lists{services::List(services::Bus::System, &e), services::List(services::Bus::User, &e)};
      },
      [generation](MainWindow *w, const Lists &lists) {
        if (generation != w->paletteGeneration_) return;
        QVector<CommandPalette::Entry> more;
        for (int b = 0; b < 2; ++b) {
          const services::Bus bus = b == 0 ? services::Bus::System : services::Bus::User;
          const QString kind = b == 0 ? tr("System service") : tr("User service");
          for (const services::Service &svc : b == 0 ? lists.first : lists.second) {
            if (svc.load == "not-found" || svc.fileState == "masked") continue;
            const QString unit = svc.name;
            auto control = [w, bus, unit](const QString &action, const QString &verb) {
              if (action != "StartUnit" &&
                  safe::question(w, verb, tr("%1 %2?").arg(verb, unit)) != QMessageBox::Yes)
                return;
              auto *watch = new QDBusPendingCallWatcher(services::Control(bus, unit, action), w);
              QObject::connect(watch, &QDBusPendingCallWatcher::finished, w, [w, watch, verb, unit] {
                watch->deleteLater();
                w->transient_->setText(watch->isError() ? tr("%1 %2 failed: %3").arg(verb, unit, watch->error().message())
                                                        : tr("%1 %2 succeeded").arg(verb, unit));
              });
            };
            const QString state = kind + " · " + svc.active;
            if (svc.active == "active" && actions::CriticalUnit(unit)) continue;  // the session stands on it
            if (svc.active == "active") {
              more.push_back({tr("Restart %1").arg(unit), state, [control] { control("RestartUnit", tr("Restart")); }});
              more.push_back({tr("Stop %1").arg(unit), state, [control] { control("StopUnit", tr("Stop")); }});
            } else {
              more.push_back({tr("Start %1").arg(unit), state, [control] { control("StartUnit", tr("Start")); }});
            }
          }
        }
        w->palette_->AddEntries(more);
      });
}

}  // namespace td::ui
