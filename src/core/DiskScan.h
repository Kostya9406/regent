#pragma once

// Walks one directory tree on a worker thread: directory sizes (allocated
// blocks, hard links counted once, never crossing into another filesystem),
// the largest files and a per-category breakdown. Pause and Stop are
// cooperative; progress is reported a few times a second.

#include <QString>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace td::diskscan {

struct Dir {
  QString name;
  quint64 bytes = 0;  // whole subtree
  quint64 files = 0;  // whole subtree
  quint64 ownFileBytes = 0;
  Dir *parent = nullptr;
  std::vector<std::unique_ptr<Dir>> children;  // sorted by bytes, descending, once done
  QString Path() const;
};

struct File {
  QString path;
  quint64 bytes;
};

enum Category { Media, Documents, Code, Archives, Applications, System, Other, CategoryCount };
QString CategoryName(int c);
Category CategoryOf(const char *name);

struct Result {
  std::unique_ptr<Dir> root;
  QVector<File> largest;  // top 200
  quint64 categoryBytes[CategoryCount] = {};
  quint64 categoryFiles[CategoryCount] = {};
  quint64 files = 0, dirs = 0, unreadable = 0;
  bool stopped = false;
  double seconds = 0;
};

struct Progress {
  quint64 files = 0, dirs = 0, bytes = 0;
  QString current;
};

class Scanner {
public:
  ~Scanner();
  // `done` and `progress` are called on the scanning thread.
  void Start(const QString &root, std::function<void(const Progress &)> progress,
             std::function<void(std::shared_ptr<Result>)> done);
  void SetPaused(bool p) { paused_ = p; }
  bool Paused() const { return paused_; }
  void Stop();         // stops and joins; for restart and teardown
  void RequestStop() {  // returns at once; the partial result still arrives through `done`
    stop_ = true;
    paused_ = false;
  }
  bool Running() const { return running_; }

private:
  void Run(QString root);
  std::thread thread_;
  std::atomic<bool> paused_{false}, stop_{false}, running_{false};
  std::function<void(const Progress &)> progress_;
  std::function<void(std::shared_ptr<Result>)> done_;
};

}  // namespace td::diskscan
