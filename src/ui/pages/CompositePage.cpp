#include "ui/pages/Pages.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/ResponsiveTables.h"

namespace td::ui {

namespace {

class CompositePage : public Page {
public:
  CompositePage(const QString &id, const QString &title, const QVector<Part> &parts, QWidget *parent)
      : Page(parent), id_(id), title_(title), parts_(parts) {
    stack_ = new QStackedWidget;
    // The switch is a strip of its own above the part: in the header it took
    // the room the part's own controls need.
    auto *strip = new QWidget;
    strip->setObjectName("PartStrip");
    auto *sl = new QHBoxLayout(strip);
    sl->setContentsMargins(8, 6, 8, 6);
    sl->setSpacing(4);
    auto *group = new QButtonGroup(this);
    group->setExclusive(true);
    for (int i = 0; i < parts.size(); ++i) {
      auto *b = new QToolButton;
      b->setObjectName("PartSwitch");
      b->setText(parts[i].label);
      b->setCheckable(true);
      b->setChecked(i == 0);
      b->setToolTip(tr("%1 (Alt+the underlined letter)").arg(QString(parts[i].label).remove('&')));
      group->addButton(b, i);
      sl->addWidget(b);
      buttons_ << b;
    }
    sl->addStretch(1);
    connect(group, &QButtonGroup::idClicked, this, [this](int i) { Switch(i); });
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    l->addWidget(strip);
    l->addWidget(stack_, 1);
    for (const Part &part : parts_) {
      // Each part keeps its own id for its lists' card styles and remembered views.
      part.page->setProperty("pageId", part.page->Id());
      InstallResponsiveTables(part.page);
      stack_->addWidget(part.page);
      connect(part.page, &Page::Status, this, &Page::Status);
    }
  }

  QString Id() const override { return id_; }
  QString Title() const override { return title_; }
  QStringList PartIds() const override {
    QStringList ids;
    for (const Part &part : parts_) ids << part.page->Id();
    return ids;
  }
  Page *CurrentPart() override { return parts_[current_].page; }

  bool ShowPart(const QString &id) override {
    for (int i = 0; i < parts_.size(); ++i)
      if (parts_[i].page->Id() == id) {
        Switch(i);
        return true;
      }
    return false;
  }

  void Update(const SnapshotPtr &s) override {
    latest_ = s;
    // The part on screen, and parts that keep history (graphs) even when hidden.
    for (int i = 0; i < parts_.size(); ++i)
      if ((i == current_ && isVisible()) || parts_[i].page->WantsBackgroundUpdates()) parts_[i].page->Update(s);
  }

  bool WantsBackgroundUpdates() const override {
    for (const Part &part : parts_)
      if (part.page->WantsBackgroundUpdates()) return true;
    return false;
  }

  void BuildHeader(QHBoxLayout *h) override {
    // The shown part's own controls.
    headers_ = new QStackedWidget;
    for (const Part &part : parts_) {
      auto *w = new QWidget;
      auto *pl = new QHBoxLayout(w);
      pl->setContentsMargins(0, 0, 0, 0);
      part.page->BuildHeader(pl);
      headers_->addWidget(w);
    }
    ShowHeader(current_);
    h->addWidget(headers_);
  }

  QMenu *PageMenu() override { return CurrentPart()->PageMenu(); }
  void FocusFilter() override { CurrentPart()->FocusFilter(); }
  bool SelectProcess(int pid) override { return CurrentPart()->SelectProcess(pid); }
  bool CanClose() override {
    for (const Part &part : parts_)
      if (!part.page->CanClose()) return false;
    return true;
  }

private:
  void Switch(int index) {
    if (index < 0 || index >= parts_.size()) return;
    const bool changed = index != current_;
    current_ = index;
    stack_->setCurrentIndex(index);
    ShowHeader(index);
    if (index < buttons_.size()) buttons_[index]->setChecked(true);
    if (latest_) parts_[index].page->Update(latest_);
    if (changed) emit PartChanged();
  }

  // A stack is as wide as its widest page: only the shown part's controls may count.
  void ShowHeader(int index) {
    if (!headers_) return;
    for (int i = 0; i < headers_->count(); ++i)
      headers_->widget(i)->setSizePolicy(i == index ? QSizePolicy::Preferred : QSizePolicy::Ignored,
                                         i == index ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    headers_->setCurrentIndex(index);
    headers_->updateGeometry();
  }

  QString id_, title_;
  QVector<Part> parts_;
  QStackedWidget *stack_;
  QStackedWidget *headers_ = nullptr;
  QVector<QToolButton *> buttons_;
  int current_ = 0;
  SnapshotPtr latest_;
};

}  // namespace

Page *CreateCompositePage(const QString &id, const QString &title, const QVector<Part> &parts, QWidget *parent) {
  return new CompositePage(id, title, parts, parent);
}

}  // namespace td::ui
