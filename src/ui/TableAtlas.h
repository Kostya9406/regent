#pragma once
#include <QHash>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QSet>
#include <QTimer>
#include <QTreeView>
#include <QVariantAnimation>
#include <QWidget>

class QPainter;
class QScrollBar;

namespace td::ui {
// The graphic face of a table: one card per row, every column of it written
// out in full, in the table's own order, sort and tree structure. The table
// (hidden) stays the model, the selection and the target of the page's keys
// and context menu, so every page action works on what the cards select.
class TableAtlas : public QWidget {
  Q_OBJECT
 public:
  explicit TableAtlas(QTreeView* source, QString domain, QWidget* parent = nullptr);
  void Refresh();
  int EntryCount() const { return entries_.size(); }
  double EntryWeight(int index) const { return index >= 0 && index < entries_.size() ? entries_[index].weight : 0; }
  QRect EntryRect(int index) const;  // in widget coordinates, after scrolling
  void ShowSortMenu();
  void SortBy(int column);  // a new column starts with its biggest values; the same column flips
  bool Dense() const { return dense_; }
  void SetDense(bool on);  // rows (one line per entry) or cards; remembered per page

 protected:
  bool event(QEvent*) override;
  void paintEvent(QPaintEvent*) override;
  void showEvent(QShowEvent*) override;
  void hideEvent(QHideEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  void contextMenuEvent(QContextMenuEvent*) override;

 private:
  struct Field {
    QString header, value;
    int slot = 0;  // the column's place among all field columns: cards line up
  };
  struct Entry {
    QPersistentModelIndex index;  // column 0 of the source row
    int depth = 0;
    bool group = false;  // has children and nothing but a title: a section header
    bool expanded = false;
    int children = 0;
    QString title, metric, state;
    QString metricValue;  // the metric without its header, for the rows view
    QVector<Field> fields;
    double weight = 0;
    bool percent = false;  // the metric is a share of 100 (CPU), not a size
    // Layout, in content coordinates.
    int y = 0, h = 0;
    QStringList titleLines;
    bool metricBelow = false;
    QStringList metricLines;  // when the metric does not fit beside the title
    QVector<QStringList> valueLines;  // per field
    QVector<QRect> cells;             // per field, relative to the card
  };
  struct Columns {
    int count = 0, pid = -1, title = 0, metric = -1, state = -1;
    QStringList headers;
    QVector<int> fields;  // columns shown as fields, in table order
  };
  // A column of the rows view: the metric, the state or a field slot.
  struct RowColumn {
    enum Kind { Metric, State, Field } kind;
    int column = -1;  // in the source model, for sorting by its header
    int slot = -1;    // Field: the slot it shows
    QString header;
    int x = 0, w = 0;  // relative to the row
  };
  void Schedule();
  void Collect(const QModelIndex& parent, int depth, const QString& path, const Columns& cols, int* visited);
  void Relayout();
  void LayoutEntry(Entry& e, int width) const;
  int ContentTop() const { return dense_ ? 78 : 58; }
  void LayoutRows();      // the rows view's columns, fitted to the width
  QString RowValue(const Entry& e, const RowColumn& c) const;
  void PaintRow(QPainter& p, int index, const QRect& r, bool selected, QColor c) const;
  void Paint(QPainter& p);
  QRect ViewRect() const;  // the rows/cards switch in the header
  int ViewHeight() const { return qMax(0, height() - ContentTop()); }
  int CardX(const Entry& e) const;
  int CardWidth(const Entry& e) const;
  int HitTest(QPoint pos) const;
  void Select(int index, Qt::KeyboardModifiers mods = Qt::NoModifier);
  void EnsureVisible(int index);
  void Toggle(int index);
  int Current() const;
  void UpdateAnimation();
  QString Title() const;
  QRect SortRect() const;
  QTreeView* source_;
  QString domain_;
  QVector<Entry> entries_;
  QTimer refresh_, animation_;
  QScrollBar* scroll_;
  double phase_ = 0, maxWeight_ = 0;
  int contentHeight_ = 0, total_ = 0, anchor_ = -1;
  QPixmap grid_;
  int slots_ = 0;               // field columns of the table
  Columns cols_;                 // as of the last Refresh
  bool dense_ = false;
  QVector<RowColumn> rowColumns_;
  QVariantAnimation fade_;  // rows ↔ cards pixel dissolve
  QPixmap from_;            // the view before the switch, breaking up
  QVector<float> dissolve_; // per cell: when it goes, 0…1
  int dissolveCols_ = 0;
  int rowTitleWidth_ = 0;
  struct CardLayout {
    QStringList titleLines;
    bool metricBelow = false;
    QStringList metricLines;
    QVector<QStringList> valueLines;
    QVector<QRect> cells;
    int h = 0;
  };
  QHash<QString, CardLayout> layoutCache_;  // by width and text: cards unchanged since the last layout
  QString rowLayoutKey_;          // width and columns the cached widths below are for
  QHash<int, int> rowWidths_;     // by source column; -1: the name
  QSet<QString> autoOpened_;    // sections opened once on first sight; the user may close them again  // dotted background, rebuilt on resize or theme change
};
}  // namespace td::ui
