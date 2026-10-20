#include "ui/Safe.h"
#include "ui/TableAtlas.h"

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QHeaderView>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "core/Settings.h"
#include "ui/OmarchyPalette.h"
#include "ui/Roles.h"
#include "ui/Theme.h"
namespace td::ui {
namespace {
constexpr int kPad = 8, kInset = 36, kGap = 6, kScrollWidth = 10, kMaxValueLines = 6;
constexpr int kRowHeight = 22, kRowGap = 12;  // the rows view
constexpr int kCell = 8;          // rows ↔ cards dissolve: pixel size
constexpr double kSpark = 0.12;   // how long a cell glows before it goes

// Long lists read best as rows; short ones (users, units, sensors) as cards.
bool RowsByDefault(const QString& domain) {
  static const QStringList rows = {"processes", "services", "connections", "installed-apps",
                                   "drivers", "disk-space", "flight-recorder", "startup-apps", "power-and-freq", "users", "system-info"};
  return rows.contains(domain);
}

QString Cell(const QModelIndex& i, int c) { return i.siblingAtColumn(c).data().toString(); }
// "Header value", except for a bare "Value" column, where the header says nothing.
QString Labelled(const QString& header, const QString& cell) {
  return header.compare("Value", Qt::CaseInsensitive) == 0 ? cell : header + " " + cell;
}
// The longest prefix of `text` no wider than `width`: binary search, so long
// command lines cost a few measurements per line instead of one per character.
int Fit(const QFontMetrics& fm, const QString& text, int width) {
  int lo = 1, hi = text.size();
  while (lo < hi) {
    const int mid = (lo + hi + 1) / 2;
    if (fm.horizontalAdvance(text.left(mid)) <= width) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}
// Splits text into lines of `width`, breaking after a space or punctuation
// where possible; only past `lines` lines is the rest elided.
QStringList Wrap(const QFontMetrics& fm, const QString& text, int width, int lines) {
  QStringList out;
  QString rest = text;
  while (!rest.isEmpty() && out.size() < lines - 1 && fm.horizontalAdvance(rest) > width) {
    const int fit = Fit(fm, rest, width);
    int cut = fit;
    for (int i = fit; i > fit / 2; --i)
      if (QStringLiteral(" -_./:·,[(=").contains(rest[i - 1])) {
        cut = i;
        break;
      }
    out << rest.left(cut).trimmed();
    rest = rest.mid(cut).trimmed();
  }
  if (!rest.isEmpty()) out << fm.elidedText(rest, Qt::ElideRight, width);
  return out;
}
double Quantity(QString text) {
  text.remove(',');
  static const QRegularExpression number("[-+]?[0-9]*\\.?[0-9]+");
  auto match = number.match(text);
  if (!match.hasMatch()) return 0;
  double value = match.captured().toDouble();
  for (const auto& [unit, mult] : {std::pair{"TB", 1e12}, {"GB", 1e9}, {"MB", 1e6}, {"kB", 1e3},
                                   {"GiB", 1073741824.0}, {"MiB", 1048576.0}, {"KiB", 1024.0}})
    if (text.contains(unit, Qt::CaseInsensitive)) {
      value *= mult;
      break;
    }
  return std::max(0.0, value);
}
QColor Color(const QString& domain) {
  if (domain == "processes" || domain == "summary" || domain == "units") return theme::Cpu();
  if (domain == "connections") return theme::Net();
  if (domain == "disk-space") return theme::Disk();
  if (domain == "power-and-freq") return theme::Temp();
  if (domain == "installed-apps" || domain == "users") return theme::Memory();
  return theme::Accent();
}
QColor StateColor(const QString& state, QColor fallback) {
  if (state.contains("fail", Qt::CaseInsensitive)) return theme::Bad();
  if (state.contains("frozen", Qt::CaseInsensitive) || state.contains("zombie", Qt::CaseInsensitive)) return theme::Warn();
  if (state.contains("running", Qt::CaseInsensitive) || state.contains("enabled", Qt::CaseInsensitive) ||
      state.contains("ESTABLISHED") || state == "●")
    return theme::Good();
  if (state.contains("inactive", Qt::CaseInsensitive) || state.contains("stopped", Qt::CaseInsensitive) ||
      state.contains("disabled", Qt::CaseInsensitive) || state.contains("dead", Qt::CaseInsensitive) || state == "○")
    return theme::Dim();
  return fallback;
}
void Pixels(QPainter& p, const QRectF& area, double value, QColor color) {
  const int count = qMax(1, int(area.width() / 8)), lit = qBound(0, qRound(value * count), count);
  for (int i = 0; i < count; ++i) p.fillRect(QRectF(area.x() + i * 8, area.y(), 5, 5), i < lit ? color : theme::Raised());
}
void Glyph(QPainter& p, QPoint origin, const QString& domain, QColor color) {
  QStringList shape;
  if (domain == "connections")
    shape = {"11000011", "11000011", "00100100", "00011000", "00011000", "00100100", "11000011", "11000011"};
  else if (domain == "disk-space")
    shape = {"11100000", "11111110", "10000001", "10111101", "10111101", "10000001", "11111111", "00000000"};
  else if (domain == "users")
    shape = {"00111100", "01111110", "01111110", "00111100", "00011000", "01111110", "11111111", "11000011"};
  else if (domain == "installed-apps" || domain == "startup-apps")
    shape = {"00011000", "01111110", "11100111", "11011011", "10011001", "11000011", "01100110", "00011000"};
  else if (domain == "services" || domain == "units")
    shape = {"00011000", "01011010", "00111100", "11100111", "11100111", "00111100", "01011010", "00011000"};
  else if (domain == "power-and-freq")
    shape = {"00011000", "00110000", "01111110", "11111100", "00011000", "00110000", "01100000", "01000000"};
  else
    shape = {"01011010", "01111110", "11011011", "01111110", "11011011", "01111110", "01011010", "00000000"};
  for (int y = 0; y < shape.size(); ++y)
    for (int x = 0; x < shape[y].size(); ++x)
      if (shape[y][x] == '1') p.fillRect(origin.x() + x * 3, origin.y() + y * 3, 2, 2, color);
}
}  // namespace

TableAtlas::TableAtlas(QTreeView* source, QString domain, QWidget* parent)
    : QWidget(parent), source_(source), domain_(std::move(domain)) {
  setObjectName("TableAtlas");
  setAccessibleName(Title());
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setMinimumHeight(110);
  setMinimumWidth(180);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  scroll_ = new QScrollBar(Qt::Vertical, this);
  scroll_->setObjectName("AtlasScroll");
  connect(scroll_, &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
  refresh_.setSingleShot(true);
  refresh_.setInterval(40);
  connect(&refresh_, &QTimer::timeout, this, &TableAtlas::Refresh);
  if (auto* m = source_->model()) {
    connect(m, &QAbstractItemModel::rowsInserted, this, [this] { Schedule(); });
    connect(m, &QAbstractItemModel::rowsRemoved, this, [this] { Schedule(); });
    connect(m, &QAbstractItemModel::rowsMoved, this, [this] { Schedule(); });
    connect(m, &QAbstractItemModel::dataChanged, this, [this] { Schedule(); });
    connect(m, &QAbstractItemModel::modelReset, this, [this] { Schedule(); });
    connect(m, &QAbstractItemModel::layoutChanged, this, [this] { Schedule(); });
  }
  connect(source_, &QTreeView::expanded, this, [this] { Schedule(); });
  connect(source_, &QTreeView::collapsed, this, [this] { Schedule(); });
  connect(source_->selectionModel(), &QItemSelectionModel::selectionChanged, this, qOverload<>(&QWidget::update));
  connect(source_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
    if (int i = Current(); i >= 0) EnsureVisible(i);
    update();
  });
  animation_.setInterval(45);
  connect(&animation_, &QTimer::timeout, this, [this] {
    phase_ += 0.035;
    update();
  });
  connect(&OmarchyPalette::Get(), &OmarchyPalette::Changed, this, [this] {
    grid_ = QPixmap();
    update();
  });
  fade_.setStartValue(0.0);
  fade_.setEndValue(1.0 + kSpark);  // every cell passes its spark and goes out
  fade_.setDuration(340);
  fade_.setEasingCurve(QEasingCurve::InOutQuad);
  connect(&fade_, &QVariantAnimation::valueChanged, this, qOverload<>(&QWidget::update));
  connect(&fade_, &QVariantAnimation::finished, this, [this] {
    from_ = QPixmap();
    dissolve_.clear();
    update();
  });
  const QString view = Settings::Get().value("listView." + domain_).toString();
  dense_ = view.isEmpty() ? RowsByDefault(domain_) : view == "rows";
}

void TableAtlas::SetDense(bool on) {
  if (on == dense_) return;
  // The entry the eye is on (the current one if on screen, else the top one)
  // keeps its place across the switch; the rest grows or shrinks around it.
  int anchor = Current();
  if (anchor >= 0 && !QRect(0, ContentTop(), width(), ViewHeight()).intersects(EntryRect(anchor))) anchor = -1;
  for (int i = 0; anchor < 0 && i < entries_.size(); ++i)
    if (EntryRect(i).bottom() >= ContentTop()) anchor = i;
  const int anchorY = anchor >= 0 ? EntryRect(anchor).top() : 0;
  if (isVisible()) {
    from_ = grab();
    // Each cell of the old view gets its moment to go: mostly noise, a little
    // top to bottom, so it reads as a pixel wipe rather than static.
    const int cols = (width() + kCell - 1) / kCell, rows = (height() + kCell - 1) / kCell;
    dissolve_.resize(cols * rows);
    auto* random = QRandomGenerator::global();
    for (int y = 0; y < rows; ++y)
      for (int x = 0; x < cols; ++x)
        dissolve_[y * cols + x] = float(0.72 * random->generateDouble() + 0.28 * y / std::max(1, rows - 1));
    dissolveCols_ = cols;
    fade_.stop();
    fade_.start();
  }
  dense_ = on;
  Settings::Get().set("listView." + domain_, on ? "rows" : "cards");
  Relayout();
  if (anchor >= 0) scroll_->setValue(ContentTop() + entries_[anchor].y - std::max(ContentTop(), anchorY));
  update();
}

// Too narrow for it beside the title: `v` still switches.
QRect TableAtlas::ViewRect() const { return width() < 380 ? QRect() : QRect(width() - 150, 8, 138, 18); }

QString TableAtlas::RowValue(const Entry& e, const RowColumn& c) const {
  if (c.kind == RowColumn::Metric) return e.metricValue;
  if (c.kind == RowColumn::State) return e.state;
  for (const auto& f : e.fields)
    if (f.slot == c.slot) return f.value;
  return {};
}

void TableAtlas::LayoutRows() {
  rowColumns_.clear();
  const QFontMetrics mf(theme::Mono(10)), hf(theme::Mono(9)), tf(theme::Mono(10, true));
  const int avail = qMax(120, width() - kScrollWidth - 20);
  QVector<RowColumn> candidates;
  if (cols_.metric >= 0) candidates << RowColumn{RowColumn::Metric, cols_.metric, -1, cols_.headers.value(cols_.metric)};
  if (cols_.state >= 0) candidates << RowColumn{RowColumn::State, cols_.state, -1, cols_.headers.value(cols_.state)};
  for (int k = 0; k < cols_.fields.size(); ++k)
    candidates << RowColumn{RowColumn::Field, cols_.fields[k], k, cols_.headers.value(cols_.fields[k])};
  // Nearly every process is "Sleeping": its dot says that, the column comes last.
  if (domain_ == "processes" && cols_.state >= 0 && candidates.size() > 1)
    candidates.move(cols_.metric >= 0 ? 1 : 0, candidates.size() - 1);
  // Each column as wide as its widest value on the first few hundred rows, within
  // reason. Widths only grow until the size or the columns change, so columns
  // do not shift every sample as the widest value comes and goes.
  const QString key = QString::number(width()) + '\x1f' + cols_.headers.join('\x1f');
  if (key != rowLayoutKey_) {
    rowLayoutKey_ = key;
    rowWidths_.clear();
  }
  int titleNatural = rowWidths_.value(-1);
  const int sample = std::min<int>(entries_.size(), 400);
  for (int i = 0; i < sample; ++i)
    if (!entries_[i].group)
      titleNatural = std::max(titleNatural, tf.horizontalAdvance(entries_[i].title) + 30 + std::min(entries_[i].depth, 8) * 14);
  for (auto& c : candidates) {
    int w = hf.horizontalAdvance(c.header.toUpper());
    for (int i = 0; i < sample; ++i)
      if (!entries_[i].group) w = std::max(w, mf.horizontalAdvance(RowValue(entries_[i], c)));
    c.w = std::clamp(std::max(w, rowWidths_.value(c.column)), 28, c.header.contains("command", Qt::CaseInsensitive) ? 420 : 220);
    rowWidths_[c.column] = c.w;
  }
  rowWidths_[-1] = titleNatural;
  // The name keeps at least a third; columns follow in table order while they fit.
  const int titleMin = std::min(titleNatural + 8, std::max(140, avail / 3));
  int x = titleMin;
  for (auto c : candidates) {
    if (x + kRowGap + c.w > avail) continue;
    c.x = x + kRowGap;
    x = c.x + c.w;
    rowColumns_ << c;
  }
  // What is left goes to the name, then to the last column (a command line).
  int spare = avail - x;
  const int toTitle = std::clamp(titleNatural + 8 - titleMin, 0, spare);
  rowTitleWidth_ = titleMin + toTitle;
  for (auto& c : rowColumns_) c.x += toTitle;
  spare -= toTitle;
  if (!rowColumns_.isEmpty()) rowColumns_.last().w += spare;
  else rowTitleWidth_ += spare;
}

QString TableAtlas::Title() const {
  if (domain_ == "processes" || domain_ == "summary") return tr("PROCESS / RESOURCE MAP");
  if (domain_ == "services") return tr("SERVICE / STATE MATRIX");
  if (domain_ == "units") return tr("UNIT / CGROUP MAP");
  if (domain_ == "connections") return tr("CONNECTION / SIGNAL ROUTES");
  if (domain_ == "disk-space") {  // three lists on one page: say which
    const QString what = source_->model() ? source_->model()->headerData(0, Qt::Horizontal).toString() : QString();
    return what.isEmpty() ? tr("STORAGE / BLOCK MAP") : tr("STORAGE / %1").arg(what.toUpper());
  }
  if (domain_ == "power-and-freq") return tr("SENSOR / LIVE METERS");
  if (domain_ == "users") return tr("USER / SESSION MAP");
  if (domain_ == "installed-apps") return tr("APPLICATION / PACKAGE SHELF");
  if (domain_ == "startup-apps") return tr("STARTUP / SWITCH BOARD");
  if (domain_ == "drivers") return tr("HARDWARE / DEVICE BUS");
  if (domain_ == "benchmarks") return tr("RESULT / RUN HISTORY");
  if (domain_ == "flight-recorder") return tr("RECORDED / PROCESS MAP");
  return tr("SYSTEM / DATA CARDS");
}

void TableAtlas::Schedule() {
  if (isVisible() && !refresh_.isActive()) refresh_.start();
}

void TableAtlas::Collect(const QModelIndex& parent, int depth, const QString& path, const Columns& cols, int* visited) {
  auto* model = source_->model();
  for (int r = 0; r < model->rowCount(parent) && *visited < 5000; ++r) {
    if (source_->isRowHidden(r, parent)) continue;
    ++*visited;
    const QModelIndex i = model->index(r, 0, parent);
    Entry e;
    e.index = i;
    e.depth = depth;
    e.children = model->rowCount(i);
    e.title = Cell(i, cols.title);
    if (e.title == QStringLiteral("…")) continue;  // a lazy folder's placeholder child
    for (int c = 0; c < cols.count; ++c) {
      if (c == cols.title || source_->isColumnHidden(c)) continue;
      const QString value = Cell(i, c);
      if (value.isEmpty()) continue;
      if (c == cols.metric) {
        e.metric = Labelled(cols.headers[c], value);
        e.metricValue = value;
        const QString& h = cols.headers[c];
        // The page's own number when it gives one; reading the text back is the fallback.
        // A "Value" column mixes units (cores, GB, GHz): no bar would compare them honestly.
        const QVariant typed = i.siblingAtColumn(c).data(kValueRole);
        e.weight = QStringList{"Version", "State", "Status", "Value"}.contains(h) ? 0
                   : typed.isValid()                                           ? std::max(0.0, typed.toDouble())
                                                                               : Quantity(value);
        e.percent = h.startsWith("CPU");
      } else if (c == cols.state) {
        e.state = value;
      } else {
        e.fields << Field{cols.headers[c], value.left(1200), int(cols.fields.indexOf(c))};
      }
    }
    if (e.title.isEmpty() && e.metric.isEmpty() && e.fields.isEmpty()) continue;
    e.group = e.children > 0 && e.metric.isEmpty() && e.state.isEmpty() && e.fields.isEmpty();
    e.expanded = e.children > 0 && source_->isExpanded(i);
    // Sections (sensor groups, Apps) hold nothing but their children: open
    // them the first time they appear, so every value is on a card.
    const QString key = path + '\x1f' + e.title.section(" (", 0, 0);
    if (e.group && !e.expanded && !autoOpened_.contains(key)) {
      autoOpened_.insert(key);
      source_->setExpanded(i, true);
      e.expanded = true;
    }
    entries_ << e;
    if (e.expanded) Collect(i, depth + 1, key, cols, visited);
  }
}

void TableAtlas::Refresh() {
  auto* model = source_->model();
  if (!model) return;
  Columns cols;
  cols.count = model->columnCount();
  for (int c = 0; c < cols.count; ++c) cols.headers << model->headerData(c, Qt::Horizontal).toString();
  auto find = [&cols](const QStringList& names) {
    for (const auto& name : names)
      for (int c = 0; c < cols.count; ++c)
        if (cols.headers[c].compare(name, Qt::CaseInsensitive) == 0) return c;
    return -1;
  };
  cols.pid = find({"PID"});
  cols.title = qMax(0, find({"Name", "Process", "Application", "Folder", "Device", "Unit", "Sensor", "Property", "Test"}));
  cols.metric = find(domain_ == "benchmarks" ? QStringList{"Score"}
                                             : QStringList{"CPU", "CPU %", "Value", "Size", "Memory", "Memory MiB",
                                                           "Version", "State", "Status"});
  cols.state = find({"State", "Status", "Enabled", "Active", "Running"});
  if (cols.state == cols.metric) cols.metric = -1;  // a State column is the state: it colours the card
  for (int c = 0; c < cols.count; ++c)
    if (c != cols.title && c != cols.metric && c != cols.state && !source_->isColumnHidden(c)) cols.fields << c;
  slots_ = int(cols.fields.size());
  cols_ = cols;
  entries_.clear();
  int visited = 0;
  Collect({}, 0, QString(), cols, &visited);
  total_ = 0;
  maxWeight_ = 0;
  for (const auto& e : entries_) {
    total_ += !e.group;
    if (!e.percent) maxWeight_ = std::max(maxWeight_, e.weight);
  }
  setAccessibleDescription(tr("%1 entries. Arrows or j/k move, Right/Left open and close groups, "
                              "s changes the sort, Menu opens the actions.").arg(total_));
  Relayout();
  UpdateAnimation();
  update();
}

// Rows span the list (depth is an indent inside the row); cards step in.
int TableAtlas::CardX(const Entry& e) const { return dense_ ? 8 : 12 + std::min(e.depth, 8) * 14; }
int TableAtlas::CardWidth(const Entry& e) const { return qMax(120, width() - kScrollWidth - 8 - CardX(e)); }

void TableAtlas::LayoutEntry(Entry& e, int width) const {
  const QFontMetrics tf(theme::Mono(11, true)), mf(theme::Mono(10)), hf(theme::Mono(9));
  e.titleLines.clear();
  e.metricLines.clear();
  e.valueLines.clear();
  e.cells.clear();
  if (e.group) {
    e.h = 28;
    return;
  }
  const int textWidth = qMax(40, width - kInset - kPad);
  QString metric = e.metric;
  if (!e.state.isEmpty() && !metric.contains(e.state)) metric += (metric.isEmpty() ? "" : " / ") + e.state;
  const int metricWidth = metric.isEmpty() ? 0 : mf.horizontalAdvance(metric);
  // The metric sits beside the title when it is short; otherwise on lines of its own.
  e.metricBelow = metricWidth > textWidth / 3;
  const int titleWidth = e.metricBelow || metric.isEmpty() ? textWidth : textWidth - metricWidth - 12;
  e.titleLines = Wrap(tf, e.title, titleWidth, 4);
  int y = kPad + int(e.titleLines.size()) * tf.height();
  if (e.metricBelow) {
    e.metricLines = Wrap(mf, metric, textWidth, 3);
    y += 2 + int(e.metricLines.size()) * mf.height();
  }
  if (e.weight > 0 || e.percent) y += 10;  // the pixel bar
  if (!e.fields.isEmpty()) {
    // Each column keeps its place on every card (a missing value leaves a gap),
    // so the eye finds "Memory" at the same spot all down the list.
    y += 6;
    const int columns = qBound(1, textWidth / 210, qMax(1, slots_));
    const int cellWidth = (textWidth - (columns - 1) * 14) / columns;
    e.valueLines.resize(e.fields.size());
    e.cells.resize(e.fields.size());
    const int rows = (qMax(1, slots_) + columns - 1) / columns;
    bool any = false;
    for (int row = 0; row < rows; ++row) {
      int rowHeight = 0;
      for (int f = 0; f < e.fields.size(); ++f) {
        if (e.fields[f].slot / columns != row) continue;
        const QStringList lines = Wrap(mf, e.fields[f].value, cellWidth, kMaxValueLines);
        const int h = hf.height() + int(lines.size()) * mf.height();
        e.valueLines[f] = lines;
        e.cells[f] = QRect(kInset + (e.fields[f].slot % columns) * (cellWidth + 14), y, cellWidth, h);
        rowHeight = std::max(rowHeight, h);
      }
      if (rowHeight == 0) continue;  // a row of slots this card has no values for
      y += (any ? 4 : 0) + rowHeight;
      if (any) for (int f = 0; f < e.fields.size(); ++f)
        if (e.fields[f].slot / columns == row) e.cells[f].translate(0, 4);
      any = true;
    }
  }
  // Room for the expander under the glyph.
  e.h = std::max(y + kPad, e.children > 0 ? 48 : 40);
}

void TableAtlas::Relayout() {
  int y = 4;
  if (dense_) LayoutRows();
  QHash<QString, CardLayout> used;  // what this layout used: the cache keeps no more
  for (auto& e : entries_) {
    if (dense_) {
      e.y = y;
      e.h = e.group ? 26 : kRowHeight;
      y += e.h + (e.group ? 2 : 0);
      continue;
    }
    // Wrapping every card's text each second is most of what cards cost: a card
    // whose text and width did not change keeps last time's layout.
    QString key = QString::number(CardWidth(e)) + '/' + QString::number(slots_) + '\x1e' + e.title + '\x1e' + e.metric + '\x1e' + e.state;
    for (const auto& f : e.fields) key += '\x1e' + QString::number(f.slot) + '\x1f' + f.value;
    if (auto hit = layoutCache_.constFind(key); hit != layoutCache_.constEnd()) {
      e.titleLines = hit->titleLines;
      e.metricBelow = hit->metricBelow;
      e.metricLines = hit->metricLines;
      e.valueLines = hit->valueLines;
      e.cells = hit->cells;
      e.h = hit->h;
    } else {
      LayoutEntry(e, CardWidth(e));
    }
    used.insert(key, CardLayout{e.titleLines, e.metricBelow, e.metricLines, e.valueLines, e.cells, e.h});
    e.y = y;
    y += e.h + kGap;
  }
  if (!dense_) layoutCache_ = std::move(used);
  contentHeight_ = y;
  scroll_->setGeometry(width() - kScrollWidth, ContentTop(), kScrollWidth, ViewHeight());
  scroll_->setRange(0, std::max(0, contentHeight_ - ViewHeight()));
  scroll_->setPageStep(std::max(1, ViewHeight()));
  scroll_->setSingleStep(40);
  scroll_->setVisible(contentHeight_ > ViewHeight());
}

QRect TableAtlas::EntryRect(int index) const {
  if (index < 0 || index >= entries_.size()) return {};
  const Entry& e = entries_[index];
  return QRect(CardX(e), ContentTop() + e.y - scroll_->value(), CardWidth(e), e.h);
}

QRect TableAtlas::SortRect() const { return QRect(width() / 2, 34, width() / 2 - 12, 18); }

int TableAtlas::HitTest(QPoint pos) const {
  if (pos.y() < ContentTop()) return -1;
  for (int i = 0; i < entries_.size(); ++i)
    if (EntryRect(i).contains(pos)) return i;
  return -1;
}

int TableAtlas::Current() const {
  const QModelIndex cur = source_->selectionModel()->currentIndex().siblingAtColumn(0);
  if (!cur.isValid()) return -1;
  for (int i = 0; i < entries_.size(); ++i)
    if (entries_[i].index == cur) return i;
  return -1;
}

void TableAtlas::EnsureVisible(int index) {
  const QRect r = EntryRect(index);
  if (r.isNull()) return;
  if (r.top() < ContentTop()) scroll_->setValue(scroll_->value() - (ContentTop() - r.top()) - 4);
  else if (r.bottom() > height()) scroll_->setValue(scroll_->value() + std::min(r.bottom() - height() + 4, r.top() - ContentTop()));
}

void TableAtlas::Select(int index, Qt::KeyboardModifiers mods) {
  if (index < 0 || index >= entries_.size() || !entries_[index].index.isValid()) return;
  auto* sel = source_->selectionModel();
  const QModelIndex i = entries_[index].index;
  const bool multi = source_->selectionMode() == QAbstractItemView::ExtendedSelection ||
                     source_->selectionMode() == QAbstractItemView::MultiSelection;
  if ((mods & Qt::ControlModifier) && multi) {
    sel->select(i, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
    sel->setCurrentIndex(i, QItemSelectionModel::NoUpdate);
    anchor_ = index;
  } else if ((mods & Qt::ShiftModifier) && anchor_ >= 0 && anchor_ < entries_.size() &&
             source_->selectionMode() == QAbstractItemView::ExtendedSelection) {
    QItemSelection range;
    for (int k = std::min(anchor_, index); k <= std::max(anchor_, index); ++k)
      if (!entries_[k].group) range.select(entries_[k].index, entries_[k].index);
    sel->select(range, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    sel->setCurrentIndex(i, QItemSelectionModel::NoUpdate);
  } else {
    sel->setCurrentIndex(i, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    anchor_ = index;
  }
  EnsureVisible(index);
  update();
}

void TableAtlas::Toggle(int index) {
  if (index < 0 || index >= entries_.size() || entries_[index].children == 0) return;
  source_->setExpanded(entries_[index].index, !entries_[index].expanded);
  Refresh();
}

void TableAtlas::ShowSortMenu() {
  QMenu menu(this);
  if (!source_->isSortingEnabled()) {
    menu.addAction(tr("This list keeps its own order"))->setEnabled(false);
  } else {
    auto* header = source_->header();
    const int current = header->sortIndicatorSection();
    for (int c = 0; c < source_->model()->columnCount(); ++c) {
      if (source_->isColumnHidden(c)) continue;
      QAction* a = menu.addAction(source_->model()->headerData(c, Qt::Horizontal).toString());
      a->setCheckable(true);
      a->setChecked(c == current);
      connect(a, &QAction::triggered, this, [this, c] { SortBy(c); });
    }
  }
  const QRect r = SortRect();
  menu.exec(mapToGlobal(QPoint(r.right() - 160, r.bottom() + 2)));
}

void TableAtlas::SortBy(int column) {
  if (!source_->isSortingEnabled() || column < 0) return;
  auto* header = source_->header();
  const bool same = column == header->sortIndicatorSection();
  source_->sortByColumn(column, same ? (header->sortIndicatorOrder() == Qt::AscendingOrder ? Qt::DescendingOrder
                                                                                           : Qt::AscendingOrder)
                                     : Qt::DescendingOrder);
  Refresh();
}

void TableAtlas::UpdateAnimation() {
  // Pixels run only along live connections; with none on screen nothing moves.
  bool live = false;
  if (domain_ == "connections" && isVisible())
    for (const auto& e : entries_) live = live || e.state.contains("ESTABLISHED");
  if (live && !animation_.isActive()) animation_.start();
  if (!live) animation_.stop();
}

bool TableAtlas::event(QEvent* event) {
  if (event->type() == QEvent::ToolTip) {
    auto* help = static_cast<QHelpEvent*>(event);
    if (ViewRect().contains(help->pos())) {
      QToolTip::showText(help->globalPos(), dense_ ? tr("Show every column on cards (v)") : tr("Show one row per entry (v)"),
                         this, ViewRect());
      return true;
    }
    if (dense_ && help->pos().y() >= ContentTop() - 20 && help->pos().y() < ContentTop() &&
        source_->isSortingEnabled() && !entries_.isEmpty()) {
      QToolTip::showText(help->globalPos(), tr("Click a column to sort by it"), this);
      return true;
    }
    if (SortRect().contains(help->pos())) {
      QToolTip::showText(help->globalPos(), tr("Sort by another column (s)"), this, SortRect());
      return true;
    }
    if (int i = HitTest(help->pos()); i >= 0) {
      // Everything the card holds, unabridged (values longer than six lines are cut on the card).
      const auto& e = entries_[i];
      QStringList lines{e.title};
      if (!e.metric.isEmpty()) lines << e.metric;
      if (!e.state.isEmpty()) lines << e.state;
      for (const auto& f : e.fields) lines << f.header + ": " + f.value;
      QToolTip::showText(help->globalPos(), safe::Tip(lines.join('\n')), this, EntryRect(i));
      return true;
    }
    QToolTip::hideText();
    event->ignore();
    return true;
  }
  return QWidget::event(event);
}

void TableAtlas::paintEvent(QPaintEvent*) {
  QPainter p(this);
  Paint(p);
  if (fade_.state() == QAbstractAnimation::Running && !from_.isNull() && dissolveCols_ > 0) {
    // Rows ↔ cards: the old view breaks into pixels. A cell lights up in the
    // page's colour just before it goes out, then the new view shows through.
    p.setClipping(false);
    const double t = fade_.currentValue().toDouble(), dpr = from_.devicePixelRatio();
    const QColor spark = Color(domain_);
    const int rows = int(dissolve_.size()) / dissolveCols_;
    for (int y = 0; y < rows; ++y) {
      int run = -1;  // consecutive old cells are copied in one go
      auto flush = [&](int end) {
        if (run < 0) return;
        const QRect target(run * kCell, y * kCell, (end - run) * kCell, kCell);
        p.drawPixmap(target, from_, QRectF(target.x() * dpr, target.y() * dpr, target.width() * dpr, target.height() * dpr));
        run = -1;
      };
      for (int x = 0; x < dissolveCols_; ++x) {
        const float when = dissolve_[y * dissolveCols_ + x];
        if (when > t + kSpark) {
          if (run < 0) run = x;
          continue;
        }
        flush(x);
        if (when > t) {
          QColor c = spark;
          c.setAlphaF(float(0.35 + 0.65 * (when - t) / kSpark));
          p.fillRect(x * kCell + 1, y * kCell + 1, kCell - 2, kCell - 2, c);
        }
      }
      flush(dissolveCols_);
    }
  }
}

void TableAtlas::Paint(QPainter& p) {
  if (grid_.size() != size() * devicePixelRatioF()) {
    grid_ = QPixmap(size() * devicePixelRatioF());
    grid_.setDevicePixelRatio(devicePixelRatioF());
    grid_.fill(theme::Base());
    QPainter g(&grid_);
    const QColor dot = theme::Mix(theme::Base(), theme::Border(), 0.35);
    for (int x = 6; x < width(); x += 9)
      for (int y = 6; y < height(); y += 9) g.fillRect(x, y, 1, 1, dot);
  }
  p.drawPixmap(0, 0, grid_);
  const QColor accent = Color(domain_);
  Glyph(p, {12, 10}, domain_, accent);
  p.setFont(theme::Mono(11, true));
  p.setPen(accent);
  const int titleWidth = width() - 46 - (ViewRect().isNull() ? 12 : 160);
  p.drawText(QRect(46, 8, titleWidth, 18), Qt::AlignLeft | Qt::AlignVCenter,
             QFontMetrics(p.font()).elidedText(Title(), Qt::ElideRight, titleWidth));
  p.setFont(theme::Mono(10));
  p.setPen(theme::Dim());
  if (!ViewRect().isNull()) p.drawText(ViewRect(), Qt::AlignRight | Qt::AlignVCenter, dense_ ? tr("ROWS · v") : tr("CARDS · v"));
  p.setFont(theme::Mono(10));
  p.setPen(theme::Dim());
  const int selected = int(source_->selectionModel()->selectedRows().size());
  p.drawText(QRect(46, 34, width() / 2 - 46, 18), Qt::AlignLeft | Qt::AlignVCenter,
             tr("%1 entries · %2 selected").arg(total_).arg(selected));
  if (source_->isSortingEnabled()) {
    const int section = source_->header()->sortIndicatorSection();
    const QString column = source_->model() ? source_->model()->headerData(section, Qt::Horizontal).toString() : QString();
    const QString sort = tr("SORT %1 %2").arg(column.toUpper(), source_->header()->sortIndicatorOrder() == Qt::AscendingOrder ? "▲" : "▼");
    p.setPen(accent);
    p.drawText(SortRect(), Qt::AlignRight | Qt::AlignVCenter, QFontMetrics(p.font()).elidedText(sort, Qt::ElideLeft, SortRect().width()));
  }
  p.setPen(theme::Border());
  p.drawLine(12, ContentTop() - 3, width() - 12, ContentTop() - 3);
  const int sorted = source_->isSortingEnabled() ? source_->header()->sortIndicatorSection() : -1;
  const QString arrow = source_->header()->sortIndicatorOrder() == Qt::AscendingOrder ? " ▲" : " ▼";
  if (dense_ && !entries_.isEmpty()) {
    // Column headers: click one to sort by it.
    p.setFont(theme::Mono(9));
    const QFontMetrics hm(p.font());
    const int hy = ContentTop() - 20;
    auto head = [&](int x, int w, int column, const QString& text) {
      p.setPen(column == sorted ? accent : theme::Dim());
      const QString label = text.toUpper() + (column == sorted ? arrow : QString());
      p.drawText(QRect(8 + x, hy, w, 16), Qt::AlignLeft | Qt::AlignVCenter, hm.elidedText(label, Qt::ElideRight, w));
    };
    head(30, rowTitleWidth_ - 30, cols_.title, cols_.headers.value(cols_.title));
    for (const auto& c : rowColumns_) head(c.x, c.w, c.column, c.header);
  }

  if (entries_.isEmpty()) {
    p.setPen(theme::Dim());
    p.drawText(rect().adjusted(12, ContentTop(), -12, -12), Qt::AlignCenter, tr("No entries yet"));
    return;
  }
  p.setClipRect(QRect(0, ContentTop(), width() - kScrollWidth, ViewHeight()));
  const QFont titleFont = theme::Mono(11, true), metricFont = theme::Mono(10), headerFont = theme::Mono(9);
  const QFontMetrics tf(titleFont), mf(metricFont), hf(headerFont);
  const int top = scroll_->value(), bottom = top + ViewHeight();
  for (int i = 0; i < entries_.size(); ++i) {
    const Entry& e = entries_[i];
    if (e.y + e.h < top) continue;
    if (e.y > bottom) break;
    const QRect r = EntryRect(i);
    const bool isSelected = source_->selectionModel()->isRowSelected(e.index.row(), e.index.parent());
    if (e.group) {
      // A section: its open/closed marker, title and size, over a rule.
      p.setFont(titleFont);
      p.setPen(isSelected ? theme::Bright() : accent);
      if (isSelected) p.fillRect(r, theme::Selection());
      // "Apps (5)" already counts itself.
      const QString text = e.title.endsWith(')') ? QString("%1 %2").arg(e.expanded ? "▾" : "▸", e.title)
                                                 : QString("%1 %2  ·  %3").arg(e.expanded ? "▾" : "▸", e.title).arg(e.children);
      const int indent = dense_ ? std::min(e.depth, 8) * 14 : 0;
      p.drawText(r.adjusted(4 + indent, 0, -4, -6), Qt::AlignLeft | Qt::AlignVCenter,
                 tf.elidedText(text, Qt::ElideRight, r.width() - 8 - indent));
      p.setPen(theme::Border());
      p.drawLine(r.left(), r.bottom() - 3, r.right(), r.bottom() - 3);
      continue;
    }
    const QColor c = StateColor(e.state, accent);
    if (dense_) {
      PaintRow(p, i, r, isSelected, c);
      continue;
    }
    p.fillRect(r, isSelected ? theme::Selection() : theme::Bg());
    p.setPen(isSelected ? c : theme::Border());
    p.drawRect(r.adjusted(0, 0, -1, -1));
    if (e.depth > 0) {  // a guide back to the parent card
      p.setPen(theme::Border());
      p.drawLine(r.left() - 8, r.top() - kGap, r.left() - 8, r.top() + 14);
      p.drawLine(r.left() - 8, r.top() + 14, r.left() - 1, r.top() + 14);
    }
    if (domain_ == "connections") {
      p.fillRect(r.x() + 8, r.y() + 10, 7, 7, c);
      p.fillRect(r.x() + 22, r.y() + 10, 7, 7, c);
      p.setPen(c);
      p.drawLine(r.x() + 15, r.y() + 13, r.x() + 22, r.y() + 13);
      if (e.state.contains("ESTABLISHED"))
        p.fillRect(r.x() + 8 + int(std::fmod(phase_ + i * .15, 1.0) * 18), r.y() + 22, 3, 3, c);
    } else {
      Glyph(p, r.topLeft() + QPoint(8, 9), domain_, c);
    }
    if (e.children > 0) {
      p.setFont(headerFont);
      p.setPen(theme::Dim());
      p.drawText(QRect(r.x() + 2, r.y() + 34, kInset - 4, hf.height()), Qt::AlignCenter,
                 QString("%1%2").arg(e.expanded ? "▾" : "▸").arg(e.children));
    }
    int y = r.y() + kPad;
    const int textX = r.x() + kInset, textWidth = r.width() - kInset - kPad;
    p.setFont(titleFont);
    p.setPen(theme::Text());
    for (const QString& line : e.titleLines) {
      p.drawText(QRect(textX, y, textWidth, tf.height()), Qt::AlignLeft | Qt::AlignVCenter, line);
      y += tf.height();
    }
    p.setFont(metricFont);
    p.setPen(c);
    QString metric = e.metric;
    if (!e.state.isEmpty() && !metric.contains(e.state)) metric += (metric.isEmpty() ? "" : " / ") + e.state;
    if (e.metricBelow) {
      y += 2;
      for (const QString& line : e.metricLines) {
        p.drawText(QRect(textX, y, textWidth, mf.height()), Qt::AlignLeft | Qt::AlignVCenter, line);
        y += mf.height();
      }
    } else if (!metric.isEmpty()) {
      p.drawText(QRect(textX, r.y() + kPad, textWidth, tf.height()), Qt::AlignRight | Qt::AlignVCenter, metric);
    }
    if (e.weight > 0 || e.percent) {
      const double fraction = e.percent ? qBound(0.0, e.weight / 100.0, 1.0) : (maxWeight_ > 0 ? e.weight / maxWeight_ : 0);
      Pixels(p, QRectF(textX, y + 4, textWidth, 5), fraction, c);
    }
    for (int f = 0; f < e.fields.size(); ++f) {
      const QRect cell = e.cells[f].translated(r.topLeft());
      p.setFont(headerFont);
      p.setPen(theme::Dim());
      p.drawText(QRect(cell.x(), cell.y(), cell.width(), hf.height()), Qt::AlignLeft | Qt::AlignVCenter,
                 hf.elidedText(e.fields[f].header.toUpper(), Qt::ElideRight, cell.width()));
      p.setFont(metricFont);
      p.setPen(theme::Text());
      int ly = cell.y() + hf.height();
      for (const QString& line : e.valueLines[f]) {
        p.drawText(QRect(cell.x(), ly, cell.width(), mf.height()), Qt::AlignLeft | Qt::AlignVCenter, line);
        ly += mf.height();
      }
    }
  }
}

void TableAtlas::PaintRow(QPainter& p, int index, const QRect& r, bool selected, QColor c) const {
  const Entry& e = entries_[index];
  const QFont rowFont = theme::Mono(10), nameFont = theme::Mono(10, true);
  const QFontMetrics mf(rowFont), nf(nameFont);
  p.fillRect(r, selected ? theme::Selection() : index % 2 ? theme::Mix(theme::Bg(), theme::Raised(), 0.45) : theme::Bg());
  if (selected) p.fillRect(QRect(r.x(), r.y(), 2, r.height()), c);
  const int indent = std::min(e.depth, 8) * 14;
  p.fillRect(QRect(r.x() + 8 + indent, r.center().y() - 2, 5, 5), c);
  if (e.children > 0) {
    p.setFont(rowFont);
    p.setPen(theme::Dim());
    p.drawText(QRect(r.x() + 16 + indent, r.y(), 12, r.height()), Qt::AlignCenter, e.expanded ? "▾" : "▸");
  }
  p.setFont(nameFont);
  p.setPen(theme::Text());
  const int nameX = 30 + indent, nameW = std::max(24, rowTitleWidth_ - nameX);
  QString name = e.title;
  if (e.children > 0 && !e.expanded) name += QString(" +%1").arg(e.children);
  p.drawText(QRect(r.x() + nameX, r.y(), nameW, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
             nf.elidedText(name, Qt::ElideRight, nameW));
  p.setFont(rowFont);
  for (const auto& col : rowColumns_) {
    const QString value = RowValue(e, col);
    if (value.isEmpty()) continue;
    const QRect cell(r.x() + col.x, r.y(), col.w, r.height());
    if (col.kind == RowColumn::Metric && (e.weight > 0 || e.percent)) {
      // The metric's share as a faint bar behind it: the heavy rows stand out.
      const double fraction = e.percent ? qBound(0.0, e.weight / 100.0, 1.0) : (maxWeight_ > 0 ? e.weight / maxWeight_ : 0);
      if (fraction > 0.004)
        p.fillRect(QRect(cell.x() - 4, r.y() + 3, std::max(2, int((col.w + 8) * fraction)), r.height() - 6),
                   theme::Mix(theme::Bg(), c, 0.28));
    }
    p.setPen(col.kind == RowColumn::Field ? theme::Text() : c);
    p.drawText(cell, Qt::AlignLeft | Qt::AlignVCenter, mf.elidedText(value, Qt::ElideRight, col.w));
  }
}

void TableAtlas::showEvent(QShowEvent*) { Refresh(); }
void TableAtlas::resizeEvent(QResizeEvent*) {
  grid_ = QPixmap();
  Relayout();  // card heights follow the width at once; the data refresh can wait
  Schedule();
}
void TableAtlas::hideEvent(QHideEvent*) {
  animation_.stop();
  fade_.stop();
  from_ = QPixmap();
  dissolve_.clear();
  refresh_.stop();
}

void TableAtlas::mousePressEvent(QMouseEvent* e) {
  setFocus();
  const QPoint pos = e->position().toPoint();
  if (ViewRect().contains(pos) && e->button() == Qt::LeftButton) {
    SetDense(!dense_);
    return;
  }
  if (SortRect().contains(pos) && e->button() == Qt::LeftButton) {
    ShowSortMenu();
    return;
  }
  if (dense_ && e->button() == Qt::LeftButton && pos.y() >= ContentTop() - 20 && pos.y() < ContentTop() &&
      source_->isSortingEnabled() && !entries_.isEmpty()) {
    const int x = pos.x() - 8;
    if (x < rowTitleWidth_) SortBy(cols_.title);
    for (const auto& c : rowColumns_)
      if (x >= c.x - kRowGap / 2 && x < c.x + c.w + kRowGap / 2) SortBy(c.column);
    return;
  }
  const int hit = HitTest(pos);
  if (hit < 0) return;
  const QRect r = EntryRect(hit);
  // The section header, or the ▸ marker under a card's glyph, opens and closes it.
  const int indent = std::min(entries_[hit].depth, 8) * 14;
  const bool expander = entries_[hit].group ||
                        (entries_[hit].children > 0 &&
                         (dense_ ? pos.x() < r.x() + 30 + indent : pos.x() < r.x() + kInset && pos.y() > r.y() + 30));
  if (e->button() == Qt::LeftButton && expander) Toggle(hit);
  // A right click keeps a multiple selection that already contains the card.
  if (e->button() == Qt::RightButton &&
      source_->selectionModel()->isRowSelected(entries_[hit].index.row(), entries_[hit].index.parent()))
    return;
  Select(HitTest(pos) >= 0 ? HitTest(pos) : hit, e->modifiers());
}

void TableAtlas::mouseDoubleClickEvent(QMouseEvent* e) {
  const int hit = HitTest(e->position().toPoint());
  if (hit < 0 || entries_[hit].group) return;
  // What a double click on the row does in the table: open folders, or the page's own action.
  if (entries_[hit].children > 0 && source_->expandsOnDoubleClick()) Toggle(hit);
  else emit source_->doubleClicked(entries_[hit].index);
}

void TableAtlas::wheelEvent(QWheelEvent* e) {
  scroll_->setValue(scroll_->value() - e->angleDelta().y() / 120 * 60);
  e->accept();
}

void TableAtlas::keyPressEvent(QKeyEvent* e) {
  const int cur = Current();
  const auto mods = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
  auto stepBy = [&](int delta) {
    if (entries_.isEmpty()) return;
    Select(std::clamp(cur < 0 ? 0 : cur + delta, 0, int(entries_.size()) - 1), mods & Qt::ShiftModifier);
  };
  auto pageBy = [&](int dir) {  // as many cards as fill the view
    if (entries_.isEmpty()) return;
    int i = std::max(0, cur);
    const int target = entries_[i].y + dir * ViewHeight();
    while (i + dir >= 0 && i + dir < entries_.size() && (dir > 0 ? entries_[i + dir].y <= target : entries_[i + dir].y >= target))
      i += dir;
    Select(i == cur ? std::clamp(cur + dir, 0, int(entries_.size()) - 1) : i, mods & Qt::ShiftModifier);
  };
  switch (e->key()) {
    case Qt::Key_Down: stepBy(1); return;
    case Qt::Key_Up: stepBy(-1); return;
    case Qt::Key_PageDown: pageBy(1); return;
    case Qt::Key_PageUp: pageBy(-1); return;
    case Qt::Key_Home: Select(0); return;
    case Qt::Key_End: Select(int(entries_.size()) - 1); return;
    case Qt::Key_Right:
      if (cur >= 0 && entries_[cur].children > 0 && !entries_[cur].expanded) Toggle(cur);
      else if (cur >= 0 && entries_[cur].expanded) Select(cur + 1);
      return;
    case Qt::Key_Left:
      if (cur >= 0 && entries_[cur].expanded) {
        Toggle(cur);
      } else if (cur >= 0) {
        for (int i = cur - 1; i >= 0; --i)
          if (entries_[i].depth < entries_[cur].depth) {
            Select(i);
            break;
          }
      }
      return;
    case Qt::Key_Menu: {
      const QPoint at = EntryRect(std::max(0, cur)).center();
      QContextMenuEvent menu(QContextMenuEvent::Keyboard, at, mapToGlobal(at));
      contextMenuEvent(&menu);
      return;
    }
    default: break;
  }
  if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && cur >= 0 && entries_[cur].group) {
    Toggle(cur);
    return;
  }
  if (e->text() == "s" && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
    ShowSortMenu();
    return;
  }
  if (e->text() == "v" && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
    SetDense(!dense_);
    return;
  }
  if (cur >= 0) {
    // Everything else is the page's: Delete, x, f, Enter… exactly as on the table row.
    QCoreApplication::sendEvent(source_, e);
    e->accept();
  } else {
    QWidget::keyPressEvent(e);
  }
}

void TableAtlas::contextMenuEvent(QContextMenuEvent* e) {
  if (e->reason() != QContextMenuEvent::Keyboard) {
    const int hit = HitTest(e->pos());
    if (hit < 0) return;
    if (!source_->selectionModel()->isRowSelected(entries_[hit].index.row(), entries_[hit].index.parent())) Select(hit);
    else source_->selectionModel()->setCurrentIndex(entries_[hit].index, QItemSelectionModel::NoUpdate);
  }
  const QModelIndex index = source_->selectionModel()->currentIndex().siblingAtColumn(0);
  if (!index.isValid() || Current() < 0) return;
  for (auto parent = index.parent(); parent.isValid(); parent = parent.parent()) source_->expand(parent);
  if (!source_->isVisible()) {
    // The table is never on screen, so it has no real geometry: give it the
    // atlas's place, so the menu the page opens at "its" row lands over the cards.
    source_->setGeometry(geometry());
    QResizeEvent resize(source_->size(), QSize());
    QCoreApplication::sendEvent(source_, &resize);
    source_->setAttribute(Qt::WA_PendingResizeEvent, false);
  }
  source_->doItemsLayout();
  source_->scrollTo(index, QAbstractItemView::PositionAtCenter);
  const QPoint position = source_->visualRect(index).center();
  if (source_->indexAt(position).siblingAtColumn(0) != index) return;
  emit source_->customContextMenuRequested(position);
}
}  // namespace td::ui
