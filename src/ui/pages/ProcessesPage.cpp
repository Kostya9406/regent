#include "ui/Safe.h"
#include "ui/pages/Pages.h"

#include "core/Actions.h"
#include "core/Cgroups.h"
#include "core/Hyprland.h"
#include "core/Services.h"
#include "core/Fs.h"
#include "core/Settings.h"
#include "ui/Roles.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDBusPendingCallWatcher>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <csignal>

namespace td::ui {

namespace {

enum Col { cName, cPid, cStatus, cUser, cCpu, cMem, cGpu, cRead, cWrite, cWake, cThreads, cVirt, cFds, cNice,
           cWorkspace, cWindow, cCmd, cCount };
constexpr int kSortRole = Qt::UserRole + 1;
constexpr int kKeyRole = Qt::UserRole + 2;    // pid; with kStartRole forms the ProcessKey
constexpr int kStartRole = Qt::UserRole + 3;

// Parent sentinels for desiredParent(); real PIDs are never 0.
constexpr ProcessKey kTopLevel{0, 0};
constexpr ProcessKey kAppsGroup{0, 1};
constexpr ProcessKey kUserGroup{0, 2};  // the group of the process's user

QString StateName(QChar c) {
  switch (c.unicode()) {
    case 'R': return QObject::tr("Running");
    case 'S': return QObject::tr("Sleeping");
    case 'D': return QObject::tr("Disk wait");
    case 'Z': return QObject::tr("Zombie");
    case 'T': case 't': return QObject::tr("Stopped");
    case 'I': return QObject::tr("Idle");
    default: return QString(c);
  }
}

class Filter : public QSortFilterProxyModel {
public:
  using QSortFilterProxyModel::QSortFilterProxyModel;
  void SetText(const QString &t) {
    beginFilterChange();
    text_ = t.trimmed();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
  }

protected:
  bool filterAcceptsRow(int row, const QModelIndex &parent) const override {
    if (text_.isEmpty()) return true;
    QModelIndex idx = sourceModel()->index(row, cName, parent);
    if (!idx.data(kKeyRole).isValid()) return false;  // groups show only through their children
    for (int c : {cName, cPid, cUser, cCmd, cWindow})
      if (sourceModel()->index(row, c, parent).data().toString().contains(text_, Qt::CaseInsensitive)) return true;
    return false;
  }
  bool lessThan(const QModelIndex &a, const QModelIndex &b) const override {
    // Groups keep their order (Apps first) whatever column is sorted.
    if (!a.parent().isValid() && !a.siblingAtColumn(cName).data(kKeyRole).isValid())
      return sortOrder() == Qt::AscendingOrder ? a.row() < b.row() : a.row() > b.row();
    QVariant x = a.data(kSortRole), y = b.data(kSortRole);
    if (x.typeId() == QMetaType::QString) return QString::localeAwareCompare(x.toString(), y.toString()) < 0;
    return x.toDouble() < y.toDouble();
  }

private:
  QString text_;
};

void ShowProperties(QWidget *parent, const ProcessInfo &p) {
  QDialog d(parent);
  d.setWindowTitle(QObject::tr("%1 (PID %2)").arg(p.name).arg(p.key.pid));
  d.resize(720, 520);
  auto *tabs = new QTabWidget;
  QString base = QString("/proc/%1/").arg(p.key.pid);
  QString exe = QFileInfo(base + "exe").symLinkTarget();
  QString cwd = QFileInfo(base + "cwd").symLinkTarget();
  auto *general = new QLabel;
  general->setTextInteractionFlags(Qt::TextSelectableByMouse);
  general->setWordWrap(true);
  general->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  general->setText(QString("<table cellpadding='4'>"
                           "<tr><td>%1</td><td>%2</td></tr><tr><td>%3</td><td>%4</td></tr>"
                           "<tr><td>%5</td><td>%6</td></tr><tr><td>%7</td><td>%8</td></tr>"
                           "<tr><td>%9</td><td>%10</td></tr><tr><td>%11</td><td>%12</td></tr></table>")
                       .arg(QObject::tr("Executable"), exe.isEmpty() ? QObject::tr("not readable") : exe.toHtmlEscaped(),
                            QObject::tr("Working folder"), cwd.isEmpty() ? QObject::tr("not readable") : cwd.toHtmlEscaped(),
                            QObject::tr("Command line"), p.cmdline.toHtmlEscaped(), QObject::tr("User"), p.user.toHtmlEscaped(),
                            QObject::tr("Parent PID"), QString::number(p.ppid), QObject::tr("Threads / nice"),
                            QString("%1 / %2").arg(p.threads).arg(p.nice)));
  tabs->addTab(general, QObject::tr("General"));
  auto text = [](const std::string &raw, bool nulSeparated) {
    QString s = QString::fromUtf8(raw);
    if (nulSeparated) s.replace(QChar(0), '\n');
    return s;
  };
  auto addText = [&](const QString &title, const QString &body) {
    auto *e = new QPlainTextEdit(body.isEmpty() ? QObject::tr("Not readable: another user's process needs root.") : body);
    e->setReadOnly(true);
    e->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    tabs->addTab(e, title);
  };
  addText(QObject::tr("Status"), text(fs::readRaw(base + "status"), false));
  addText(QObject::tr("Environment"), text(fs::readRaw(base + "environ"), true));
  addText(QObject::tr("Limits"), text(fs::readRaw(base + "limits"), false));
  addText(QObject::tr("Memory map"), text(fs::readRaw(base + "smaps_rollup"), false));
  auto *v = new QVBoxLayout(&d);
  v->addWidget(tabs);
  auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
  QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  v->addWidget(bb);
  d.exec();
}

class ProcessesPage : public Page {
public:
  explicit ProcessesPage(QWidget *parent) : Page(parent) {
    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels({tr("Name"), tr("PID"), tr("Status"), tr("User name"), tr("CPU"),
                                       tr("Memory"), tr("GPU"), tr("Disk read"), tr("Disk write"), tr("Wakeups/s"),
                                       tr("Threads"), tr("Virtual"), tr("FDs"), tr("Nice"), tr("Workspace"),
                                       tr("Window"), tr("Command line")});
    model_->horizontalHeaderItem(cGpu)->setToolTip(tr("Busiest GPU engine used by the process (DRM fdinfo)"));
    model_->horizontalHeaderItem(cMem)->setToolTip(
        tr("Resident memory (RSS). Shared libraries count in every process that maps them, so the column adds up "
           "to more than the memory in use; Users counts private memory only"));
    model_->horizontalHeaderItem(cWake)->setToolTip(
        tr("How often the process's threads are woken up per second: frequent wakeups keep a laptop from idling"));
    proxy_ = new Filter(this);
    proxy_->setSourceModel(model_);
    proxy_->setRecursiveFilteringEnabled(true);
    proxy_->setSortRole(kSortRole);
    proxy_->setDynamicSortFilter(true);

    view_ = new QTreeView;
    view_->setModel(proxy_);
    view_->setSortingEnabled(true);
    view_->sortByColumn(cCpu, Qt::DescendingOrder);
    view_->setUniformRowHeights(true);
    view_->setAlternatingRowColors(true);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->header()->setStretchLastSection(true);
    view_->header()->resizeSection(cName, 260);
    for (int c = cPid; c < cCmd; ++c) view_->header()->resizeSection(c, c == cUser ? 110 : c == cWindow ? 220 : 86);
    view_->installEventFilter(this);
    connect(view_, &QTreeView::customContextMenuRequested, this, &ProcessesPage::ContextMenu);
    connect(view_, &QTreeView::doubleClicked, this, [this] {
      if (auto p = Selected(); p.size() == 1) ShowProperties(this, p.first());
    });
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ProcessesPage::ShowDetail);

    detail_ = new QLabel;  // shown only while something is selected
    detail_->setObjectName("Dim");
    detail_->setAlignment(Qt::AlignCenter);
    detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_->setMinimumHeight(48);
    detail_->setWordWrap(true);
    detail_->hide();

    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(view_, 1);
    l->addWidget(detail_);
  }

  QString Id() const override { return "processes"; }
  QString Title() const override { return tr("Processes"); }
  void FocusFilter() override { filter_->setFocus(); filter_->selectAll(); }

  bool SelectProcess(int pid) override {
    pendingPid_ = pid;
    pendingTries_ = 5;  // a window that has just opened may not have its first snapshot yet
    TrySelectPending();
    return true;
  }

  QMenu *PageMenu() override {
    if (menu_) return menu_;
    menu_ = new QMenu(tr("&Processes"), this);
    QAction *pick = menu_->addAction(tr("&Pick a window…"), this, [this] { StartPick(); });
    pick->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W));
    pick->setToolTip(tr("Focus any window, by click or keyboard, to find its process"));
    pick->setEnabled(hypr::Available());
    QAction *focus = menu_->addAction(tr("&Focus window"), this, [this] { FocusSelected(); });
    focus->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    menu_->addAction(tr("User &accounts…"), this, [this] {
      const actions::Result r = actions::OpenUserAccounts();
      if (!r.ok) safe::warning(this, tr("User accounts"), r.message);
    });
    return menu_;
  }

  void BuildHeader(QHBoxLayout *h) override {
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter by name, user, PID or command"));
    filter_->setClearButtonEnabled(true);
    filter_->setMinimumWidth(140);
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString &t) {
      proxy_->SetText(t);
      if (!t.isEmpty()) view_->expandAll();
    });
    h->addWidget(filter_);
    // Apps first (windows, then background), by user, or the parent tree.
    groupingBox_ = new QComboBox;
    groupingBox_->addItem(tr("By app"), "apps");
    groupingBox_->addItem(tr("By user"), "user");
    groupingBox_->addItem(tr("Tree"), "tree");
    groupingBox_->setToolTip(tr("Group processes: apps and background, by user, or as the parent tree"));
    groupingBox_->setCurrentIndex(std::max(0, groupingBox_->findData(Settings::Get().processGrouping())));
    connect(groupingBox_, &QComboBox::activated, this, [this] {
      Settings::Get().set("processGrouping", groupingBox_->currentData());
      if (snap_) Update(snap_);
    });
    h->addWidget(groupingBox_);
    auto *end = new QPushButton(tr("&End task"));
    end->setToolTip(tr("SIGTERM (Delete); Shift+Delete sends SIGKILL"));
    connect(end, &QPushButton::clicked, this, [this] { SignalSelected(SIGTERM); });
    h->addWidget(end);
  }

  void Update(const SnapshotPtr &s) override {
    snap_ = s;
    const QString grouping = Settings::Get().processGrouping();
    if (grouping != grouping_) {
      model_->removeRows(0, model_->rowCount());
      items_.clear();
      apps_ = background_ = nullptr;
      users_.clear();
      grouping_ = grouping;
      treeMode_ = grouping == "tree";
      byUser_ = grouping == "user";
      expandedOnce_ = false;
      if (groupingBox_) {
        QSignalBlocker block(groupingBox_);
        groupingBox_->setCurrentIndex(std::max(0, groupingBox_->findData(grouping)));
      }
    }
    if (!treeMode_ && !byUser_ && !apps_) {
      apps_ = Group(tr("Apps"));
      background_ = Group(tr("Background processes"));
    }
    if (byUser_) {
      // One section per user, yours first: who runs what, and how much it costs.
      struct Total { int n = 0; double cpu = 0; quint64 anon = 0; QString name; };
      QMap<uid_t, Total> totals;
      for (const ProcessInfo &p : s->processes) {
        Total &t = totals[p.uid];
        ++t.n;
        t.cpu += p.cpu;
        t.anon += p.anon;
        t.name = p.user;
      }
      QList<uid_t> order = totals.keys();
      std::stable_sort(order.begin(), order.end(), [](uid_t a, uid_t b) { return (a == getuid()) > (b == getuid()); });
      for (uid_t uid : order)
        if (!users_.contains(uid)) users_.insert(uid, Group(totals[uid].name));
      for (auto it = users_.begin(); it != users_.end(); ++it) {
        const Total t = totals.value(it.key());
        // Title only, so the list shows it as a section; " (" keeps its key stable.
        it.value()->setText(tr("%1 (%2 · CPU %3% · %4 private)")
                                .arg(t.name.isEmpty() ? QString::number(it.key()) : t.name).arg(t.n)
                                .arg(t.cpu, 0, 'f', 1).arg(fs::humanBytes(t.anon)));
      }
    }

    QHash<ProcessKey, const ProcessInfo *> byKey;
    QHash<int, const ProcessInfo *> byPid;
    for (const ProcessInfo &p : s->processes) {
      byKey.insert(p.key, &p);
      byPid.insert(p.key.pid, &p);
    }
    // Where each process belongs: its parent's row, or a group.
    auto windowedAncestor = [&](const ProcessInfo &p) -> const ProcessInfo * {
      const ProcessInfo *cur = &p;
      for (int depth = 0; cur && depth < 64; ++depth) {
        if (cur->hasWindow) return cur;
        cur = byPid.value(cur->ppid);
      }
      return nullptr;
    };
    auto desiredParent = [&](const ProcessInfo &p) -> ProcessKey {
      // kTopLevel = top level (or the Background group), kAppsGroup = the Apps group.
      if (treeMode_) {
        const ProcessInfo *pp = byPid.value(p.ppid);
        return pp && pp != &p ? pp->key : kTopLevel;
      }
      // By user: a flat list per user, sorted as the page sorts (the tree view has the parents).
      if (byUser_) return kUserGroup;
      const ProcessInfo *app = windowedAncestor(p);
      if (!app) return kTopLevel;
      if (app == &p) return kAppsGroup;
      const ProcessInfo *pp = byPid.value(p.ppid);
      return pp ? pp->key : kAppsGroup;
    };

    // Drop rows of processes that are gone (children first so takeRow stays valid).
    QList<ProcessKey> dead;
    for (auto it = items_.cbegin(); it != items_.cend(); ++it)
      if (!byKey.contains(it.key())) dead << it.key();
    for (const ProcessKey &k : dead) Remove(k);

    // Insert parents before children: sort by start time.
    QVector<const ProcessInfo *> order;
    for (const ProcessInfo &p : s->processes) order << &p;
    std::sort(order.begin(), order.end(), [](auto *a, auto *b) { return a->key.start < b->key.start; });
    for (const ProcessInfo *p : order) {
      const ProcessKey k = p->key, want = desiredParent(*p);
      QStandardItem *fallback = treeMode_ ? model_->invisibleRootItem() : byUser_ ? users_.value(p->uid) : background_;
      QStandardItem *parentItem = want == kTopLevel ? fallback
                                  : want == kAppsGroup ? apps_
                                  : want == kUserGroup ? users_.value(p->uid)
                                              : items_.value(want, nullptr);
      if (!parentItem) parentItem = fallback;
      QStandardItem *item = items_.value(k, nullptr);
      // Never move a row under its own subtree (a reused PID can look like that).
      for (QStandardItem *a = parentItem; item && a; a = a->parent())
        if (a == item) parentItem = fallback;
      if (item && ParentOf(item) != parentItem) {
        QList<QStandardItem *> row = ParentOf(item)->takeRow(item->row());
        parentItem->appendRow(row);
      } else if (!item) {
        QList<QStandardItem *> row;
        for (int c = 0; c < cCount; ++c) {
          auto *it = new QStandardItem;
          it->setEditable(false);
          if (c >= cPid && c != cStatus && c != cUser && c != cCmd && c != cWindow)
            it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
          row << it;
        }
        row[cName]->setData(k.pid, kKeyRole);
        row[cName]->setData(p->key.start, kStartRole);
        parentItem->appendRow(row);
        items_.insert(k, row[cName]);
        item = row[cName];
      }
      Fill(item, *p);
    }
    if (byUser_ && !expandedOnce_ && users_.contains(getuid())) {
      view_->expand(proxy_->mapFromSource(users_.value(getuid())->index()));
      expandedOnce_ = true;
    }
    if (apps_) {
      apps_->setText(tr("Apps (%1)").arg(apps_->rowCount()));
      background_->setText(tr("Background processes (%1)").arg(background_->rowCount()));
      if (!expandedOnce_) {
        view_->expand(proxy_->mapFromSource(apps_->index()));
        view_->expand(proxy_->mapFromSource(background_->index()));
        expandedOnce_ = true;
      }
    }
    TrySelectPending();
    ShowDetail();
  }

protected:
  bool eventFilter(QObject *o, QEvent *e) override {
    if (o == view_ && e->type() == QEvent::KeyPress) {
      auto *k = static_cast<QKeyEvent *>(e);
      if (k->key() == Qt::Key_Delete) {
        SignalSelected(k->modifiers() & Qt::ShiftModifier ? SIGKILL : SIGTERM);
        return true;
      }
      if (k->key() == Qt::Key_Menu) {
        ContextMenu(view_->visualRect(view_->currentIndex()).center());
        return true;
      }
      if (k->key() == Qt::Key_Escape && pickTimer_ && pickTimer_->isActive()) {
        StopPick(tr("Window picking cancelled"));
        return true;
      }
      // Single letters, vim-like: x ends, X kills, f focuses the window, Enter shows properties.
      if (!(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        if (k->text() == "x") { SignalSelected(SIGTERM); return true; }
        if (k->text() == "X") { SignalSelected(SIGKILL); return true; }
        if (k->text() == "f") { FocusSelected(); return true; }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
          if (auto p = Selected(); p.size() == 1) ShowProperties(this, p.first());
          return true;
        }
      }
    }
    return Page::eventFilter(o, e);
  }

private:
  void TrySelectPending() {
    if (pendingPid_ <= 0) return;
    for (auto it = items_.cbegin(); it != items_.cend(); ++it) {
      if (it.key().pid != pendingPid_) continue;
      if (filter_ && !filter_->text().isEmpty()) filter_->clear();  // the filter could hide it
      const QModelIndex idx = proxy_->mapFromSource(it.value()->index());
      if (!idx.isValid()) break;
      for (QModelIndex up = idx.parent(); up.isValid(); up = up.parent()) view_->expand(up);
      view_->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
      view_->scrollTo(idx, QAbstractItemView::PositionAtCenter);
      view_->setFocus();
      pendingPid_ = 0;
      return;
    }
    if (--pendingTries_ <= 0) {
      emit Status(tr("Process %1 is not running").arg(pendingPid_));
      pendingPid_ = 0;
    }
  }

  // Window picking: Regent has the focus when it starts; the first other
  // window to take it (a click, or Super+arrow from the keyboard) is the answer.
  void StartPick() {
    if (!hypr::Available()) return;
    pickSelf_ = hypr::ActiveWindow().address;
    if (!pickTimer_) {
      pickTimer_ = new QTimer(this);
      pickTimer_->setInterval(150);
      connect(pickTimer_, &QTimer::timeout, this, [this] {
        const hypr::Window w = hypr::ActiveWindow();
        if (pickClock_.elapsed() > 20000) return StopPick(tr("No window picked"));
        if (w.pid <= 0 || w.address == pickSelf_) return;
        StopPick(tr("Picked %1 · %2").arg(w.cls, w.title.left(60)));
        SelectProcess(w.pid);
        if (QWidget *top = window()) {
          top->raise();
          top->activateWindow();
        }
      });
    }
    pickClock_.start();
    pickTimer_->start();
    emit Status(tr("Focus the window to identify: click it or switch to it with the keyboard · Esc cancels"));
  }

  void StopPick(const QString &message) {
    pickTimer_->stop();
    emit Status(message);
  }

  void FocusSelected() {
    QVector<ProcessInfo> sel = Selected();
    if (sel.size() != 1) return;
    // The process itself, or the nearest ancestor that owns a window (a tab's renderer → the browser).
    const ProcessInfo *w = &sel.first();
    QHash<int, const ProcessInfo *> byPid;
    for (const ProcessInfo &p : snap_->processes) byPid.insert(p.key.pid, &p);
    for (int depth = 0; w && w->windowAddress.isEmpty() && depth < 64; ++depth) w = byPid.value(w->ppid);
    if (!w || w->windowAddress.isEmpty()) {
      emit Status(tr("%1 has no window").arg(sel.first().name));
      return;
    }
    if (!hypr::Focus(w->windowAddress)) emit Status(tr("The window of %1 is gone").arg(w->name));
  }

  QStandardItem *Group(const QString &title) {
    QList<QStandardItem *> row;
    for (int c = 0; c < cCount; ++c) {
      auto *it = new QStandardItem(c == 0 ? title : QString());
      it->setEditable(false);
      if (c == 0) {
        QFont f = it->font();
        f.setBold(true);
        it->setFont(f);
      }
      row << it;
    }
    model_->appendRow(row);
    return row[0];
  }

  QStandardItem *ParentOf(QStandardItem *it) const {
    return it->parent() ? it->parent() : model_->invisibleRootItem();
  }

  void Remove(const ProcessKey &k) {
    QStandardItem *it = items_.take(k);
    if (!it) return;
    // Re-home surviving children one level up before the row goes.
    QStandardItem *parent = ParentOf(it);
    while (it->rowCount() > 0) parent->appendRow(it->takeRow(0));
    parent->removeRow(it->row());
  }

  static void Set(QStandardItem *it, const QString &text, const QVariant &sort) {
    if (it->text() != text) it->setText(text);
    if (it->data(kSortRole) != sort) {
      it->setData(sort, kSortRole);
      if (sort.typeId() != QMetaType::QString) it->setData(sort.toDouble(), kValueRole);
    }
  }

  void Fill(QStandardItem *nameItem, const ProcessInfo &p) {
    QStandardItem *parent = ParentOf(nameItem);
    int r = nameItem->row();
    auto cell = [&](int c) { return parent->child(r, c); };
    Set(cell(cName), p.name, p.name.toLower());
    Set(cell(cPid), QString::number(p.key.pid), p.key.pid);
    Set(cell(cStatus), StateName(p.state), StateName(p.state));
    Set(cell(cUser), p.user, p.user);
    Set(cell(cCpu), QString::number(p.cpu, 'f', 1) + "%", p.cpu);
    Set(cell(cMem), fs::humanBytes(p.rss), double(p.rss));
    // "—" where /proc/<pid>/io belongs to another user; the tooltip says why.
    Set(cell(cRead), p.readBps < 0 ? QStringLiteral("—") : fs::humanRate(p.readBps), p.readBps);
    Set(cell(cWrite), p.writeBps < 0 ? QStringLiteral("—") : fs::humanRate(p.writeBps), p.writeBps);
    if (p.readBps < 0 && cell(cRead)->toolTip().isEmpty()) {
      cell(cRead)->setToolTip(tr("Access denied: another user's I/O counters need root"));
      cell(cWrite)->setToolTip(cell(cRead)->toolTip());
    }
    // Empty for processes that hold no GPU context, so the few that do stand out.
    Set(cell(cGpu), p.gpu < 0 ? QString() : QString::number(p.gpu, 'f', 0) + "%", p.gpu);
    if (p.gpu >= 0) cell(cGpu)->setToolTip(tr("GPU memory %1").arg(fs::humanBytes(p.gpuMemory)));
    Set(cell(cWake), p.wakeups < 0 ? QStringLiteral("—") : QString::number(p.wakeups, 'f', 0), p.wakeups);
    Set(cell(cWorkspace), p.workspace, p.workspace);
    Set(cell(cWindow), p.windowTitle, p.windowTitle.toLower());
    if (cell(cWindow)->toolTip() != safe::Tip(p.windowClass)) cell(cWindow)->setToolTip(safe::Tip(p.windowClass));
    Set(cell(cThreads), QString::number(p.threads), p.threads);
    Set(cell(cVirt), fs::humanBytes(p.vsize), double(p.vsize));
    Set(cell(cFds), p.fds < 0 ? QStringLiteral("—") : QString::number(p.fds), p.fds);
    Set(cell(cNice), QString::number(p.nice), p.nice);
    Set(cell(cCmd), p.cmdline, p.cmdline);
    // Heat on the CPU and memory cells, like Task Manager.
    auto heat = [](double frac) {
      frac = std::clamp(frac, 0.0, 1.0);
      QColor c = theme::Good();
      c.setAlpha(int(30 + 150 * frac));
      return frac < 0.005 ? QBrush() : QBrush(c);
    };
    cell(cCpu)->setBackground(heat(p.cpu / 50.0));
    cell(cMem)->setBackground(heat(snap_ && snap_->memTotal ? double(p.rss) / snap_->memTotal * 8 : 0));
    QColor fg = p.state == 'Z' ? theme::Bad() : theme::Text();
    if (cell(cName)->foreground().color() != fg) cell(cName)->setForeground(fg);
  }

  QVector<ProcessInfo> Selected() const {
    QVector<ProcessInfo> out;
    if (!snap_) return out;
    for (const QModelIndex &i : view_->selectionModel()->selectedRows(cName)) {
      QVariant pid = i.data(kKeyRole);
      if (!pid.isValid()) continue;
      const ProcessKey k{pid.toInt(), i.data(kStartRole).toULongLong()};
      for (const ProcessInfo &p : snap_->processes)
        if (p.key == k) out << p;
    }
    return out;
  }

  void ShowDetail() {
    QVector<ProcessInfo> sel = Selected();
    detail_->setVisible(!sel.isEmpty());
    if (sel.size() != 1) {
      detail_->setText(tr("%1 processes selected").arg(sel.size()));
      return;
    }
    const ProcessInfo &p = sel.first();
    // One substitution pass: a process may be named "%2".
    detail_->setText(tr("<b>%1</b> · PID %2 · parent %3 · %4 · %5 threads · nice %6<br><span style='color:%8'>%7</span>")
                         .arg(p.name.toHtmlEscaped(), QString::number(p.key.pid), QString::number(p.ppid),
                              p.user.toHtmlEscaped(), QString::number(p.threads), QString::number(p.nice),
                              p.cmdline.left(400).toHtmlEscaped(), theme::Dim().name()));
  }

  void Report(const QString &what, const QVector<actions::Result> &results) {
    int ok = 0;
    QString err;
    for (const auto &r : results) {
      if (r.ok) ++ok;
      else err = r.message;
      if (r.ok && !r.message.isEmpty()) err = r.message;
    }
    if (ok == results.size() && err.isEmpty()) emit Status(tr("%1 succeeded").arg(what));
    else if (ok > 0) emit Status(tr("%1: %2").arg(what, err));
    else safe::warning(this, what, tr("%1 failed: %2").arg(what, err));
  }

  void SignalSelected(int sig) {
    QVector<ProcessInfo> sel = Selected();
    if (sel.isEmpty()) return;
    QString what = sig == SIGKILL ? tr("Force terminate") : sig == SIGTERM ? tr("End task")
                   : sig == SIGSTOP ? tr("Suspend") : tr("Resume");
    QStringList critical;
    for (const auto &p : sel)
      if (actions::CriticalProcess(p.key.pid)) critical << QString("%1 (%2)").arg(p.name).arg(p.key.pid);
    if (!critical.isEmpty()) {
      // A suspended compositor or Regent cannot be resumed from the screen it froze.
      if (sig == SIGSTOP) {
        safe::information(this, what, tr("%1 keeps the desktop or Regent running and cannot be suspended.")
                                                 .arg(critical.join(", ")));
        return;
      }
      if (sig != SIGCONT &&
          safe::warning(this, what,
                               tr("%1 keeps the desktop session running. Ending it can close every window or lock "
                                  "you out of the screen. Continue?").arg(critical.join(", ")),
                               QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    }
    if (sig == SIGKILL) {
      QStringList names;
      for (const auto &p : sel) names << QString("%1 (%2)").arg(p.name).arg(p.key.pid);
      if (safe::question(this, what, tr("Kill %1 immediately? Unsaved data will be lost.").arg(names.join(", "))) != QMessageBox::Yes)
        return;
    }
    QVector<actions::Result> rs;
    for (const auto &p : sel) rs << actions::Signal(p.key, sig);
    Report(what, rs);
  }

  void ContextMenu(const QPoint &pos) {
    QVector<ProcessInfo> sel = Selected();
    if (sel.isEmpty()) return;
    QMenu m(this);
    m.addAction(tr("&End task"), QKeySequence(Qt::Key_Delete), this, [this] { SignalSelected(SIGTERM); });
    m.addAction(tr("Force &terminate (SIGKILL)"), QKeySequence(Qt::SHIFT | Qt::Key_Delete), this, [this] { SignalSelected(SIGKILL); });
    m.addAction(tr("&Suspend (SIGSTOP)"), this, [this] { SignalSelected(SIGSTOP); });
    m.addAction(tr("&Resume (SIGCONT)"), this, [this] { SignalSelected(SIGCONT); });
    QMenu *prio = m.addMenu(tr("Set &priority"));
    for (auto [label, nice] : {std::pair{tr("&Normal (0)"), 0}, std::pair{tr("&Low priority (10)"), 10},
                               std::pair{tr("L&owest priority (19)"), 19}}) {
      int n = nice;
      prio->addAction(label, this, [this, n] { SetNice(n); });
    }
    prio->addAction(tr("&Custom nice…"), this, [this] {
      bool ok = false;
      int n = QInputDialog::getInt(this, tr("Set priority"),
                                   tr("Nice value (-20 highest … 19 lowest). Lowering nice needs CAP_SYS_NICE."),
                                   Selected().value(0).nice, -20, 19, 1, &ok);
      if (ok) SetNice(n);
    });
    m.addAction(tr("Set &affinity…"), this, [this] { SetAffinity(); });
    QMenu *io = m.addMenu(tr("&I/O priority"));
    for (auto [label, cls, level] : {std::tuple{tr("&Idle — only when the disk is free"), 3, 0},
                                     std::tuple{tr("&Low (best effort 7)"), 2, 7},
                                     std::tuple{tr("&Normal (best effort 4)"), 2, 4},
                                     std::tuple{tr("&High (best effort 0, needs rights)"), 2, 0}}) {
      actions::IoPriority prio{cls, level};
      QAction *a = io->addAction(label, this, [this, prio] {
        QVector<actions::Result> rs;
        for (const auto &p : Selected()) rs << actions::SetIoPriority(p.key, prio);
        Report(tr("Set I/O priority"), rs);
      });
      const actions::IoPriority now = actions::GetIoPriority(sel.first().key);
      a->setCheckable(true);
      a->setChecked(now.ioClass == cls && (cls == 3 || now.level == level));
    }
    m.addAction(tr("&OOM kill priority…"), this, [this] { SetOom(); });
    if (sel.size() == 1) {
      bool user = false;
      const QString unit = actions::SystemdUnit(sel.first().key.pid, &user);
      if (!actions::CriticalUnit(unit)) {
        const QString name = cgroups::DisplayName(unit);
        m.addAction(tr("F&reeze app (%1)").arg(name), this, [this, unit, user] { FreezeUnit(unit, user, true); });
        m.addAction(tr("T&haw app (%1)").arg(name), this, [this, unit, user] { FreezeUnit(unit, user, false); });
      }
    }
    m.addSeparator();
    if (sel.size() == 1) {
      ProcessInfo p = sel.first();
      if (!p.windowAddress.isEmpty()) {
        m.addAction(tr("&Focus window"), QKeySequence(Qt::Key_F), this, [this] { FocusSelected(); });
        QMenu *ws = m.addMenu(tr("&Move window to workspace"));
        const QString address = p.windowAddress;
        for (int n = 1; n <= 10; ++n)
          ws->addAction(QString("&%1").arg(n % 10), this, [this, address, n] {
            if (!hypr::MoveToWorkspace(address, QString::number(n), false)) emit Status(tr("Could not move the window"));
            else if (QWidget *top = window()) top->activateWindow();
          });
        ws->addAction(tr("&Scratchpad"), this, [this, address] {
          if (!hypr::MoveToWorkspace(address, "special:scratchpad", false)) emit Status(tr("Could not move the window"));
        });
        m.addAction(tr("C&lose window"), this, [this, address, p] {
          if (!hypr::Close(address)) emit Status(tr("The window of %1 is gone").arg(p.name));
        });
        m.addSeparator();
      }
      m.addAction(tr("Open file &location"), this, [p] {
        QString exe = QFileInfo(QString("/proc/%1/exe").arg(p.key.pid)).symLinkTarget();
        if (!exe.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(exe).absolutePath()));
      });
      m.addAction(tr("&Copy command line"), this, [p] { QApplication::clipboard()->setText(p.cmdline); });
      m.addAction(tr("P&roperties"), this, [this, p] { ShowProperties(this, p); });
    }
    m.exec(view_->viewport()->mapToGlobal(pos));
  }

  void SetNice(int n) {
    QVector<actions::Result> rs;
    for (const auto &p : Selected()) rs << actions::SetNice(p.key, n);
    Report(tr("Set priority"), rs);
  }

  void SetOom() {
    QVector<ProcessInfo> sel = Selected();
    if (sel.isEmpty()) return;
    int current = 0;
    actions::OomScoreAdj(sel.first().key, &current);
    bool ok = false;
    int v = QInputDialog::getInt(this, tr("OOM kill priority"),
                                 tr("When memory runs out the kernel kills the process with the highest score first.\n"
                                    "-1000 never kill · 0 default · 1000 kill first. Lowering needs administrator rights."),
                                 current, -1000, 1000, 100, &ok);
    if (!ok) return;
    QVector<actions::Result> rs;
    for (const auto &p : sel) rs << actions::SetOomScoreAdj(p.key, v);
    Report(tr("Set OOM priority"), rs);
  }

  void FreezeUnit(const QString &unit, bool user, bool freeze) {
    const QString verb = freeze ? tr("Freeze") : tr("Thaw");
    if (freeze && safe::question(this, verb,
                                        tr("Freeze every process of %1? It keeps its memory but stops running "
                                           "until you thaw it.").arg(unit)) != QMessageBox::Yes)
      return;
    auto *w = new QDBusPendingCallWatcher(
        services::Freeze(user ? services::Bus::User : services::Bus::System, unit, freeze), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, verb, unit] {
      w->deleteLater();
      if (w->isError()) safe::warning(this, verb, tr("%1 %2 failed: %3").arg(verb, unit, w->error().message()));
      else emit Status(tr("%1 %2 succeeded").arg(verb, unit));
    });
  }

  void SetAffinity() {
    QVector<ProcessInfo> sel = Selected();
    if (sel.isEmpty() || !snap_) return;
    QDialog d(this);
    d.setWindowTitle(tr("Processor affinity"));
    auto *v = new QVBoxLayout(&d);
    v->addWidget(new QLabel(tr("Which processors may %1 run on?").arg(sel.size() == 1 ? sel.first().name : tr("these processes"))));
    QVector<int> current = actions::Affinity(sel.first().key);
    auto *grid = new QGridLayout;
    QVector<QCheckBox *> boxes;
    const QVector<int> online = actions::OnlineCpus();  // offline CPUs leave gaps in the numbering
    for (int i = 0; i < online.size(); ++i) {
      auto *b = new QCheckBox(QString("CPU %1").arg(online[i]));
      b->setChecked(current.contains(online[i]));
      b->setProperty("cpu", online[i]);
      grid->addWidget(b, i / 4, i % 4);
      boxes << b;
    }
    auto *all = new QCheckBox(tr("All processors"));
    all->setChecked(std::all_of(online.begin(), online.end(), [&](int c) { return current.contains(c); }));
    connect(all, &QCheckBox::toggled, &d, [boxes](bool on) {
      for (auto *b : boxes) b->setChecked(on);
    });
    v->addWidget(all);
    v->addLayout(grid);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    if (d.exec() != QDialog::Accepted) return;
    QVector<int> cpus;
    for (QCheckBox *b : boxes)
      if (b->isChecked()) cpus << b->property("cpu").toInt();
    QVector<actions::Result> rs;
    for (const auto &p : sel) rs << actions::SetAffinity(p.key, cpus);
    Report(tr("Set affinity"), rs);
  }

  QStandardItemModel *model_;
  Filter *proxy_;
  QTreeView *view_;
  QLineEdit *filter_ = nullptr;
  QLabel *detail_;
  QHash<ProcessKey, QStandardItem *> items_;
  QMenu *menu_ = nullptr;
  int pendingPid_ = 0, pendingTries_ = 0;
  QTimer *pickTimer_ = nullptr;
  QElapsedTimer pickClock_;
  QString pickSelf_;
  QStandardItem *apps_ = nullptr, *background_ = nullptr;
  bool treeMode_ = false;
  bool byUser_ = false;
  QString grouping_;
  QComboBox *groupingBox_ = nullptr;
  QMap<uid_t, QStandardItem *> users_;  // byUser_: the section of each user
  bool expandedOnce_ = false;
  SnapshotPtr snap_;
};

}  // namespace

Page *CreateProcessesPage(QWidget *parent) { return new ProcessesPage(parent); }

}  // namespace td::ui
