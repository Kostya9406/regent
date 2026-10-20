#include "core/DiskScan.h"

#include <QObject>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

namespace td::diskscan {

QString Dir::Path() const {
  if (!parent) return name;
  QString p = parent->Path();
  return p.endsWith('/') ? p + name : p + '/' + name;
}

QString CategoryName(int c) {
  switch (c) {
    case Media: return QObject::tr("Media");
    case Documents: return QObject::tr("Documents");
    case Code: return QObject::tr("Code");
    case Archives: return QObject::tr("Archives");
    case Applications: return QObject::tr("Applications");
    case System: return QObject::tr("System");
    default: return QObject::tr("Other");
  }
}

Category CategoryOf(const char *name) {
  const char *dot = std::strrchr(name, '.');
  if (!dot || dot == name) return Other;
  char ext[12];
  size_t n = std::strlen(dot + 1);
  if (n == 0 || n >= sizeof ext) return Other;
  for (size_t i = 0; i <= n; ++i) ext[i] = char(std::tolower(static_cast<unsigned char>(dot[1 + i])));
  static const std::pair<const char *, Category> table[] = {
      {"jpg", Media}, {"jpeg", Media}, {"png", Media}, {"gif", Media}, {"webp", Media}, {"heic", Media},
      {"svg", Media}, {"bmp", Media}, {"tif", Media}, {"tiff", Media}, {"raw", Media}, {"dng", Media},
      {"cr2", Media}, {"nef", Media}, {"psd", Media}, {"mp3", Media}, {"flac", Media}, {"ogg", Media},
      {"opus", Media}, {"wav", Media}, {"m4a", Media}, {"aac", Media}, {"mp4", Media}, {"mkv", Media},
      {"webm", Media}, {"mov", Media}, {"avi", Media}, {"m4v", Media},
      {"pdf", Documents}, {"doc", Documents}, {"docx", Documents}, {"odt", Documents}, {"xls", Documents},
      {"xlsx", Documents}, {"ods", Documents}, {"ppt", Documents}, {"pptx", Documents}, {"odp", Documents},
      {"txt", Documents}, {"md", Documents}, {"epub", Documents}, {"csv", Documents}, {"rtf", Documents},
      {"c", Code}, {"h", Code}, {"cpp", Code}, {"hpp", Code}, {"cc", Code}, {"rs", Code}, {"go", Code},
      {"py", Code}, {"js", Code}, {"ts", Code}, {"tsx", Code}, {"jsx", Code}, {"java", Code}, {"kt", Code},
      {"qml", Code}, {"sh", Code}, {"rb", Code}, {"lua", Code}, {"json", Code}, {"toml", Code}, {"yaml", Code},
      {"yml", Code}, {"html", Code}, {"css", Code}, {"sql", Code}, {"o", Code}, {"pyc", Code},
      {"zip", Archives}, {"tar", Archives}, {"gz", Archives}, {"xz", Archives}, {"zst", Archives},
      {"bz2", Archives}, {"7z", Archives}, {"rar", Archives}, {"iso", Archives}, {"img", Archives},
      {"deb", Applications}, {"rpm", Applications}, {"appimage", Applications}, {"apk", Applications},
      {"exe", Applications}, {"msi", Applications}, {"jar", Applications}, {"flatpak", Applications},
      {"so", System}, {"ko", System}, {"a", System}, {"log", System}, {"cache", System}, {"db", System},
      {"sqlite", System}, {"sqlite3", System}, {"tmp", System}, {"swp", System}, {"lock", System},
  };
  for (const auto &[e, c] : table)
    if (std::strcmp(e, ext) == 0) return c;
  return Other;
}

Scanner::~Scanner() { Stop(); }

void Scanner::Start(const QString &root, std::function<void(const Progress &)> progress,
                    std::function<void(std::shared_ptr<Result>)> done) {
  Stop();
  progress_ = std::move(progress);
  done_ = std::move(done);
  stop_ = false;
  paused_ = false;
  running_ = true;
  thread_ = std::thread([this, root] { Run(root); });
}

void Scanner::Stop() {
  stop_ = true;
  paused_ = false;
  if (thread_.joinable()) thread_.join();
}

void Scanner::Run(QString rootPath) {
  using Clock = std::chrono::steady_clock;
  auto started = Clock::now(), lastReport = started;
  auto result = std::make_shared<Result>();
  result->root = std::make_unique<Dir>();
  result->root->name = rootPath;

  struct stat rootSt;
  QByteArray rootBytes = rootPath.toLocal8Bit();
  if (::stat(rootBytes.constData(), &rootSt) != 0) {
    running_ = false;
    done_(result);
    return;
  }
  const dev_t device = rootSt.st_dev;
  result->root->ownFileBytes = quint64(rootSt.st_blocks) * 512;
  std::unordered_set<quint64> seenInodes;  // hard links: count once
  std::vector<File> top;                    // min-heap of the largest files
  auto heapCmp = [](const File &a, const File &b) { return a.bytes > b.bytes; };

  struct Frame {
    int fd;
    DIR *dir;
    Dir *node;
  };
  std::vector<Frame> stack;
  int rootFd = ::open(rootBytes.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (rootFd >= 0) {
    if (DIR *d = ::fdopendir(rootFd)) stack.push_back({rootFd, d, result->root.get()});
    else ::close(rootFd);
  }
  Progress prog;
  while (!stack.empty() && !stop_) {
    while (paused_ && !stop_) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Frame &f = stack.back();
    dirent *e = ::readdir(f.dir);
    if (!e) {
      ::closedir(f.dir);  // also closes f.fd
      stack.pop_back();
      continue;
    }
    if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
    struct stat st;
    if (::fstatat(f.fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      ++result->unreadable;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if (st.st_dev != device) continue;  // another filesystem is mounted here
      auto child = std::make_unique<Dir>();
      child->name = QString::fromLocal8Bit(e->d_name);
      child->parent = f.node;
      child->ownFileBytes = quint64(st.st_blocks) * 512;  // the directory's own blocks, as du counts them
      Dir *raw = child.get();
      f.node->children.push_back(std::move(child));
      ++result->dirs;
      ++prog.dirs;
      int fd = ::openat(f.fd, e->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (fd < 0) {
        ++result->unreadable;
        continue;
      }
      DIR *d = ::fdopendir(fd);
      if (!d) {
        ::close(fd);
        continue;
      }
      stack.push_back({fd, d, raw});
      continue;
    }
    if (!S_ISREG(st.st_mode)) continue;
    if (st.st_nlink > 1 && !seenInodes.insert((quint64(st.st_dev) << 40) ^ quint64(st.st_ino)).second) continue;
    quint64 bytes = quint64(st.st_blocks) * 512;
    f.node->ownFileBytes += bytes;
    ++f.node->files;
    ++result->files;
    ++prog.files;
    prog.bytes += bytes;
    Category c = CategoryOf(e->d_name);
    result->categoryBytes[c] += bytes;
    ++result->categoryFiles[c];
    if (top.size() < 200 || bytes > top.front().bytes) {
      top.push_back({f.node->Path() + '/' + QString::fromLocal8Bit(e->d_name), bytes});
      std::push_heap(top.begin(), top.end(), heapCmp);
      if (top.size() > 200) {
        std::pop_heap(top.begin(), top.end(), heapCmp);
        top.pop_back();
      }
    }
    auto now = Clock::now();
    if (now - lastReport > std::chrono::milliseconds(200)) {
      lastReport = now;
      prog.current = f.node->Path();
      progress_(prog);
    }
  }
  for (Frame &f : stack) ::closedir(f.dir);
  result->stopped = stop_;

  // Roll sizes up and sort children, deepest first (post-order without recursion).
  std::vector<Dir *> order{result->root.get()};
  for (size_t i = 0; i < order.size(); ++i)
    for (auto &c : order[i]->children) order.push_back(c.get());
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    Dir *d = *it;
    d->bytes += d->ownFileBytes;
    if (d->parent) {
      d->parent->bytes += d->bytes;
      d->parent->files += d->files;
    }
    std::sort(d->children.begin(), d->children.end(), [](auto &a, auto &b) { return a->bytes > b->bytes; });
  }
  std::sort(top.begin(), top.end(), [](const File &a, const File &b) { return a.bytes > b.bytes; });
  result->largest = QVector<File>(top.begin(), top.end());
  result->seconds = std::chrono::duration<double>(Clock::now() - started).count();
  running_ = false;
  done_(result);
}

}  // namespace td::diskscan
