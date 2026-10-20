#pragma once

// Network and local sockets from /proc/net/{tcp,tcp6,udp,udp6,unix}, each
// matched to its owning process through the socket inodes in /proc/<pid>/fd.
// Without root only our own processes' fds are readable, so other users'
// sockets keep their uid but no process name.

#include <QString>
#include <QVector>
#include <sys/types.h>

namespace td::connections {

enum class Exposure { Loopback, LocalNetwork, Internet, AllInterfaces, LocalIpc };
QString ExposureName(Exposure e);

struct Socket {
  QString proto;     // TCP, TCP6, UDP, UDP6, Unix
  QString state;     // LISTEN, ESTABLISHED, … ; UDP: UNCONN / CONNECTED
  QString localAddr; // address without port, or the Unix path
  QString remoteAddr;
  int localPort = 0;
  int remotePort = 0;
  quint64 inode = 0;
  uid_t uid = 0;
  QString user;
  int pid = 0;       // 0: not resolvable without root
  quint64 pidStart = 0;  // the owner's starttime, for PID-reuse checks
  QString process;
  QString service;   // /etc/services name of the listening / remote port
  quint64 txQueue = 0, rxQueue = 0;
  Exposure exposure = Exposure::Loopback;
  bool listening() const { return state == "LISTEN" || state == "UNCONN"; }
  QString Key() const { return proto + '|' + localAddr + ':' + QString::number(localPort) + '|' + remoteAddr + ':' +
                               QString::number(remotePort) + '|' + QString::number(inode); }
};

struct Table {
  QVector<Socket> sockets;
  int unresolved = 0;  // sockets whose owner we could not see
};

Table Collect(bool includeUnix);  // blocking; run off the GUI thread

}  // namespace td::connections
