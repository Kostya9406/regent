#pragma once

// A Flight Recorder session as one self-contained HTML page (inline SVG, no
// scripts, no external files) to attach to a bug report or send to someone.
// It names processes but leaves out command lines, which can carry secrets.

#include "core/FlightRecorder.h"

#include <QString>

namespace td::report {

QString Html(const std::deque<FlightRecorder::Frame> &frames, const QString &host);

}  // namespace td::report
