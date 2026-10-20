#pragma once

// The machine-readable face of a Snapshot, for `regent --json` and
// `regent --stream`. This is the contract the Omarchy bar widget reads, so
// keys only ever get added, never renamed. Bump "schema" on a breaking change.

#include "core/Snapshot.h"

#include <QJsonObject>

namespace td {

constexpr int kJsonSchema = 1;
QJsonObject ToJson(const Snapshot &s, int topProcesses = 5);

}  // namespace td
