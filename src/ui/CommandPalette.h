#pragma once

// Ctrl+K: one fuzzy search over everything Regent can do — pages, every
// menu command, running processes, systemd services. Type a few letters of
// each word ("kil fir", "rst blu"), Enter runs the top match.

#include <QDialog>
#include <QVector>
#include <functional>

class QLineEdit;
class QListWidget;
class QLabel;

namespace td::ui {

class CommandPalette : public QDialog {
  Q_OBJECT
public:
  struct Entry {
    QString text;   // what is matched and shown: "Kill firefox (1234)"
    QString hint;   // dim, right side: "Process · 3% CPU" or a shortcut
    std::function<void()> run;
  };
  explicit CommandPalette(QWidget *parent);
  void SetEntries(QVector<Entry> entries);
  void AddEntries(const QVector<Entry> &entries);  // arrives later (services load off the GUI thread)
  void Popup();

  // Subsequence match with bonuses for word starts and runs; < 0 when it does not match.
  static int Score(const QString &query, const QString &text);

protected:
  bool eventFilter(QObject *o, QEvent *e) override;

private:
  void Filter();
  void RunCurrent();
  QLineEdit *edit_;
  QListWidget *list_;
  QLabel *count_;
  QVector<Entry> entries_;
};

}  // namespace td::ui
