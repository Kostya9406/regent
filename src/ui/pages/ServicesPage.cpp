#include "ui/Safe.h"
#include "ui/pages/Pages.h"
#include "ui/Theme.h"

#include <QCoreApplication>
#include "core/Actions.h"
#include "core/Services.h"

#include <QComboBox>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QToolButton>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include "ui/Async.h"

#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace td::ui {

namespace {

class ServicesPage : public Page {
public:
  explicit ServicesPage(QWidget *parent) : Page(parent) {
    tree_ = new QTreeWidget;
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->setHeaderLabels({tr("Running"), tr("Name"), tr("PID"), tr("Description"), tr("State"), tr("Startup")});
    tree_->header()->resizeSection(0, 80);
    tree_->header()->resizeSection(1, 330);
    tree_->header()->resizeSection(2, 80);
    tree_->header()->resizeSection(3, 360);
    tree_->header()->resizeSection(4, 140);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(1, Qt::AscendingOrder);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
      QTreeWidgetItem *at = tree_->itemAt(pos);
      if (!at) return;
      tree_->setCurrentItem(at);
      QMenu m(this);
      m.addAction(tr("&Start"), this, [this] { Control("StartUnit"); });
      m.addAction(tr("S&top"), this, [this] { Control("StopUnit"); });
      m.addAction(tr("&Restart"), this, [this] { Control("RestartUnit"); });
      m.exec(tree_->viewport()->mapToGlobal(pos));
    });
    note_ = new QLabel;
    note_->setObjectName("Dim");
    note_->setWordWrap(true);
    // The selected unit's recent journal, errors in red: why did it fail?
    journal_ = new QPlainTextEdit;
    journal_->setObjectName("ServiceJournal");
    journal_->setReadOnly(true);
    journal_->setMaximumBlockCount(400);
    journal_->setLineWrapMode(QPlainTextEdit::NoWrap);
    journal_->setFont(theme::Mono(12));
    journal_->setPlaceholderText(tr("Select a service to see its journal"));
    journal_->setAccessibleName(tr("Service journal"));
    journalTimer_ = new QTimer(this);
    journalTimer_->setSingleShot(true);
    journalTimer_->setInterval(250);  // scrolling through the list must not spawn a journalctl per row
    connect(journalTimer_, &QTimer::timeout, this, [this] { LoadJournal(); });
    // The list is rebuilt every few seconds; reload only when another unit is selected.
    // The journal pane opens with the first selected service: empty, it only took room.
    journal_->hide();
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *cur) {
      // A list rebuild passes through "nothing selected": keep the pane (and its focus) meanwhile.
      if (cur) SyncJournal();
      if (startStop_ && cur) startStop_->setText(cur->data(0, Qt::UserRole).toBool() ? tr("Sto&p") : tr("&Start"));
      if (!journal_->isHidden() && cur && cur->text(1) != journalUnit_) journalTimer_->start();
    });
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 8);
    l->addWidget(tree_, 3);
    l->addWidget(journal_, 2);
    l->addWidget(note_);
  }

  QString Id() const override { return "services"; }
  QString Title() const override { return tr("Services"); }
  void FocusFilter() override { filter_->setFocus(); }

  void BuildHeader(QHBoxLayout *h) override {
    bus_ = new QComboBox;
    bus_->addItems({tr("System"), tr("User")});
    bus_->setToolTip(tr("System services, or your user's services (systemctl --user)"));
    bus_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(bus_, &QComboBox::currentIndexChanged, this, [this] {
      if (startStop_) startStop_->setEnabled(false);
      Reload();
    });
    h->addWidget(bus_);
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter services"));
    filter_->setClearButtonEnabled(true);
    filter_->setMinimumWidth(110);
    connect(filter_, &QLineEdit::textChanged, this, [this] { Show(); });
    h->addWidget(filter_, 1);
    // Says what it will do to the selected service: Start, or Stop.
    startStop_ = new QPushButton(tr("&Start"));
    startStop_->setToolTip(tr("Start or stop the selected service"));
    startStop_->setEnabled(false);
    connect(startStop_, &QPushButton::clicked, this, [this] {
      QTreeWidgetItem *it = tree_->currentItem();
      if (it) Control(it->data(0, Qt::UserRole).toBool() ? "StopUnit" : "StartUnit");
    });
    h->addWidget(startStop_);
    auto *log = new QToolButton;
    log->setText(tr("&Journal"));
    log->setCheckable(true);
    log->setChecked(true);
    log->setToolTip(tr("Show the selected service's journal (Ctrl+J)"));
    log->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_J));
    connect(log, &QToolButton::toggled, this, [this](bool on) {
      journalWanted_ = on;
      SyncJournal();
      if (!journal_->isHidden()) LoadJournal();
    });
    h->addWidget(log);
  }

  void Update(const SnapshotPtr &s) override {
    // The unit list is re-read every few seconds, not every sample.
    if (busy_ || (loaded_ && s->generation - loadedAt_ < 5)) return;
    loadedAt_ = s->generation;
    Reload();
  }

private:
  void SyncJournal() { journal_->setVisible(journalWanted_ && tree_->currentItem()); }

  services::Bus Bus() const {
    return bus_ && bus_->currentIndex() == 1 ? services::Bus::User : services::Bus::System;
  }

  void Reload() {
    if (busy_) {
      reloadPending_ = true;  // e.g. the bus changed mid-load: load again when this one lands
      return;
    }
    busy_ = true;
    if (startStop_) startStop_->setEnabled(listBus_ == Bus() && loaded_);
    services::Bus bus = Bus();
    struct Loaded {
      QVector<services::Service> list;
      QString error;
      services::Bus bus;
    };
    RunAsync<Loaded, ServicesPage>(
        this,
        [bus] {
          Loaded l{{}, {}, bus};
          l.list = services::List(bus, &l.error);
          return l;
        },
        [](ServicesPage *p, const Loaded &l) {
          p->busy_ = false;
          p->loaded_ = true;
          p->list_ = l.list;
          p->listBus_ = l.bus;
          p->error_ = l.error;
          p->Show();
          if (p->reloadPending_ || l.bus != p->Bus()) {
            p->reloadPending_ = false;
            p->Reload();
          }
        });
  }

  void Show() {
    // Controls act on the bus the list came from, and only when that is the bus on screen.
    if (startStop_) startStop_->setEnabled(listBus_ == Bus());
    QString cur = tree_->currentItem() ? tree_->currentItem()->text(1) : QString();
    QString f = filter_ ? filter_->text().trimmed() : QString();
    const int scroll = tree_->verticalScrollBar()->value();  // a refresh must not move the list
    tree_->setUpdatesEnabled(false);
    tree_->setSortingEnabled(false);
    tree_->clear();
    int running = 0;
    for (const services::Service &s : list_) {
      bool on = s.active == "active";
      running += on;
      if (!f.isEmpty() && !s.name.contains(f, Qt::CaseInsensitive) && !s.description.contains(f, Qt::CaseInsensitive)) continue;
      auto *it = new QTreeWidgetItem(tree_, {on ? "●" : "○", s.name, s.mainPid ? QString::number(s.mainPid) : "—",
                                             s.description, s.active + " (" + s.sub + ")", s.fileState});
      it->setData(0, Qt::UserRole, on);
      it->setForeground(0, s.active == "failed" ? theme::Bad() : on ? theme::Good() : theme::Dim());
      if (s.active == "failed") it->setForeground(4, theme::Bad());
      if (s.name == cur) tree_->setCurrentItem(it);
    }
    tree_->setSortingEnabled(true);
    tree_->verticalScrollBar()->setValue(scroll);
    tree_->setUpdatesEnabled(true);
    QString busName = listBus_ == services::Bus::System ? tr("system") : tr("user");
    note_->setText(error_.isEmpty() ? tr("%1 %2 services · %3 running · read over D-Bus from systemd")
                                          .arg(list_.size()).arg(busName).arg(running)
                                    : tr("systemd did not answer: %1").arg(error_));
  }

  void LoadJournal() {
    QTreeWidgetItem *it = tree_->currentItem();
    if (!it) return;
    const QString unit = it->text(1);
    journalUnit_ = unit;
    if (journalProc_) {
      journalProc_->disconnect(this);
      journalProc_->kill();
      journalProc_->deleteLater();
    }
    QStringList args;
    if (listBus_ == services::Bus::User) args << "--user";
    args << "--unit" << unit << "--lines" << "80" << "--no-pager" << "--quiet" << "--output" << "json"
         << "--output-fields" << "MESSAGE,PRIORITY";
    auto *proc = journalProc_ = new QProcess(this);
    connect(proc, &QProcess::finished, this, [this, proc, unit] {
      proc->deleteLater();
      if (journalProc_ == proc) journalProc_ = nullptr;
      if (unit != journalUnit_) return;
      ShowJournal(unit, proc->readAllStandardOutput(), QString::fromUtf8(proc->readAllStandardError()).trimmed());
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
      if (e != QProcess::FailedToStart) return;
      proc->deleteLater();
      if (journalProc_ == proc) journalProc_ = nullptr;
      journal_->setPlainText(tr("journalctl is not available."));
    });
    proc->start("journalctl", args);
  }

  void ShowJournal(const QString &unit, const QByteArray &out, const QString &err) {
    journal_->clear();
    int lines = 0;
    for (const QByteArray &line : out.split('\n')) {
      const QJsonObject o = QJsonDocument::fromJson(line).object();
      if (o.isEmpty()) continue;
      ++lines;
      // MESSAGE may arrive as an array of bytes when it is not valid UTF-8.
      QString msg = o.value("MESSAGE").isString() ? o.value("MESSAGE").toString() : tr("<binary message>");
      const qint64 usec = o.value("__REALTIME_TIMESTAMP").toString().toLongLong();
      const QString when = QDateTime::fromMSecsSinceEpoch(usec / 1000).toString("MM-dd HH:mm:ss");
      const int prio = o.value("PRIORITY").toString("6").toInt();
      const QColor color = prio <= 3 ? theme::Bad() : prio == 4 ? theme::Warn() : theme::Text();
      journal_->appendHtml(QString("<span style='color:%1'>%2</span> <span style='color:%3'>%4</span>")
                               .arg(theme::Dim().name(), when, color.name(), msg.toHtmlEscaped()));
    }
    if (lines == 0)
      journal_->setPlainText(!err.isEmpty() ? err
                                            : tr("No journal entries for %1 that you can read. System logs of other "
                                                 "units need membership in the wheel, adm or systemd-journal group.")
                                                  .arg(unit));
  }

  void Control(const QString &action) {
    QTreeWidgetItem *it = tree_->currentItem();
    if (!it || listBus_ != Bus() || busy_) return;
    QString unit = it->text(1);
    services::Bus bus = listBus_;
    QString busName = bus == services::Bus::System ? tr("system") : tr("user");
    QString verb = action == "StartUnit" ? tr("Start") : action == "StopUnit" ? tr("Stop") : tr("Restart");
    if (action != "StartUnit" && actions::CriticalUnit(unit)) {
      safe::information(this, verb, tr("%1 keeps the desktop session running; Regent will not %2 it.")
                                               .arg(unit, verb.toLower()));
      return;
    }
    if (action != "StartUnit" &&
        safe::question(this, verb, tr("%1 the %2 service %3?").arg(verb, busName, unit)) != QMessageBox::Yes)
      return;
    emit Status(tr("%1 %2…").arg(verb, unit));
    auto *w = new QDBusPendingCallWatcher(services::Control(bus, unit, action), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, unit, verb] {
      w->deleteLater();
      QDBusPendingReply<QDBusObjectPath> r = *w;
      if (!r.isError()) emit Status(tr("%1 %2 succeeded").arg(verb, unit));
      else safe::warning(this, verb, tr("%1 %2 failed: %3").arg(verb, unit, r.error().message()));
      loaded_ = false;
      Reload();
    });
  }

  QTreeWidget *tree_;
  QLabel *note_;
  QPlainTextEdit *journal_;
  QTimer *journalTimer_;
  QProcess *journalProc_ = nullptr;
  QString journalUnit_;
  QLineEdit *filter_ = nullptr;
  QComboBox *bus_ = nullptr;
  QVector<services::Service> list_;
  QString error_;
  QPushButton *startStop_ = nullptr;
  bool journalWanted_ = true;  // the Journal switch; the pane shows once a service is selected
  services::Bus listBus_ = services::Bus::System;
  bool busy_ = false, loaded_ = false, reloadPending_ = false;
  quint64 loadedAt_ = 0;
};

}  // namespace

Page *CreateServicesPage(QWidget *parent) { return new ServicesPage(parent); }

}  // namespace td::ui
