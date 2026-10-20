#include "ui/ResponsiveTables.h"

#include <QEvent>
#include <QHeaderView>
#include <QLayout>
#include <QPointer>
#include <QSplitter>
#include <QStackedLayout>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include "ui/TableAtlas.h"
#include "ui/Theme.h"
namespace td::ui {
namespace {
class SplitLayout : public QObject {
 public:
  SplitLayout(QSplitter* split, QWidget* page)
      : QObject(page), split_(split), page_(page), orientation_(split->orientation()) {
    for (int i = 0; i < split_->count(); ++i) order_ << split_->widget(i);
    page_->installEventFilter(this);
    page_->window()->installEventFilter(this);
    split_->installEventFilter(this);
    // Shares hold until the user drags a handle; after that their sizes win.
    connect(split_, &QSplitter::splitterMoved, this, [this] {
      if (!applying_) shares_.clear();
    });
    QTimer::singleShot(0, this, [this] { Adapt(); });
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* e) override {
    if (watched == split_) {
      if (e->type() == QEvent::Resize && !shares_.isEmpty()) ApplyShares();
      return false;
    }
    if (e->type() == QEvent::Show) page_->window()->installEventFilter(this);
    if (e->type() == QEvent::Resize || e->type() == QEvent::Show || e->type() == QEvent::WindowStateChange)
      QTimer::singleShot(0, this, [this] { Adapt(); });
    return false;
  }

 private:
  void Adapt() {
    bool compact = !page_->window()->isFullScreen() &&
                   page_->window()->width() < (compact_ ? 1040 : 980);
    if (compact == compact_ || orientation_ != Qt::Horizontal) return;
    compact_ = compact;
    if (compact) {
      sizes_ = split_->sizes();
      // A vertical QSplitter is only as wide as its narrowest child allows: a
      // side list capped for the wide layout (System Info: 260 px) would pin the
      // whole page to that width. Stacked, every pane spans the page.
      maxWidths_.clear();
      for (auto w : order_)
        if (w) {
          maxWidths_ << w->maximumWidth();
          w->setMaximumWidth(QWIDGETSIZE_MAX);
        }
      split_->setOrientation(Qt::Vertical);
      bool primary = false;
      for (auto w : order_)
        if (w && w->property("primaryGraphic").toBool()) {
          split_->insertWidget(0, w);
          primary = true;
          break;
        }
      // Shares of the height, applied once the splitter has its real height:
      // sizes set while it is still unlaid out are lost when it first grows.
      auto table = [](QWidget* w) {
        return w->objectName() == "ResponsiveTable" || w->findChild<QWidget*>("ResponsiveTable");
      };
      shares_.clear();
      if (primary) {
        // The graphic gets two fifths; the panels below share the rest. Fixed
        // 350/220 heights overflowed a half-screen window and clipped panels.
        for (int i = 0; i < split_->count(); ++i)
          shares_ << (i == 0 ? 0.4 : split_->count() > 1 ? 0.6 / (split_->count() - 1) : 0);
      } else if (split_->count() != 2) {
        for (int i = 0; i < split_->count(); ++i) shares_ << 1.0 / split_->count();
      } else if (table(split_->widget(0)) && !table(split_->widget(1))) {
        // A table above its detail pane: the table is what the page is for.
        shares_ << 0.75 << 0.25;
      } else {
        // A navigation list above its content: the list scrolls, the content gets the room.
        shares_ << 0.3 << 0.7;
      }
      ApplyShares();
    } else {
      shares_.clear();
      for (int i = 0; i < order_.size(); ++i)
        if (order_[i]) split_->insertWidget(i, order_[i]);
      int restored = 0;
      for (auto w : order_)
        if (w && restored < maxWidths_.size()) w->setMaximumWidth(maxWidths_[restored++]);
      split_->setOrientation(orientation_);
      split_->setSizes(sizes_);
    }
  }
  void ApplyShares() {
    const int total = split_->height();
    if (total < 120) return;  // not laid out yet: the splitter's first real resize applies them
    QList<int> sizes;
    for (double share : shares_) sizes << int(total * share);
    applying_ = true;
    split_->setSizes(sizes);
    applying_ = false;
  }
  QSplitter* split_;
  QWidget* page_;
  QList<double> shares_;  // stacked layout's height shares, kept through resizes until the user drags
  bool applying_ = false;
  Qt::Orientation orientation_;
  QVector<QPointer<QWidget>> order_;
  QList<int> sizes_;
  QList<int> maxWidths_;  // the wide layout's caps, by order_, while stacked
  bool compact_ = false;
};
// Every table is shown as its graphic overview (TableAtlas): one card per
// row with all of its columns. The table itself stays, hidden, as the model,
// the selection and the target of the page's keys and menus.
class TablePanel : public QWidget {
 public:
  TablePanel(QTreeView* view, QWidget* page) : QWidget(view->parentWidget()), view_(view) {
    setObjectName("ResponsiveTable");
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* body = new QWidget;
    body->setMinimumWidth(0);
    auto* stack = new QStackedLayout(body);
    stack->setContentsMargins(0, 0, 0, 0);
    atlas_ = new TableAtlas(view_, page->property("pageId").toString());
    view_->setParent(body);
    stack->addWidget(view_);
    stack->addWidget(atlas_);
    stack->setCurrentWidget(atlas_);
    setFocusProxy(atlas_);
    layout->addWidget(body, 1);
  }
  QSize minimumSizeHint() const override { return {180, atlas_->minimumHeight()}; }
  QSize sizeHint() const override { return {420, 300}; }

 private:
  QTreeView* view_;
  TableAtlas* atlas_;
};
}  // namespace
void InstallResponsiveTables(QWidget* page) {
  for (auto* view : page->findChildren<QTreeView*>()) {
    if (!view->parentWidget()) continue;
    if (view->parentWidget()->parentWidget() &&
        view->parentWidget()->parentWidget()->objectName() == "ResponsiveTable")
      continue;
    // Navigation categories are not data tables.
    if (view->isHeaderHidden() && view->model()->columnCount() == 1) continue;
    QWidget* parent = view->parentWidget();
    auto* split = qobject_cast<QSplitter*>(parent);
    auto* placeholder = new QWidget;
    QLayoutItem* item = nullptr;
    int index = -1;
    if (split) {
      index = split->indexOf(view);
      split->replaceWidget(index, placeholder);
    } else if (parent->layout()) {
      item = parent->layout()->replaceWidget(view, placeholder);
      if (!item) {
        delete placeholder;
        continue;
      }
    } else {
      delete placeholder;
      continue;
    }
    auto* panel = new TablePanel(view, page);
    if (split)
      split->replaceWidget(index, panel);
    else {
      delete parent->layout()->replaceWidget(placeholder, panel);
      delete item;
    }
    delete placeholder;
  }
  // Performance keeps its original always-visible device list on the left.
  if (page->property("pageId").toString() != "performance")
    for (auto* split : page->findChildren<QSplitter*>()) new SplitLayout(split, page);
}
}  // namespace td::ui
