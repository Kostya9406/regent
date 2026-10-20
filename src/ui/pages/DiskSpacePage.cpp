#include "ui/Safe.h"
#include "ui/pages/Pages.h"

#include "core/DiskScan.h"
#include "core/Fs.h"
#include "ui/OmarchyPalette.h"
#include "ui/PixelWidgets.h"
#include "ui/Roles.h"
#include "ui/Theme.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace td::ui {

namespace {

using diskscan::Dir;

// File-type colours follow the Omarchy theme, with Regent's as fallback.
QColor CategoryColor(int c) {
  static const char *keys[diskscan::CategoryCount] = {"blue", "green", "magenta", "yellow", "red", "dark_foreground", "cyan"};
  static const QColor fallback[diskscan::CategoryCount] = {
      {0x3d, 0x9b, 0xff}, {0x40, 0xd0, 0x70}, {0xb2, 0x5c, 0xf0}, {0xf0, 0xa0, 0x20},
      {0xe8, 0x4a, 0x4a}, {0x8a, 0x8a, 0x95}, {0x2e, 0xc4, 0xe8}};
  return OmarchyPalette::Get().color(keys[c], fallback[c]);
}

// Squarified treemap of one directory's children; click to open, Backspace to go up.
class Treemap : public QWidget {
public:
  std::function<void(Dir *)> onOpen;
  explicit Treemap(QWidget *parent = nullptr) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(300, 160);  // stacked above three panels on a half-screen window
  }
  void SetDir(Dir *d) {
    dir_ = d;
    Layout();
    update();
  }

protected:
  void resizeEvent(QResizeEvent *) override { Layout(); }
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.fillRect(rect(), theme::Bg());
    if (!dir_) {
      p.setPen(theme::Dim());
      p.drawText(rect(), Qt::AlignCenter, tr("Choose a folder and press Scan (Ctrl+R).\nFolders, file types and the largest files appear here."));
      return;
    }
    for (int i = 0; i < rects_.size(); ++i) {
      QRectF r = rects_[i].adjusted(1, 1, -1, -1);
      static const char *keys[] = {"blue", "green", "magenta", "yellow", "cyan", "orange", "red"};
      QColor c = OmarchyPalette::Get().color(keys[i % 7], QColor::fromHsv((i * 37) % 360, 120, 160));
      if (i == hover_) c = c.lighter(130);
      p.fillRect(r, c.darker(170));
      p.setPen(c);
      p.drawRect(r);
      if (r.width() > 60 && r.height() > 30) {
        p.setPen(theme::Text());
        Dir *d = items_[i];
        p.drawText(r.adjusted(6, 4, -6, -4), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   (d ? d->name : tr("files")) + '\n' + fs::humanBytes(d ? d->bytes : dir_->ownFileBytes));
      }
    }
  }
  void mouseMoveEvent(QMouseEvent *e) override {
    int h = Hit(e->position());
    if (h != hover_) {
      hover_ = h;
      setToolTip(h >= 0 && items_[h] ? safe::Tip(items_[h]->Path() + "\n" + fs::humanBytes(items_[h]->bytes)) : QString());
      update();
    }
  }
  void mousePressEvent(QMouseEvent *e) override {
    int h = Hit(e->position());
    if (e->button() == Qt::LeftButton && h >= 0 && items_[h] && onOpen) onOpen(items_[h]);
    if (e->button() == Qt::BackButton && dir_ && dir_->parent && onOpen) onOpen(dir_->parent);
  }
  void keyPressEvent(QKeyEvent *e) override {
    if (e->key() == Qt::Key_Backspace && dir_ && dir_->parent && onOpen) onOpen(dir_->parent);
    else QWidget::keyPressEvent(e);
  }

private:
  int Hit(QPointF pt) const {
    for (int i = 0; i < rects_.size(); ++i)
      if (rects_[i].contains(pt)) return i;
    return -1;
  }

  void Layout() {
    rects_.clear();
    items_.clear();
    hover_ = -1;
    if (!dir_ || dir_->bytes == 0) return;
    QVector<QPair<double, Dir *>> v;
    for (auto &c : dir_->children)
      if (c->bytes > 0 && v.size() < 150) v << qMakePair(double(c->bytes), c.get());
    if (dir_->ownFileBytes > 0) v << qMakePair(double(dir_->ownFileBytes), static_cast<Dir *>(nullptr));
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first > b.first; });
    double total = 0;
    for (auto &x : v) total += x.first;
    QRectF area = rect();
    double scale = area.width() * area.height() / total;
    // Squarify: grow a row while the worst aspect ratio improves.
    int i = 0;
    while (i < v.size()) {
      double side = std::min(area.width(), area.height());
      int j = i;
      double rowSum = 0, worst = 1e18;
      while (j < v.size()) {
        double s = rowSum + v[j].first * scale;
        double mx = v[i].first * scale, mn = v[j].first * scale;
        double w = std::max(side * side * mx / (s * s), (s * s) / (side * side * mn));
        if (w > worst && j > i) break;
        worst = w;
        rowSum = s;
        ++j;
      }
      bool horizontal = area.width() >= area.height();
      double thick = rowSum / side;
      double off = 0;
      for (int k = i; k < j; ++k) {
        double len = v[k].first * scale / thick;
        QRectF r = horizontal ? QRectF(area.left(), area.top() + off, thick, len)
                              : QRectF(area.left() + off, area.top(), len, thick);
        rects_ << r;
        items_ << v[k].second;
        off += len;
      }
      area = horizontal ? area.adjusted(thick, 0, 0, 0) : area.adjusted(0, thick, 0, 0);
      i = j;
    }
  }

  Dir *dir_ = nullptr;
  QVector<QRectF> rects_;
  QVector<Dir *> items_;
  int hover_ = -1;
};

class DiskSpacePage : public Page {
public:
  explicit DiskSpacePage(QWidget *parent) : Page(parent) {
    dirs_ = new QTreeWidget;
    dirs_->setHeaderLabels({tr("Folder"), tr("Size"), tr("Files"), tr("% of parent")});
    dirs_->header()->resizeSection(0, 260);
    dirs_->setUniformRowHeights(true);
    connect(dirs_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *it) { Populate(it); });
    connect(dirs_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it) {
      if (it) map_->SetDir(static_cast<Dir *>(it->data(0, Qt::UserRole).value<void *>()));
    });
    dirs_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(dirs_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
      QTreeWidgetItem *it = dirs_->currentItem();
      if (!it) return;
      QString path = static_cast<Dir *>(it->data(0, Qt::UserRole).value<void *>())->Path();
      QMenu m(this);
      m.addAction(tr("&Open folder"), this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
      m.addAction(tr("&Scan this folder"), this, [this, path] { path_->setText(path); StartScan(); });
      m.exec(dirs_->viewport()->mapToGlobal(pos));
    });

    map_ = new Treemap;
    map_->setObjectName("DiskTreemap");
    map_->setProperty("primaryGraphic",true);
    map_->onOpen = [this](Dir *d) { Select(d); };

    types_ = new QTreeWidget;
    types_->setRootIsDecorated(false);
    types_->setHeaderLabels({tr("File type"), tr("Size"), tr("Files")});
    types_->setMaximumHeight(220);
    largest_ = new QTreeWidget;
    largest_->setRootIsDecorated(false);
    largest_->setHeaderLabels({tr("Largest files"), tr("Size")});
    largest_->header()->resizeSection(0, 300);
    connect(largest_, &QTreeWidget::itemActivated, this, [](QTreeWidgetItem *it) {
      QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(it->toolTip(0)).absolutePath()));
    });
    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(types_);
    rv->addWidget(largest_, 1);

    auto *split = split_ = new QSplitter;
    split->addWidget(dirs_);
    split->addWidget(map_);
    split->addWidget(right);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    split->setStretchFactor(2, 2);

    progress_ = new QProgressBar;
    progress_->setRange(0, 0);
    progress_->setFixedHeight(8);
    progress_->setTextVisible(false);
    progress_->hide();
    status_ = new QLabel(tr("Scans stay on one filesystem and count hard links once. Nothing is changed on disk."));
    status_->setObjectName("Dim");
    status_->setWordWrap(true);
    volumes_ = new PixelVolumes;
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 8);
    l->addWidget(volumes_);
    l->addWidget(split, 1);
    l->addWidget(progress_);
    l->addWidget(status_);

    menu_ = new QMenu(tr("&Disk Space"), this);
    menu_->addAction(tr("&Start Scan"), QKeySequence(Qt::CTRL | Qt::Key_R), this, [this] { StartScan(); });
    menu_->addAction(tr("&Pause / Resume Scan"), this, [this] { TogglePause(); });
    menu_->addAction(tr("Sto&p Scan"), this, [this] { scanner_.RequestStop(); });
    // Before the first scan only the map's prompt shows; three empty lists only took room.
    // Deferred: the lists are wrapped in their card panels after the page is built.
    QTimer::singleShot(0, this, [this] { ShowResults(false); });
  }

  void ShowResults(bool on) {
    for (int i = 0; i < split_->count(); ++i)
      if (split_->widget(i) != map_) split_->widget(i)->setVisible(on);
    if (!on) return;
    // Hidden panes have no size; a layout switch meanwhile may have kept them at 0.
    QList<int> sizes = split_->sizes();
    if (!sizes.contains(0)) return;
    const int total = split_->orientation() == Qt::Horizontal ? split_->width() : split_->height();
    for (int &size : sizes) size = total / int(sizes.size());
    split_->setSizes(sizes);
  }

  ~DiskSpacePage() override { scanner_.Stop(); }

  QString Id() const override { return "disk-space"; }
  QString Title() const override { return tr("Disk Space"); }
  QMenu *PageMenu() override { return menu_; }
  void FocusFilter() override { path_->setFocus(); path_->selectAll(); }

  void BuildHeader(QHBoxLayout *h) override {
    path_ = new QLineEdit(QDir::homePath());
    path_->setMinimumWidth(140);
    connect(path_, &QLineEdit::returnPressed, this, [this] { StartScan(); });
    h->addWidget(path_);
    auto *browse = browse_ = new QPushButton(tr("Browse…"));
    browse->setToolTip(tr("Choose a folder to scan"));
    connect(browse, &QPushButton::clicked, this, [this] {
      QString d = QFileDialog::getExistingDirectory(this, tr("Choose a folder to scan"), path_->text());
      if (!d.isEmpty()) path_->setText(d);
    });
    h->addWidget(browse);
    start_ = new QPushButton(tr("&Start Scan"));
    connect(start_, &QPushButton::clicked, this, [this] { scanner_.Running() ? TogglePause() : StartScan(); });
    h->addWidget(start_);
    stop_ = new QPushButton(tr("Sto&p"));
    stop_->setToolTip(tr("Stop the scan"));
    stop_->setEnabled(false);
    connect(stop_, &QPushButton::clicked, this, [this] { scanner_.RequestStop(); });
    h->addWidget(stop_);
    SetStartLabel(StartLabel::Start);
  }

  void Update(const SnapshotPtr &) override {}

protected:
  // A half-screen window leaves the header about 330 px: the path keeps the
  // room and the buttons drop to short labels (full names stay as tooltips).
  void resizeEvent(QResizeEvent *e) override {
    Page::resizeEvent(e);
    const bool compact = width() < 760;
    if (!browse_ || compact == compactHeader_) return;
    compactHeader_ = compact;
    browse_->setText(compact ? QStringLiteral("…") : tr("Browse…"));
    path_->setMinimumWidth(compact ? 90 : 140);
    SetStartLabel(startLabel_);
  }

private:
  enum class StartLabel { Start, Pause, Resume };
  void SetStartLabel(StartLabel s) {
    startLabel_ = s;
    static const char *const full[] = {QT_TR_NOOP("&Start Scan"), QT_TR_NOOP("&Pause Scan"), QT_TR_NOOP("Res&ume Scan")};
    static const char *const brief[] = {QT_TR_NOOP("&Scan"), QT_TR_NOOP("&Pause"), QT_TR_NOOP("Res&ume")};
    const int i = int(s);
    start_->setText(tr(compactHeader_ ? brief[i] : full[i]));
    start_->setToolTip(tr(full[i]).remove('&') + (s == StartLabel::Start ? QStringLiteral(" (Ctrl+R)") : QString()));
  }

  void TogglePause() {
    if (!scanner_.Running()) return;
    scanner_.SetPaused(!scanner_.Paused());
    SetStartLabel(scanner_.Paused() ? StartLabel::Resume : StartLabel::Pause);
  }

  void StartScan() {
    QString root = QDir::cleanPath(path_->text().trimmed());
    if (!QFileInfo(root).isDir()) {
      safe::warning(this, tr("Disk Space"), tr("%1 is not a folder.").arg(root));
      return;
    }
    // The tree widgets point into the old result; drop them before it goes.
    dirs_->clear();
    map_->SetDir(nullptr);
    result_.reset();
    ShowResults(true);
    progress_->show();
    SetStartLabel(StartLabel::Pause);
    stop_->setEnabled(true);
    QPointer<DiskSpacePage> self(this);
    // Each scan has a generation; a late result of a replaced scan is dropped.
    const int gen = ++scanGen_;
    scanner_.Start(
        root,
        [self, gen](const diskscan::Progress &p) {
          QMetaObject::invokeMethod(qApp, [self, p, gen] {
            if (self && self->scanGen_ == gen && self->scanner_.Running())
              self->status_->setText(tr("Scanning %1 · %2 files · %3")
                                         .arg(p.current).arg(p.files).arg(fs::humanBytes(p.bytes)));
          }, Qt::QueuedConnection);
        },
        [self, gen](std::shared_ptr<diskscan::Result> r) {
          QMetaObject::invokeMethod(qApp, [self, r, gen] {
            if (self && self->scanGen_ == gen) self->Show(r);
          }, Qt::QueuedConnection);
        });
  }

  void Show(std::shared_ptr<diskscan::Result> r) {
    result_ = r;
    progress_->hide();
    SetStartLabel(StartLabel::Start);
    stop_->setEnabled(false);
    status_->setText(tr("%1%2 · %3 in %4 files, %5 folders · %6 unreadable · %7 s")
                         .arg(r->root->name, r->stopped ? tr(" (stopped early)") : QString())
                         .arg(fs::humanBytes(r->root->bytes)).arg(r->files).arg(r->dirs).arg(r->unreadable)
                         .arg(r->seconds, 0, 'f', 1));
    dirs_->clear();
    auto *top = Item(nullptr, r->root.get());
    dirs_->addTopLevelItem(top);
    top->setExpanded(true);
    dirs_->setCurrentItem(top);

    types_->clear();
    for (int c = 0; c < diskscan::CategoryCount; ++c) {
      if (!r->categoryFiles[c]) continue;
      auto *it = new QTreeWidgetItem(types_, {diskscan::CategoryName(c), fs::humanBytes(r->categoryBytes[c]),
                                              QString::number(r->categoryFiles[c])});
      it->setForeground(0, CategoryColor(c));
      it->setData(1, Qt::UserRole, double(r->categoryBytes[c]));
      it->setData(1, kValueRole, double(r->categoryBytes[c]));
    }
    types_->sortItems(1, Qt::DescendingOrder);
    // Paint the scanned folder's share of its filesystem by file type.
    QVector<PixelVolumes::Slice> slices;
    for (int c = 0; c < diskscan::CategoryCount; ++c)
      if (r->categoryBytes[c]) slices << PixelVolumes::Slice{double(r->categoryBytes[c]), CategoryColor(c)};
    std::sort(slices.begin(), slices.end(), [](auto &a, auto &b) { return a.bytes > b.bytes; });
    volumes_->Refresh();
    volumes_->SetBreakdown(r->root->name, slices);
    largest_->clear();
    for (const auto &f : r->largest) {
      auto *it = new QTreeWidgetItem(largest_, {QFileInfo(f.path).fileName(), fs::humanBytes(f.bytes)});
      it->setData(1, kValueRole, double(f.bytes));
      it->setToolTip(0, safe::Tip(f.path));
    }
  }

  QTreeWidgetItem *Item(QTreeWidgetItem *parent, Dir *d) {
    auto *it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem;
    it->setText(0, d->name);
    it->setText(1, fs::humanBytes(d->bytes));
    it->setData(1, kValueRole, double(d->bytes));
    it->setText(2, QString::number(d->files));
    double pct = d->parent && d->parent->bytes ? 100.0 * d->bytes / d->parent->bytes : 100;
    it->setText(3, QString::number(pct, 'f', 1) + "%");
    it->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<void *>(d)));
    if (!d->children.empty()) new QTreeWidgetItem(it, {QStringLiteral("…")});  // lazy placeholder
    return it;
  }

  void Populate(QTreeWidgetItem *it) {
    if (it->childCount() != 1 || it->child(0)->data(0, Qt::UserRole).isValid()) return;
    delete it->takeChild(0);
    Dir *d = static_cast<Dir *>(it->data(0, Qt::UserRole).value<void *>());
    int n = 0;
    for (auto &c : d->children) {
      if (++n > 500) break;  // the treemap and the rest still account for every byte
      Item(it, c.get());
    }
  }

  void Select(Dir *d) {
    // Walk the tree widget down to d, expanding as we go.
    QVector<Dir *> chain;
    for (Dir *x = d; x; x = x->parent) chain.prepend(x);
    QTreeWidgetItem *it = dirs_->topLevelItem(0);
    for (int i = 1; it && i < chain.size(); ++i) {
      it->setExpanded(true);
      QTreeWidgetItem *next = nullptr;
      for (int k = 0; k < it->childCount(); ++k)
        if (it->child(k)->data(0, Qt::UserRole).value<void *>() == chain[i]) next = it->child(k);
      it = next;
    }
    if (it) {
      dirs_->setCurrentItem(it);
      dirs_->scrollToItem(it);
    }
    map_->SetDir(d);
    map_->setFocus();
  }

  diskscan::Scanner scanner_;
  int scanGen_ = 0;
  std::shared_ptr<diskscan::Result> result_;
  QTreeWidget *dirs_, *types_, *largest_;
  QSplitter *split_;
  Treemap *map_;
  PixelVolumes *volumes_;
  QProgressBar *progress_;
  QLabel *status_;
  QLineEdit *path_ = nullptr;
  QPushButton *browse_ = nullptr;
  bool compactHeader_ = false;
  StartLabel startLabel_ = StartLabel::Start;
  QPushButton *start_ = nullptr, *stop_ = nullptr;
  QMenu *menu_;
};

}  // namespace

Page *CreateDiskSpacePage(QWidget *parent) { return new DiskSpacePage(parent); }

}  // namespace td::ui
