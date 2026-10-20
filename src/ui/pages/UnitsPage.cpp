#include "ui/Safe.h"
#include "ui/pages/Pages.h"

#include "core/Actions.h"
#include "core/Cgroups.h"
#include "core/Fs.h"
#include "core/Services.h"
#include "ui/Roles.h"
#include "ui/Theme.h"

#include <QDBusPendingCallWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace td::ui {

namespace {

enum Col { cUnit, cCpu, cMem, cCache, cRead, cWrite, cTasks, cState, cCount };

class UnitItem : public QTreeWidgetItem {
public:
  using QTreeWidgetItem::QTreeWidgetItem;
  bool operator<(const QTreeWidgetItem &other) const override {
    const int c = treeWidget()->sortColumn();
    if (c >= cCpu && c <= cTasks) return data(c, Qt::UserRole).toDouble() < other.data(c, Qt::UserRole).toDouble();
    return QTreeWidgetItem::operator<(other);
  }
};

// Resources per systemd slice, service and scope: one row per app, however
// many processes it runs, from the kernel's own cgroup accounting.
class UnitsPage : public Page {
public:
  explicit UnitsPage(QWidget *parent) : Page(parent) {
    tree_ = new QTreeWidget;
    tree_->setColumnCount(cCount);
    tree_->setHeaderLabels({tr("Unit"), tr("CPU"), tr("Memory"), tr("File cache"), tr("Disk read"), tr("Disk write"),
                            tr("Tasks"), tr("State")});
    tree_->headerItem()->setToolTip(cMem, tr("Memory the unit holds, without its file cache"));
    tree_->headerItem()->setToolTip(cCache, tr("Page cache of files the unit read or wrote; the kernel frees it when memory runs short"));
    tree_->header()->resizeSection(cUnit, 320);
    for (int c = cCpu; c < cCount; ++c) tree_->header()->resizeSection(c, 96);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(cMem, Qt::DescendingOrder);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->installEventFilter(this);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
      if (QTreeWidgetItem *at = tree_->itemAt(pos)) {
        tree_->setCurrentItem(at);
        Menu(tree_->viewport()->mapToGlobal(pos));
      }
    });
    note_ = new QLabel(tr("Memory leaves out the file cache. Disk columns need systemd's io controller."));
    note_->setObjectName("Dim");
    note_->setWordWrap(true);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 8);
    l->addWidget(tree_, 1);
    l->addWidget(note_);
  }

  QString Id() const override { return "units"; }
  QString Title() const override { return tr("Units"); }
  void FocusFilter() override { filter_->setFocus(); filter_->selectAll(); }

  void BuildHeader(QHBoxLayout *h) override {
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter units"));
    filter_->setClearButtonEnabled(true);
    filter_->setMinimumWidth(140);
    connect(filter_, &QLineEdit::textChanged, this, [this] { ApplyFilter(); });
    h->addWidget(filter_);
  }

  void Update(const SnapshotPtr &) override {
    const QVector<cgroups::Node> nodes = sampler_.Collect();
    QSet<QString> seen;
    tree_->setSortingEnabled(false);
    for (const cgroups::Node &n : nodes) {
      seen.insert(n.path);
      QTreeWidgetItem *it = items_.value(n.path);
      if (!it) {
        QTreeWidgetItem *parent = items_.value(n.parent);
        it = parent ? new UnitItem(parent) : new UnitItem(tree_);
        it->setText(cUnit, cgroups::DisplayName(n.name));
        it->setToolTip(cUnit, safe::Tip(n.path));
        it->setData(cUnit, Qt::UserRole, n.name);
        it->setData(cUnit, Qt::UserRole + 1, n.userManager);
        it->setData(cUnit, Qt::UserRole + 2, n.foreign);
        for (int c = cCpu; c < cState; ++c) it->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
        if (n.name.endsWith(".slice")) {
          QFont f = it->font(cUnit);
          f.setBold(true);
          it->setFont(cUnit, f);
        }
        items_.insert(n.path, it);
        if (!expanded_ && n.parent.isEmpty()) it->setExpanded(true);
      }
      Set(it, cCpu, QString::number(n.cpu, 'f', 1) + "%", n.cpu);
      Set(it, cMem, fs::humanBytes(n.memory), double(n.memory));
      Set(it, cCache, fs::humanBytes(n.cache), double(n.cache));
      Set(it, cRead, n.readBps < 0 ? QStringLiteral("—") : fs::humanRate(n.readBps), n.readBps);
      Set(it, cWrite, n.writeBps < 0 ? QStringLiteral("—") : fs::humanRate(n.writeBps), n.writeBps);
      Set(it, cTasks, n.tasks < 0 ? QStringLiteral("—") : QString::number(n.tasks), n.tasks);
      const QString state = n.frozen ? tr("Frozen") : tr("Running");
      if (it->text(cState) != state) {
        it->setText(cState, state);
        it->setForeground(cState, n.frozen ? theme::Warn() : theme::Dim());
      }
    }
    expanded_ = true;
    // Children before parents, so a parent never takes its children with it.
    QStringList gone;
    for (auto it = items_.cbegin(); it != items_.cend(); ++it)
      if (!seen.contains(it.key())) gone << it.key();
    std::sort(gone.begin(), gone.end(), [](const QString &a, const QString &b) { return a.count('/') > b.count('/'); });
    for (const QString &k : gone) delete items_.take(k);
    tree_->setSortingEnabled(true);
    ApplyFilter();
  }

protected:
  bool eventFilter(QObject *o, QEvent *e) override {
    if (o == tree_ && e->type() == QEvent::KeyPress) {
      auto *k = static_cast<QKeyEvent *>(e);
      if (k->key() == Qt::Key_Menu && tree_->currentItem()) {
        Menu(tree_->viewport()->mapToGlobal(tree_->visualItemRect(tree_->currentItem()).center()));
        return true;
      }
    }
    return Page::eventFilter(o, e);
  }

private:
  static void Set(QTreeWidgetItem *it, int c, const QString &text, double sort) {
    if (it->text(c) != text) it->setText(c, text);
    if (it->data(c, Qt::UserRole).toDouble() != sort) {
      it->setData(c, Qt::UserRole, sort);
      it->setData(c, kValueRole, sort);
    }
  }

  void ApplyFilter() {
    const QString f = filter_ ? filter_->text().trimmed() : QString();
    // A row stays when it or any descendant matches.
    std::function<bool(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *it) {
      bool any = f.isEmpty() || it->text(cUnit).contains(f, Qt::CaseInsensitive) ||
                 it->data(cUnit, Qt::UserRole).toString().contains(f, Qt::CaseInsensitive);
      for (int i = 0; i < it->childCount(); ++i) any |= visit(it->child(i));
      it->setHidden(!any);
      if (!f.isEmpty() && any) it->setExpanded(true);
      return any;
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) visit(tree_->topLevelItem(i));
  }

  void Menu(const QPoint &global) {
    QTreeWidgetItem *it = tree_->currentItem();
    if (!it) return;
    const QString unit = it->data(cUnit, Qt::UserRole).toString();
    const bool user = it->data(cUnit, Qt::UserRole + 1).toBool();
    const bool frozen = it->text(cState) == tr("Frozen");
    QMenu m(this);
    // Slices hold whole sessions (freezing user.slice would freeze the desktop
    // and Regent with it); Regent's own unit would freeze the hand that thaws it.
    // Another user's units: their names may imitate system ones, so offer nothing.
    const bool critical = actions::CriticalUnit(unit) || it->data(cUnit, Qt::UserRole + 2).toBool();
    if ((unit.endsWith(".service") || unit.endsWith(".scope")) && !critical) {
      m.addAction(frozen ? tr("T&haw") : tr("F&reeze"), this, [this, unit, user, frozen] {
        if (!frozen && safe::question(this, tr("Freeze"),
                                             tr("Freeze %1? Its processes keep their memory but stop running until "
                                                "you thaw it.").arg(unit)) != QMessageBox::Yes)
          return;
        Watch(services::Freeze(user ? services::Bus::User : services::Bus::System, unit, !frozen),
              frozen ? tr("Thaw") : tr("Freeze"), unit);
      });
    }
    if ((unit.endsWith(".service") || unit.endsWith(".scope")) && !critical) {
      m.addAction(tr("S&top"), this, [this, unit, user] {
        if (safe::question(this, tr("Stop"), tr("Stop %1 and every process in it?").arg(unit)) != QMessageBox::Yes)
          return;
        Watch(services::Control(user ? services::Bus::User : services::Bus::System, unit, "StopUnit"), tr("Stop"), unit);
      });
    }
    if (unit.endsWith(".service") && !critical) {
      m.addAction(tr("&Restart"), this, [this, unit, user] {
        if (safe::question(this, tr("Restart"), tr("Restart %1?").arg(unit)) != QMessageBox::Yes) return;
        Watch(services::Control(user ? services::Bus::User : services::Bus::System, unit, "RestartUnit"), tr("Restart"),
              unit);
      });
    }
    if (critical && !unit.endsWith(".slice"))
      m.addAction(tr("Protected: the desktop, the session or Regent itself runs here"))->setEnabled(false);
    if (m.isEmpty()) return;
    m.exec(global);
  }

  void Watch(const QDBusPendingCall &call, const QString &verb, const QString &unit) {
    emit Status(tr("%1 %2…").arg(verb, unit));
    auto *w = new QDBusPendingCallWatcher(call, this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, verb, unit] {
      w->deleteLater();
      if (w->isError()) safe::warning(this, verb, tr("%1 %2 failed: %3").arg(verb, unit, w->error().message()));
      else emit Status(tr("%1 %2 succeeded").arg(verb, unit));
    });
  }

  QTreeWidget *tree_;
  QLabel *note_;
  QLineEdit *filter_ = nullptr;
  cgroups::Sampler sampler_;
  QHash<QString, QTreeWidgetItem *> items_;
  bool expanded_ = false;
};

}  // namespace

Page *CreateUnitsPage(QWidget *parent) { return new UnitsPage(parent); }

}  // namespace td::ui
