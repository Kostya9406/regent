#include "ui/Safe.h"
#include "ui/pages/Pages.h"
#include "ui/Theme.h"

#include "core/Actions.h"
#include "core/Connections.h"
#include "ui/Async.h"
#include "ui/PixelWidgets.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QScrollBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <csignal>

namespace td::ui {

namespace {

using connections::Exposure;
using connections::Socket;

// Sockets of our own processes first: other users' ones carry no process to act on.
class SocketItem : public QTreeWidgetItem {
 public:
  using QTreeWidgetItem::QTreeWidgetItem;
  bool operator<(const QTreeWidgetItem &other) const override {
    const bool mine = !data(0, Qt::UserRole + 1).toBool(), theirs = !other.data(0, Qt::UserRole + 1).toBool();
    if (mine != theirs && treeWidget()->sortColumn() == 0)
      return (treeWidget()->header()->sortIndicatorOrder() == Qt::AscendingOrder) == mine;
    return QTreeWidgetItem::operator<(other);
  }
};

class ConnectionsPage : public Page {
public:
  explicit ConnectionsPage(QWidget *parent) : Page(parent) {
    dash_ = new RallyDash;
    tree_ = new QTreeWidget;
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setHeaderLabels({tr("Process"), tr("PID"), tr("Protocol"), tr("State"), tr("Local endpoint"),
                            tr("Remote endpoint"), tr("Service"), tr("Exposure"), tr("User")});
    tree_->header()->resizeSection(0, 170);
    tree_->header()->resizeSection(1, 70);
    tree_->header()->resizeSection(2, 70);
    tree_->header()->resizeSection(3, 110);
    tree_->header()->resizeSection(4, 230);
    tree_->header()->resizeSection(5, 230);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::AscendingOrder);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &ConnectionsPage::Menu);
    note_ = new QLabel;
    note_->setObjectName("Dim");
    note_->setWordWrap(true);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 8);
    l->addWidget(dash_);
    l->addWidget(tree_, 1);
    l->addWidget(note_);
  }

  QString Id() const override { return "connections"; }
  QString Title() const override { return tr("Connections"); }
  void FocusFilter() override { filter_->setFocus(); }

  void BuildHeader(QHBoxLayout *h) override {
    which_ = new QComboBox;
    which_->addItems({tr("All network"), tr("Listening"), tr("Established"), tr("Internet"), tr("Local network"),
                      tr("Loopback"), tr("TCP"), tr("UDP"), tr("Local IPC (Unix)")});
    connect(which_, &QComboBox::currentIndexChanged, this, [this] { Show(); });
    h->addWidget(which_);
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter by process, address or port"));
    filter_->setClearButtonEnabled(true);
    connect(filter_, &QLineEdit::textChanged, this, [this] { Show(); });
    h->addWidget(filter_);
  }

  void Update(const SnapshotPtr &s) override {
    // The dashboard follows every sample; the socket table every second one.
    RallyDash::Reading r = reading_;
    r.rxBps = r.txBps = 0;
    for (const NetInfo &n : s->nets)
      if (n.physical) {
        r.rxBps += n.rxBps;
        r.txBps += n.txBps;
      }
    reading_ = r;
    dash_->SetReading(reading_);
    if (busy_ || (loaded_ && s->generation - loadedAt_ < 2)) return;
    busy_ = true;
    loadedAt_ = s->generation;
    RunAsync<connections::Table, ConnectionsPage>(
        this, [] { return connections::Collect(true); },
        [](ConnectionsPage *p, const connections::Table &t) {
          p->busy_ = false;
          p->loaded_ = true;
          p->table_ = t;
          p->Count();
          p->Show();
        });
  }

private:
  void Count() {
    RallyDash::Reading r = reading_;
    r.loopback = r.lan = r.internet = r.anyIface = r.ipc = r.established = r.listening = 0;
    for (const Socket &s : table_.sockets) {
      if (s.exposure == Exposure::LocalIpc) {
        ++r.ipc;
        continue;
      }
      r.established += s.state == "ESTABLISHED";
      r.listening += s.listening();
      switch (s.exposure) {
        case Exposure::Loopback: ++r.loopback; break;
        case Exposure::LocalNetwork: ++r.lan; break;
        case Exposure::Internet: ++r.internet; break;
        case Exposure::AllInterfaces: ++r.anyIface; break;
        case Exposure::LocalIpc: break;
      }
    }
    reading_ = r;
    dash_->SetReading(r);
  }

  bool Wanted(const Socket &s) const {
    int w = which_ ? which_->currentIndex() : 0;
    bool isUnix = s.proto == "Unix";
    switch (w) {
      case 0: if (isUnix) return false; break;
      case 1: if (isUnix || !s.listening()) return false; break;
      case 2: if (isUnix || s.state != "ESTABLISHED") return false; break;
      case 3: if (s.exposure != Exposure::Internet) return false; break;
      case 4: if (s.exposure != Exposure::LocalNetwork) return false; break;
      case 5: if (s.exposure != Exposure::Loopback) return false; break;
      case 6: if (!s.proto.startsWith("TCP")) return false; break;
      case 7: if (!s.proto.startsWith("UDP")) return false; break;
      case 8: if (!isUnix) return false; break;
    }
    QString f = filter_ ? filter_->text().trimmed() : QString();
    if (f.isEmpty()) return true;
    for (const QString &x : {s.process, s.localAddr, s.remoteAddr, QString::number(s.localPort), QString::number(s.remotePort),
                             s.service, QString::number(s.pid)})
      if (x.contains(f, Qt::CaseInsensitive)) return true;
    return false;
  }

  static QString Endpoint(const QString &addr, int port, bool isUnix) {
    if (isUnix) return addr.isEmpty() ? QStringLiteral("—") : addr;
    if (addr == "0.0.0.0" || addr == "::") return port ? QString("*:%1").arg(port) : QStringLiteral("*");
    QString a = addr.contains(':') ? '[' + addr + ']' : addr;
    return port ? a + ':' + QString::number(port) : a;
  }

  void Show() {
    QString cur = tree_->currentItem() ? tree_->currentItem()->data(0, Qt::UserRole).toString() : QString();
    const int scroll = tree_->verticalScrollBar()->value();  // a refresh must not move the list
    tree_->setUpdatesEnabled(false);
    tree_->setSortingEnabled(false);
    tree_->clear();
    int shown = 0;
    for (const Socket &s : table_.sockets) {
      if (!Wanted(s)) continue;
      bool isUnix = s.proto == "Unix";
      // Another user's socket has no process we may see; its owner and service say whose it is.
      QString owner = s.user.isEmpty() ? tr("other user") : s.user;
      if (!s.service.isEmpty()) owner += " · " + s.service;
      auto *it = new SocketItem(tree_, {s.process.isEmpty() ? "[" + owner + "]" : s.process,
                                             s.pid ? QString::number(s.pid) : QStringLiteral("—"), s.proto, s.state,
                                             Endpoint(s.localAddr, s.localPort, isUnix),
                                             isUnix || s.listening() ? QStringLiteral("—") : Endpoint(s.remoteAddr, s.remotePort, false),
                                             s.service, connections::ExposureName(s.exposure), s.user});
      it->setData(0, Qt::UserRole, s.Key());
      it->setData(1, Qt::UserRole, s.pid);
      it->setData(2, Qt::UserRole, s.pidStart);
      it->setData(0, Qt::UserRole + 1, s.process.isEmpty());
      if (s.process.isEmpty()) it->setForeground(0, theme::Dim());
      if (s.exposure == Exposure::AllInterfaces) it->setForeground(7, theme::Warn());
      if (s.exposure == Exposure::Internet) it->setForeground(7, theme::Accent());
      if (s.Key() == cur) tree_->setCurrentItem(it);
      ++shown;
    }
    tree_->setSortingEnabled(true);
    tree_->verticalScrollBar()->setValue(scroll);
    tree_->setUpdatesEnabled(true);
    note_->setText(tr("%1 shown of %2 sockets · %3 owned by other users (their process is visible to root only) · "
                      "read from /proc/net, no network traffic is generated")
                       .arg(shown).arg(table_.sockets.size()).arg(table_.unresolved));
  }

  void Menu(const QPoint &pos) {
    QTreeWidgetItem *it = tree_->itemAt(pos);  // the row under the pointer, not a stale selection
    if (!it) return;
    tree_->setCurrentItem(it);
    QMenu m(this);
    QString remote = it->text(5), local = it->text(4);
    m.addAction(tr("Copy &remote endpoint"), this, [remote] { QApplication::clipboard()->setText(remote); });
    m.addAction(tr("Copy &local endpoint"), this, [local] { QApplication::clipboard()->setText(local); });
    int pid = it->data(1, Qt::UserRole).toInt();
    quint64 start = it->data(2, Qt::UserRole).toULongLong();
    if (pid > 0) {
      QString name = it->text(0);
      m.addSeparator();
      m.addAction(tr("&End process %1 (%2)").arg(name).arg(pid), this, [this, pid, start, name] {
        const QString ask = actions::CriticalProcess(pid)
                                ? tr("%1 (PID %2) keeps the desktop session running. Ending it can close every window. "
                                     "Send SIGTERM anyway?").arg(name).arg(pid)
                                : tr("Send SIGTERM to %1 (PID %2)?").arg(name).arg(pid);
        if (safe::question(this, tr("End process"), ask) != QMessageBox::Yes)
          return;
        // The start time was taken with the socket table; Signal() refuses if the PID was reused since.
        actions::Result r = actions::Signal({pid, start}, SIGTERM);
        if (r.ok) emit Status(tr("Sent SIGTERM to %1").arg(name));
        else safe::warning(this, tr("End process"), r.message);
      });
    }
    m.exec(tree_->viewport()->mapToGlobal(pos));
  }

  RallyDash *dash_;
  QTreeWidget *tree_;
  QLabel *note_;
  QComboBox *which_ = nullptr;
  QLineEdit *filter_ = nullptr;
  connections::Table table_;
  RallyDash::Reading reading_;
  bool busy_ = false, loaded_ = false;
  quint64 loadedAt_ = 0;
};

}  // namespace

Page *CreateConnectionsPage(QWidget *parent) { return new ConnectionsPage(parent); }

}  // namespace td::ui
