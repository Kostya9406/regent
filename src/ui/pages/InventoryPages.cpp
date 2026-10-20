#include "ui/Safe.h"
#include "ui/pages/Pages.h"
#include "ui/Roles.h"
#include "ui/Theme.h"

#include "core/Actions.h"
#include "core/Fs.h"
#include "core/Inventory.h"
#include "ui/Async.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <functional>


namespace td::ui {

namespace {

// ---------------------------------------------------------- Installed apps

class InstalledAppsPage : public Page {
public:
  explicit InstalledAppsPage(QWidget *parent) : Page(parent) {
    tree_ = new QTreeWidget;
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    // Every fact about a package is a column: the cards show them all, so no detail pane is needed.
    tree_->setHeaderLabels({tr("Application"), tr("Package"), tr("Version"), tr("Installed"), tr("Size"), tr("Reason"),
                            tr("Description"), tr("Website"), tr("Packager"), tr("Command"), tr("Desktop file")});
    tree_->header()->resizeSection(0, 260);
    tree_->header()->resizeSection(1, 200);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::AscendingOrder);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
      const inventory::App *a = Current();
      if (!a) return;
      QMenu m(this);
      if (!a->exec.isEmpty()) {
        QString exec = a->exec;
        exec.remove(QRegularExpression(" ?%[fFuUdDnNickvm]"));
        exec.replace("%%", "%");  // the desktop spec's escaped percent
        m.addAction(tr("&Launch"), this, [this, exec] {
          auto r = actions::RunTask(exec);
          if (!r.ok) safe::warning(this, tr("Launch"), r.message);
        });
      }
      // Package metadata (AUR included) is not ours: open web pages only, never file: or custom schemes.
      if (const QUrl web(a->url); web.isValid() && (web.scheme() == "https" || web.scheme() == "http")) {
        QString url = a->url;
        m.addAction(tr("Open &website"), this, [url] { QDesktopServices::openUrl(QUrl(url)); });
      }
      m.exec(tree_->viewport()->mapToGlobal(pos));
    });
    count_ = new QLabel;
    count_->setObjectName("Dim");
    count_->setWordWrap(true);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 8);
    l->addWidget(tree_, 1);
    l->addWidget(count_);
  }

  QString Id() const override { return "installed-apps"; }
  QString Title() const override { return tr("Installed Apps"); }
  void FocusFilter() override { filter_->setFocus(); }

  void BuildHeader(QHBoxLayout *h) override {
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter applications"));
    filter_->setClearButtonEnabled(true);
    connect(filter_, &QLineEdit::textChanged, this, [this] { Show(); });
    h->addWidget(filter_);
    all_ = new QCheckBox(tr("Show packages without desktop entries"));
    all_->setProperty("compactText", tr("All packages"));
    connect(all_, &QCheckBox::toggled, this, [this] { Show(); });
    h->addWidget(all_);
  }

  void Update(const SnapshotPtr &) override {
    if (requested_) return;
    requested_ = true;
    count_->setText(tr("Scanning for installed applications…"));
    RunAsync<QVector<inventory::App>, InstalledAppsPage>(this, [] { return inventory::InstalledApps(); },
                                       [](InstalledAppsPage *p, const QVector<inventory::App> &apps) {
                                         p->apps_ = apps;
                                         p->Show();
                                       });
  }

private:
  const inventory::App *Current() const {
    QTreeWidgetItem *it = tree_->currentItem();
    return it ? &apps_[it->data(0, Qt::UserRole).toInt()] : nullptr;
  }

  void Show() {
    QString f = filter_ ? filter_->text().trimmed() : QString();
    bool all = all_ && all_->isChecked();
    tree_->setSortingEnabled(false);
    tree_->clear();
    int withDesktop = 0, shown = 0;
    for (int i = 0; i < apps_.size(); ++i) {
      const auto &a = apps_[i];
      bool desktop = !a.desktopNames.isEmpty();
      withDesktop += desktop;
      if (!desktop && !all) continue;
      QString name = desktop ? a.desktopNames.join(", ") : a.package;
      if (!f.isEmpty() && !name.contains(f, Qt::CaseInsensitive) && !a.package.contains(f, Qt::CaseInsensitive) &&
          !a.description.contains(f, Qt::CaseInsensitive))
        continue;
      QString exec = a.exec;
      exec.remove(QRegularExpression(" ?%[fFuUdDnNickvm]"));
      auto *it = new QTreeWidgetItem(tree_, {name, a.package, a.version, a.installed.toString("yyyy-MM-dd hh:mm"),
                                             fs::humanBytes(a.size), a.reason, a.description, a.url, a.packager, exec,
                                             a.desktopFile});
      it->setData(0, Qt::UserRole, i);
      it->setData(4, Qt::UserRole, double(a.size));
      it->setData(4, kValueRole, double(a.size));
      ++shown;
    }
    tree_->setSortingEnabled(true);
    count_->setText(tr("%1 applications with desktop entries · %2 packages · %3 shown · from the pacman local database")
                        .arg(withDesktop).arg(apps_.size()).arg(shown));
  }

  QTreeWidget *tree_;
  QLabel *count_;
  QLineEdit *filter_ = nullptr;
  QCheckBox *all_ = nullptr;
  QVector<inventory::App> apps_;
  bool requested_ = false;
};

// ----------------------------------------------------------------- Drivers

class DriversPage : public Page {
public:
  explicit DriversPage(QWidget *parent) : Page(parent) {
    summary_ = new QLabel;
    summary_->setFont(theme::Mono(13));
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::RichText);

    // Devices: what hardware is there and which driver serves it.
    devices_ = new QTreeWidget;
    devices_->setAlternatingRowColors(true);
    devices_->setUniformRowHeights(true);
    devices_->setHeaderLabels({tr("Device"), tr("Class"), tr("Driver"), tr("Module"), tr("Vendor"), tr("Address"),
                               tr("IDs"), tr("Interface drivers"), tr("Modalias")});
    devices_->header()->resizeSection(0, 360);
    devices_->header()->resizeSection(1, 200);
    devices_->header()->resizeSection(2, 140);
    devices_->header()->resizeSection(3, 120);
    devices_->header()->resizeSection(4, 200);

    modules_ = new QTreeWidget;
    modules_->setRootIsDecorated(false);
    modules_->setAlternatingRowColors(true);
    modules_->setUniformRowHeights(true);
    modules_->setHeaderLabels({tr("Module"), tr("State"), tr("Size"), tr("Used by"), tr("Version"), tr("Source"),
                               tr("Devices"), tr("Depends on"), tr("References"), tr("Source version"), tr("Taint flags"),
                               tr("File")});
    modules_->header()->resizeSection(0, 240);
    modules_->setSortingEnabled(true);
    modules_->sortByColumn(0, Qt::AscendingOrder);

    views_ = new QStackedWidget;
    views_->addWidget(devices_);
    views_->addWidget(modules_);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(8, 8, 0, 8);
    l->addWidget(summary_);
    l->addWidget(views_, 1);
    // From a device, Enter jumps to its module in the module list.
    connect(devices_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
      QString mod = it->text(3).isEmpty() ? it->text(2) : it->text(3);
      if (mod.isEmpty() || mod == "—") return;
      which_->setCurrentIndex(2);
      filter_->setText(mod);
    });
  }

  QString Id() const override { return "drivers"; }
  QString Title() const override { return tr("Drivers"); }
  void FocusFilter() override { filter_->setFocus(); }

  void BuildHeader(QHBoxLayout *h) override {
    which_ = new QComboBox;
    which_->addItems({tr("Devices"), tr("Devices without a driver"), tr("Loaded modules"), tr("All modules"),
                      tr("Not loaded"), tr("Built-in"), tr("Out-of-tree")});
    connect(which_, &QComboBox::currentIndexChanged, this, [this] { Show(); });
    h->addWidget(which_);
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter drivers and devices"));
    filter_->setClearButtonEnabled(true);
    connect(filter_, &QLineEdit::textChanged, this, [this] { Show(); });
    h->addWidget(filter_);
  }

  void Update(const SnapshotPtr &s) override {
    // Modules and devices change rarely; re-read every 30 samples.
    if (loading_ || (loaded_ && s->generation - loadedAt_ < 30)) return;
    loading_ = true;
    loadedAt_ = s->generation;
    struct Both {
      inventory::Modules mods;
      QVector<inventory::Device> devs;
    };
    RunAsync<Both, DriversPage>(this, [] { return Both{inventory::KernelModules(), inventory::Devices()}; },
                    [](DriversPage *p, const Both &b) {
                      p->loading_ = false;
                      p->loaded_ = true;
                      p->mods_ = b.mods;
                      p->devs_ = b.devs;
                      p->Show();
                    });
  }

private:
  bool DeviceMode() const { return !which_ || which_->currentIndex() <= 1; }

  void Show() {
    int unbound = 0;
    for (const auto &d : devs_) unbound += d.driver.isEmpty() && d.interfaceDrivers.isEmpty();
    summary_->setText(tr("Linux %1 · <b>%2</b> devices (<b>%3</b> without a driver) · <b>%4</b> modules · "
                         "<b>%5</b> loaded · <b>%6</b> out-of-tree · kernel memory <b>%7</b> · Enter on a device lists its module")
                          .arg(mods_.kernel).arg(devs_.size()).arg(unbound).arg(mods_.list.size())
                          .arg(mods_.loaded).arg(mods_.outOfTree).arg(fs::humanBytes(mods_.loadedBytes)));
    views_->setCurrentIndex(DeviceMode() ? 0 : 1);
    if (DeviceMode()) ShowDevices();
    else ShowModules();
  }

  bool Matches(const QStringList &fields) const {
    QString f = filter_ ? filter_->text().trimmed() : QString();
    if (f.isEmpty()) return true;
    for (const QString &x : fields)
      if (x.contains(f, Qt::CaseInsensitive)) return true;
    return false;
  }

  void ShowDevices() {
    QString cur = devices_->currentItem() ? devices_->currentItem()->text(5) : QString();
    bool onlyUnbound = which_ && which_->currentIndex() == 1;
    QSet<QString> collapsed;
    for (int i = 0; i < devices_->topLevelItemCount(); ++i)
      if (!devices_->topLevelItem(i)->isExpanded()) collapsed << devices_->topLevelItem(i)->data(0, Qt::UserRole).toString();
    devices_->setUpdatesEnabled(false);
    devices_->clear();
    QHash<QString, QTreeWidgetItem *> groups;
    for (int i = 0; i < devs_.size(); ++i) {
      const auto &d = devs_[i];
      QString driver = !d.driver.isEmpty() && d.driver != "usb" ? d.driver : d.interfaceDrivers.join(", ");
      bool bound = !driver.isEmpty();
      if (onlyUnbound && bound) continue;
      if (!Matches({d.name, d.vendor, d.kind, driver, d.module, d.address, d.ids})) continue;
      QTreeWidgetItem *g = groups.value(d.bus);
      if (!g) {
        g = new QTreeWidgetItem(devices_, {d.bus == "PCI" ? tr("PCI devices") : tr("USB devices")});
        g->setData(0, Qt::UserRole, d.bus);
        QFont f = g->font(0);
        f.setBold(true);
        g->setFont(0, f);
        g->setExpanded(!collapsed.contains(d.bus));
        groups.insert(d.bus, g);
      }
      auto *it = new QTreeWidgetItem(g, {d.name, d.kind, bound ? driver : tr("no driver"),
                                         d.module.isEmpty() ? (bound ? tr("built-in") : QString()) : d.module,
                                         d.vendor, d.address, d.ids, d.interfaceDrivers.join(", "), d.modalias});
      it->setData(0, Qt::UserRole + 1, i);
      it->setToolTip(0, safe::Tip(d.name));
      if (!bound) it->setForeground(2, theme::Warn());
      if (d.address == cur) devices_->setCurrentItem(it);
    }
    for (QTreeWidgetItem *g : groups) g->setText(0, g->text(0) + QString(" (%1)").arg(g->childCount()));
    devices_->setUpdatesEnabled(true);
  }

  void ShowModules() {
    QString cur = modules_->currentItem() ? modules_->currentItem()->text(0) : QString();
    int which = which_->currentIndex() - 2;  // 0 loaded, 1 all, 2 not loaded, 3 built-in, 4 out-of-tree
    modules_->setUpdatesEnabled(false);
    modules_->setSortingEnabled(false);
    modules_->clear();
    for (int i = 0; i < mods_.list.size(); ++i) {
      const auto &m = mods_.list[i];
      if ((which == 0 && !m.loaded) || (which == 2 && (m.loaded || m.builtin)) || (which == 3 && !m.builtin) ||
          (which == 4 && !m.outOfTree))
        continue;
      if (!Matches({m.name, m.path})) continue;
      QString state = m.builtin ? tr("Built-in") : m.loaded ? tr("Loaded") : tr("Not loaded");
      QString source = m.outOfTree ? tr("Out-of-tree") : m.path.startsWith("kernel/") ? tr("In-tree") : m.path.section('/', 0, 0);
      QStringList devicesUsing;
      for (const auto &d : devs_)
        if (d.module == m.name || d.driver == m.name || d.interfaceDrivers.contains(m.name)) devicesUsing << d.name;
      auto *it = new QTreeWidgetItem(modules_, {m.name, state, m.loaded ? fs::humanBytes(m.size) : QString(),
                                                m.usedBy.join(", "), m.version, source, devicesUsing.join("; "),
                                                m.depends.join(", "), m.loaded ? QString::number(m.refs) : QString(),
                                                m.srcversion, m.taint,
                                                m.path.isEmpty() ? QString() : "/lib/modules/" + mods_.kernel + '/' + m.path});
      it->setData(0, Qt::UserRole, i);
      if (m.outOfTree) it->setForeground(5, theme::Warn());
      if (m.name == cur) modules_->setCurrentItem(it);
    }
    modules_->setSortingEnabled(true);
    modules_->setUpdatesEnabled(true);
  }

  QLabel *summary_;
  QTreeWidget *devices_, *modules_;
  QStackedWidget *views_;
  QComboBox *which_ = nullptr;
  QLineEdit *filter_ = nullptr;
  inventory::Modules mods_;
  QVector<inventory::Device> devs_;
  bool loading_ = false, loaded_ = false;
  quint64 loadedAt_ = 0;
};

}  // namespace

Page *CreateInstalledAppsPage(QWidget *parent) { return new InstalledAppsPage(parent); }
Page *CreateDriversPage(QWidget *parent) { return new DriversPage(parent); }

}  // namespace td::ui
