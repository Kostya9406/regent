#include "ui/CommandPalette.h"

#include "ui/Theme.h"

#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

namespace td::ui {

namespace {

constexpr int kHintRole = Qt::UserRole + 1;
constexpr int kIndexRole = Qt::UserRole + 2;

// The command on the left, its hint (kind, shortcut) dim on the right.
class EntryDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
    QStyleOptionViewItem o = option;
    initStyleOption(&o, index);
    const QString hint = index.data(kHintRole).toString();
    const int hintWidth = hint.isEmpty() ? 0 : qMin(o.rect.width() / 2, o.fontMetrics.horizontalAdvance(hint) + 16);
    o.text = o.fontMetrics.elidedText(o.text, Qt::ElideRight, o.rect.width() - hintWidth - 16);
    QStyledItemDelegate::paint(p, o, index);
    if (hint.isEmpty()) return;
    p->save();
    p->setPen(o.state & QStyle::State_Selected ? theme::Bright() : theme::Dim());
    QRect r = o.rect.adjusted(0, 0, -8, 0);
    p->drawText(r, Qt::AlignRight | Qt::AlignVCenter, o.fontMetrics.elidedText(hint, Qt::ElideLeft, hintWidth - 8));
    p->restore();
  }
  QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    return {s.width(), qMax(s.height(), 26)};
  }
};

bool WordStart(const QString &t, int i) {
  if (i == 0) return true;
  const QChar c = t[i - 1];
  return c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == '/' || c == ':' || c == '@' ||
         (t[i].isUpper() && c.isLower());
}

int TokenScore(const QString &token, const QString &text, const QString &lower) {
  int best = -1;
  // Try every place the first letter occurs and keep the best greedy match from there.
  for (int start = lower.indexOf(token[0]); start >= 0; start = lower.indexOf(token[0], start + 1)) {
    int score = 0, pos = start, prev = -2;
    bool ok = true;
    for (QChar c : token) {
      pos = lower.indexOf(c, pos);
      if (pos < 0) {
        ok = false;
        break;
      }
      score += 10;
      if (WordStart(text, pos)) score += 15;
      if (pos == prev + 1) score += 12;
      else if (prev >= 0) score -= qMin(pos - prev, 10);
      prev = pos;
      ++pos;
    }
    if (!ok) break;
    score -= qMin(start, 20) / 2;  // earlier matches read as more relevant
    best = qMax(best, score);
  }
  return best;
}

}  // namespace

int CommandPalette::Score(const QString &query, const QString &text) {
  const QString lower = text.toLower();
  int total = 0;
  for (const QString &token : query.toLower().split(' ', Qt::SkipEmptyParts)) {
    const int s = TokenScore(token, text, lower);
    if (s < 0) return -1;
    total += s;
  }
  return total;
}

CommandPalette::CommandPalette(QWidget *parent) : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint) {
  setObjectName("CommandPalette");
  setAttribute(Qt::WA_DeleteOnClose, false);
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(10, 10, 10, 10);
  v->setSpacing(6);
  edit_ = new QLineEdit;
  edit_->setPlaceholderText(tr("Type a command, page, process or service…"));
  edit_->setFont(theme::Mono(16));
  edit_->installEventFilter(this);
  v->addWidget(edit_);
  list_ = new QListWidget;
  list_->setItemDelegate(new EntryDelegate(list_));
  list_->setUniformItemSizes(true);
  list_->setFocusPolicy(Qt::NoFocus);  // the keys stay in the search field
  connect(list_, &QListWidget::itemActivated, this, [this] { RunCurrent(); });
  connect(list_, &QListWidget::itemClicked, this, [this] { RunCurrent(); });
  v->addWidget(list_, 1);
  count_ = new QLabel;
  count_->setObjectName("Dim");
  count_->setText(tr("↑↓ choose · Enter run · Esc close"));
  v->addWidget(count_);
  connect(edit_, &QLineEdit::textChanged, this, [this] { Filter(); });
}

void CommandPalette::SetEntries(QVector<Entry> entries) {
  entries_ = std::move(entries);
  Filter();
}

void CommandPalette::AddEntries(const QVector<Entry> &entries) {
  entries_ += entries;
  if (isVisible()) Filter();
}

void CommandPalette::Popup() {
  QWidget *host = parentWidget();
  const int w = qMin(760, qMax(320, host->width() - 40));
  const int h = qMin(460, qMax(240, host->height() - 120));
  resize(w, h);
  move(host->mapToGlobal(QPoint((host->width() - w) / 2, qMin(90, host->height() / 8))));
  edit_->clear();
  Filter();
  show();
  raise();
  activateWindow();
  edit_->setFocus();
}

void CommandPalette::Filter() {
  const QString q = edit_->text().trimmed();
  QVector<QPair<int, int>> ranked;  // score, entry
  for (int i = 0; i < entries_.size(); ++i) {
    const int s = q.isEmpty() ? 0 : Score(q, entries_[i].text);
    if (s >= 0) ranked.push_back({s, i});
  }
  // Ties keep the given order: pages and commands first, then processes and services.
  std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
  list_->setUpdatesEnabled(false);
  list_->clear();
  for (int k = 0; k < qMin<int>(200, ranked.size()); ++k) {
    const Entry &e = entries_[ranked[k].second];
    auto *it = new QListWidgetItem(e.text);
    it->setData(kHintRole, e.hint);
    it->setData(kIndexRole, ranked[k].second);
    list_->addItem(it);
  }
  list_->setCurrentRow(list_->count() ? 0 : -1);
  list_->setUpdatesEnabled(true);
  count_->setText(tr("%1 of %2 · ↑↓ choose · Enter run · Esc close").arg(ranked.size()).arg(entries_.size()));
}

void CommandPalette::RunCurrent() {
  QListWidgetItem *it = list_->currentItem();
  if (!it) return;
  const int i = it->data(kIndexRole).toInt();
  if (i < 0 || i >= entries_.size()) return;
  auto run = entries_[i].run;
  hide();
  if (run) run();
}

bool CommandPalette::eventFilter(QObject *o, QEvent *e) {
  if (o == edit_ && e->type() == QEvent::KeyPress) {
    auto *k = static_cast<QKeyEvent *>(e);
    const int row = list_->currentRow();
    const bool ctrl = k->modifiers() & Qt::ControlModifier;
    switch (k->key()) {
      case Qt::Key_Down: list_->setCurrentRow(qMin(row + 1, list_->count() - 1)); return true;
      case Qt::Key_Up: list_->setCurrentRow(qMax(row - 1, 0)); return true;
      case Qt::Key_PageDown: list_->setCurrentRow(qMin(row + 10, list_->count() - 1)); return true;
      case Qt::Key_PageUp: list_->setCurrentRow(qMax(row - 10, 0)); return true;
      case Qt::Key_N: if (ctrl) { list_->setCurrentRow(qMin(row + 1, list_->count() - 1)); return true; } break;
      case Qt::Key_P: if (ctrl) { list_->setCurrentRow(qMax(row - 1, 0)); return true; } break;
      case Qt::Key_J: if (ctrl) { list_->setCurrentRow(qMin(row + 1, list_->count() - 1)); return true; } break;
      case Qt::Key_K: if (ctrl) { list_->setCurrentRow(qMax(row - 1, 0)); return true; } break;
      case Qt::Key_Return:
      case Qt::Key_Enter: RunCurrent(); return true;
      case Qt::Key_Escape: hide(); return true;
      default: break;
    }
  }
  return QDialog::eventFilter(o, e);
}

}  // namespace td::ui
