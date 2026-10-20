#include "ui/Safe.h"
#include "ui/pages/Pages.h"
#include "ui/Theme.h"

#include "core/Actions.h"
#include "core/Fs.h"
#include "core/Startup.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace td::ui {

namespace {

void RunTaskDialog(QWidget *parent) {
  bool ok = false;
  QString cmd = QInputDialog::getText(parent, QObject::tr("Run new task"),
                                      QObject::tr("Type the name of a program, with any arguments:"),
                                      QLineEdit::Normal, QString(), &ok);
  if (!ok || cmd.trimmed().isEmpty()) return;
  actions::Result r = actions::RunTask(cmd);
  if (!r.ok) safe::warning(parent, QObject::tr("Run new task"), r.message);
}

// ------------------------------------------------------------------ Startup

class StartupPage : public Page {
public:
  explicit StartupPage(QWidget *parent) : Page(parent) {
    tree_ = new QTreeWidget;
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->setHeaderLabels({tr("Enabled"), tr("Name"), tr("Command"), tr("Location")});
    tree_->header()->resizeSection(0, 80);
    tree_->header()->resizeSection(1, 280);
    tree_->header()->resizeSection(2, 420);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(1, Qt::AscendingOrder);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this] { Toggle(); });
    info_ = new QLabel(tr("Hyprland starts Omarchy's apps and yours (~/.config/hypr/autostart.lua); "
                          "XDG autostart adds desktop entries. Changes apply at the next login."));
    info_->setObjectName("Dim");
    info_->setWordWrap(true);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 8);
    l->addWidget(tree_, 1);
    l->addWidget(info_);
  }

  QString Id() const override { return "startup-apps"; }
  QString Title() const override { return tr("Startup apps"); }
  void FocusFilter() override { filter_->setFocus(); }

  void BuildHeader(QHBoxLayout *h) override {
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter startup apps"));
    filter_->setClearButtonEnabled(true);
    connect(filter_, &QLineEdit::textChanged, this, [this] { Reload(); });
    h->addWidget(filter_);
    auto *toggle = new QPushButton(tr("Enable / &Disable"));
    toggle->setProperty("compactText", tr("&Toggle"));
    connect(toggle, &QPushButton::clicked, this, [this] { Toggle(); });
    h->addWidget(toggle);
    auto *run = new QPushButton(tr("Run new task"));
    run->setProperty("compactText", tr("Run…"));
    connect(run, &QPushButton::clicked, this, [this] { RunTaskDialog(this); });
    h->addWidget(run);
  }

  void Update(const SnapshotPtr &) override {
    if (!loaded_) Reload();
  }

private:
  void Reload() {
    loaded_ = true;
    entries_ = startup::List();
    tree_->clear();
    QString f = filter_ ? filter_->text().trimmed() : QString();
    // Two sections: what Hyprland starts (Omarchy's way), then XDG desktop entries.
    QTreeWidgetItem *groups[2] = {nullptr, nullptr};
    int counts[2] = {0, 0};
    for (int i = 0; i < entries_.size(); ++i) {
      const auto &e = entries_[i];
      if (!f.isEmpty() && !e.name.contains(f, Qt::CaseInsensitive) && !e.exec.contains(f, Qt::CaseInsensitive)) continue;
      const bool hypr = e.source == startup::Entry::Source::Hyprland;
      QTreeWidgetItem *&group = groups[hypr ? 0 : 1];
      if (!group) {
        group = new QTreeWidgetItem(tree_);
        group->setFirstColumnSpanned(true);
        group->setExpanded(true);
      }
      ++counts[hypr ? 0 : 1];
      const QString where = hypr ? (e.user ? tr("Yours · line %1").arg(e.line + 1) : tr("Omarchy"))
                                 : (e.user ? tr("User") : tr("System"));
      auto *it = new QTreeWidgetItem(group, {e.enabled ? tr("✔ Enabled") : tr("Disabled"), e.name, e.exec, where});
      it->setData(0, Qt::UserRole, i);
      it->setToolTip(1, safe::Tip(e.comment));
      it->setToolTip(3, safe::Tip(e.path));
      it->setForeground(0, e.enabled ? theme::Good() : theme::Dim());
    }
    // A section is a title only, so the card view shows it as a heading.
    if (groups[0]) groups[0]->setText(1, tr("Hyprland (%1)").arg(counts[0]));
    if (groups[1]) groups[1]->setText(1, tr("XDG autostart (%1)").arg(counts[1]));
  }

  void Toggle() {
    QTreeWidgetItem *it = tree_->currentItem();
    if (!it || !it->data(0, Qt::UserRole).isValid()) return;  // a section heading
    const startup::Entry e = entries_[it->data(0, Qt::UserRole).toInt()];
    QString verb = e.enabled ? tr("Disable") : tr("Enable");
    const bool hypr = e.source == startup::Entry::Source::Hyprland;
    if (hypr && !e.user) {
      safe::warning(this, verb, tr("“%1” is started by Omarchy itself (%2). Regent changes only your own "
                                   "~/.config/hypr/autostart.lua.").arg(e.name, e.path));
      return;
    }
    const QString what = hypr ? tr("This %1 line %2 of ~/.config/hypr/autostart.lua.")
                                    .arg(e.enabled ? tr("comments out") : tr("restores")).arg(e.line + 1)
                              : tr("This writes %1.").arg("~/.config/autostart/" + e.id);
    if (safe::question(this, verb, tr("%1 “%2” at login?\n%3").arg(verb, e.name, what)) != QMessageBox::Yes)
      return;
    QString err;
    if (!startup::SetEnabled(e, !e.enabled, &err)) safe::warning(this, verb, err);
    else emit Status(tr("%1 will %2 at the next login").arg(e.name, e.enabled ? tr("not start") : tr("start")));
    Reload();
  }

  QTreeWidget *tree_;
  QLabel *info_;
  QLineEdit *filter_ = nullptr;
  QVector<startup::Entry> entries_;
  bool loaded_ = false;
};

}  // namespace

Page *CreateStartupPage(QWidget *parent) { return new StartupPage(parent); }

}  // namespace td::ui
