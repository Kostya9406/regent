#include "core/Connections.h"

#include <QSet>

#include "core/Fs.h"

#include <QHash>
#include <QHostAddress>
#include <QObject>

#include <cstring>
#include <dirent.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace td::connections {

namespace {

const char *TcpState(int st) {
  static const char *names[] = {"?", "ESTABLISHED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1", "FIN_WAIT2", "TIME_WAIT",
                                "CLOSE", "CLOSE_WAIT", "LAST_ACK", "LISTEN", "CLOSING", "NEW_SYN_RECV"};
  return st > 0 && st < 13 ? names[st] : "?";
}

// /proc/net addresses are the kernel's in-memory bytes printed as
// native-endian 32-bit words: undo that word by word.
QHostAddress ParseAddr(const char *hex, size_t len) {
  if (len == 8) {
    quint32 v = quint32(std::strtoul(std::string(hex, 8).c_str(), nullptr, 16));
    quint8 b[4];
    std::memcpy(b, &v, 4);  // the word's in-memory bytes are the address bytes
    return QHostAddress(quint32(b[0]) << 24 | quint32(b[1]) << 16 | quint32(b[2]) << 8 | b[3]);
  }
  if (len == 32) {
    Q_IPV6ADDR a;
    for (int w = 0; w < 4; ++w) {
      quint32 v = quint32(std::strtoul(std::string(hex + w * 8, 8).c_str(), nullptr, 16));
      std::memcpy(a.c + w * 4, &v, 4);
    }
    QHostAddress h(a);
    bool ok = false;
    quint32 v4 = h.toIPv4Address(&ok);  // ::ffff:a.b.c.d → a.b.c.d
    return ok ? QHostAddress(v4) : h;
  }
  return {};
}

bool IsPrivate(const QHostAddress &a) {
  if (a.isLoopback() || a.isLinkLocal() || a.isUniqueLocalUnicast() || a.isSiteLocal()) return true;
  for (auto [net, bits] : {std::pair{"10.0.0.0", 8}, {"172.16.0.0", 12}, {"192.168.0.0", 16}, {"100.64.0.0", 10}, {"169.254.0.0", 16}})
    if (a.isInSubnet(QHostAddress(QString::fromLatin1(net)), bits)) return true;
  return false;
}

QString Service(int port, const char *proto) {
  thread_local QHash<QString, QString> cache;  // Collect() may run on any pool thread
  QString key = QString::number(port) + proto;
  auto it = cache.constFind(key);
  if (it != cache.constEnd()) return *it;
  // getservbyport_r reads /etc/services only; no network lookup happens.
  servent se, *res = nullptr;
  char buf[1024];
  QString name;
  if (port > 0 && getservbyport_r(htons(quint16(port)), proto, &se, buf, sizeof buf, &res) == 0 && res)
    name = QString::fromLatin1(res->s_name);
  cache.insert(key, name);
  return name;
}

QString UserName(uid_t uid) {
  thread_local QHash<uid_t, QString> cache;
  auto it = cache.constFind(uid);
  if (it != cache.constEnd()) return *it;
  passwd pw, *res = nullptr;
  char buf[4096];
  QString n = getpwuid_r(uid, &pw, buf, sizeof buf, &res) == 0 && res ? QString::fromLocal8Bit(pw.pw_name) : QString::number(uid);
  cache.insert(uid, n);
  return n;
}

struct Owner {
  int pid;
  quint64 start;
  QString comm;
};

quint64 StartTime(int pid) {
  std::string st = fs::readRaw(QString("/proc/%1/stat").arg(pid), 4096);
  size_t rp = st.rfind(')');
  if (rp == std::string::npos) return 0;
  const char *p = st.c_str() + rp + 2;
  for (int field = 3; field < 22 && p; ++field) {
    p = std::strchr(p, ' ');
    if (p) ++p;
  }
  return p ? std::strtoull(p, nullptr, 10) : 0;
}

// inode → owner for every socket fd we may read.
QHash<quint64, Owner> SocketOwners() {
  QHash<quint64, Owner> owners;
  DIR *proc = ::opendir("/proc");
  if (!proc) return owners;
  char path[320], link[128];
  while (dirent *e = ::readdir(proc)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    int pid = std::atoi(e->d_name);
    std::snprintf(path, sizeof path, "/proc/%d/fd", pid);
    DIR *fds = ::opendir(path);
    if (!fds) continue;
    QString comm;
    quint64 start = 0;
    while (dirent *f = ::readdir(fds)) {
      if (f->d_name[0] == '.') continue;
      std::snprintf(path, sizeof path, "/proc/%d/fd/%s", pid, f->d_name);
      ssize_t n = ::readlink(path, link, sizeof link - 1);
      if (n < 9 || std::strncmp(link, "socket:[", 8) != 0) continue;
      link[n] = 0;
      quint64 inode = std::strtoull(link + 8, nullptr, 10);
      if (comm.isEmpty()) {
        comm = fs::readText(QString("/proc/%1/comm").arg(pid));
        start = StartTime(pid);
      }
      if (!owners.contains(inode)) owners.insert(inode, {pid, start, comm});
    }
    ::closedir(fds);
  }
  ::closedir(proc);
  return owners;
}

void ReadInet(const char *file, const char *proto, bool udp, Table &t) {
  std::string text = fs::readRaw(file, 32 << 20);
  size_t pos = text.find('\n');  // skip the header
  while (pos != std::string::npos && pos + 1 < text.size()) {
    size_t end = text.find('\n', pos + 1);
    std::string l = text.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
    pos = end;
    char local[64], remote[64];
    unsigned st = 0, uid = 0;
    unsigned long long tx = 0, rx = 0, inode = 0;
    unsigned long tr = 0, when = 0, retr = 0, timeout = 0;
    int sl = 0;
    if (std::sscanf(l.c_str(), " %d: %63s %63s %x %llx:%llx %lx:%lx %lx %u %lu %llu", &sl, local, remote, &st, &tx, &rx,
                    &tr, &when, &retr, &uid, &timeout, &inode) < 12)
      continue;
    char *lc = std::strchr(local, ':'), *rc = std::strchr(remote, ':');
    if (!lc || !rc) continue;
    Socket s;
    s.proto = proto;
    QHostAddress la = ParseAddr(local, size_t(lc - local)), ra = ParseAddr(remote, size_t(rc - remote));
    s.localAddr = la.toString();
    s.remoteAddr = ra.toString();
    s.localPort = int(std::strtol(lc + 1, nullptr, 16));
    s.remotePort = int(std::strtol(rc + 1, nullptr, 16));
    if (udp) s.state = s.remotePort == 0 ? "UNCONN" : "CONNECTED";
    else s.state = TcpState(int(st));
    s.txQueue = tx;
    s.rxQueue = rx;
    s.uid = uid_t(uid);
    s.inode = inode;
    const char *sp = udp ? "udp" : "tcp";
    s.service = Service(s.listening() ? s.localPort : s.remotePort, sp);
    // No peer yet (listening, or bound but unconnected): exposure is where it is bound.
    bool noPeer = s.listening() || ra.isNull() || ra == QHostAddress::AnyIPv4 || ra == QHostAddress::AnyIPv6;
    if (noPeer) {
      s.exposure = la.isLoopback() ? Exposure::Loopback
                   : (la == QHostAddress::AnyIPv4 || la == QHostAddress::AnyIPv6) ? Exposure::AllInterfaces
                   : IsPrivate(la) ? Exposure::LocalNetwork
                                   : Exposure::Internet;  // bound to a public address
    } else {
      s.exposure = ra.isLoopback() ? Exposure::Loopback : IsPrivate(ra) ? Exposure::LocalNetwork : Exposure::Internet;
    }
    t.sockets << s;
  }
}

void ReadUnix(Table &t) {
  std::string text = fs::readRaw("/proc/net/unix", 32 << 20);
  // The kernel prints socket paths raw: a path with a newline can continue as a
  // line that looks like another socket. Keep the first entry of each inode.
  QSet<unsigned long long> seen;
  size_t pos = text.find('\n');
  while (pos != std::string::npos && pos + 1 < text.size()) {
    size_t end = text.find('\n', pos + 1);
    std::string l = text.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
    pos = end;
    char num[32];
    unsigned refs = 0, protocol = 0, flags = 0, type = 0, st = 0;
    unsigned long long inode = 0;
    int consumed = 0;
    if (std::sscanf(l.c_str(), "%31s %x %x %x %x %x %llu%n", num, &refs, &protocol, &flags, &type, &st, &inode, &consumed) < 7)
      continue;
    if (inode != 0 && seen.contains(inode)) continue;
    seen.insert(inode);
    Socket s;
    s.proto = "Unix";
    s.inode = inode;
    std::string path = l.substr(size_t(consumed));
    size_t a = path.find_first_not_of(' ');
    s.localAddr = a == std::string::npos ? QString() : QString::fromStdString(path.substr(a));
    if (s.localAddr.startsWith('@')) s.localAddr = QObject::tr("@%1 (abstract)").arg(s.localAddr.mid(1));
    // st: 1 unconnected, 3 connected; flag 0x10000 is __SO_ACCEPTCON (listening)
    s.state = (flags & 0x10000) ? "LISTEN" : st == 3 ? "CONNECTED" : "UNCONN";
    s.exposure = Exposure::LocalIpc;
    t.sockets << s;
  }
}

}  // namespace

QString ExposureName(Exposure e) {
  switch (e) {
    case Exposure::Loopback: return QObject::tr("Loopback");
    case Exposure::LocalNetwork: return QObject::tr("Local network");
    case Exposure::Internet: return QObject::tr("Internet");
    case Exposure::AllInterfaces: return QObject::tr("All interfaces");
    case Exposure::LocalIpc: return QObject::tr("Local IPC");
  }
  return {};
}

Table Collect(bool includeUnix) {
  Table t;
  ReadInet("/proc/net/tcp", "TCP", false, t);
  ReadInet("/proc/net/tcp6", "TCP6", false, t);
  ReadInet("/proc/net/udp", "UDP", true, t);
  ReadInet("/proc/net/udp6", "UDP6", true, t);
  if (includeUnix) ReadUnix(t);
  auto owners = SocketOwners();
  for (Socket &s : t.sockets) {
    auto it = owners.constFind(s.inode);
    if (it != owners.constEnd()) {
      s.pid = it->pid;
      s.pidStart = it->start;
      s.process = it->comm;
      if (s.proto == "Unix") {  // /proc/net/unix has no uid column: the owner's is the process's
        struct stat st;
        if (::stat(QString("/proc/%1").arg(s.pid).toLatin1().constData(), &st) == 0) s.uid = st.st_uid;
      }
    } else if (s.inode != 0) {
      ++t.unresolved;
    }
    s.user = s.proto == "Unix" && !s.pid ? QString() : UserName(s.uid);
  }
  return t;
}

}  // namespace td::connections
