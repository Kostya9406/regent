#pragma once

// Headless subcommands: no window, no GUI connection.
//
//   regent top [-n N] [--sort cpu|mem|gpu|wakeups]   busiest processes as a table
//   regent kill NAME|PID… [--force] [--all]          end processes (SIGTERM, or SIGKILL with --force)
//   regent menu                                      pick a process in the Omarchy menu, then act on it
//   regent --json --fields cpu,mem,temp              only some keys of the JSON snapshot

#include <QJsonObject>
#include <QStringList>

namespace td::cli {

// Runs `args` (argv without the program) when it names a subcommand; returns
// the exit status, or -1 when args are not a subcommand.
int Run(const QStringList &args);

// Keeps the given top-level keys or dotted paths (cpu.tempC); aliases: mem,
// temp, net, gpu, top, procs. Unknown names come back as null.
QJsonObject SelectFields(const QJsonObject &full, const QStringList &fields);

}  // namespace td::cli
