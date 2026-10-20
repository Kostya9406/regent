#include "core/FlightReport.h"

#include "core/Fs.h"

#include <QHash>

#include <algorithm>

namespace td::report {

namespace {

QString E(const QString &s) { return s.toHtmlEscaped(); }

// %1…%99 replaced in one pass, so text that came from a process name
// ("%4" is a valid name) is never substituted again.
QString Fill(const QString &tmpl, const QStringList &values) {
  QString out;
  out.reserve(tmpl.size() * 2);
  for (int i = 0; i < tmpl.size(); ++i) {
    if (tmpl[i] == '%' && i + 1 < tmpl.size() && tmpl[i + 1].isDigit()) {
      int n = tmpl[i + 1].digitValue(), len = 1;
      if (i + 2 < tmpl.size() && tmpl[i + 2].isDigit()) {
        n = n * 10 + tmpl[i + 2].digitValue();
        len = 2;
      }
      if (n >= 1 && n <= values.size()) {
        out += values[n - 1];
        i += len;
        continue;
      }
    }
    out += tmpl[i];
  }
  return out;
}

QString Num(double v, int precision) { return QString::number(v, 'f', precision); }

QString Polyline(const std::deque<FlightRecorder::Frame> &frames, double (*value)(const FlightRecorder::Frame &),
                 double w, double h) {
  QString points;
  const int n = int(frames.size());
  for (int i = 0; i < n; ++i) {
    const double v = value(frames[i]);
    if (v < 0) continue;
    const double x = n == 1 ? w / 2 : w * i / (n - 1);
    const double y = h - h * std::clamp(v, 0.0, 100.0) / 100;
    points += QString("%1,%2 ").arg(x, 0, 'f', 1).arg(y, 0, 'f', 1);
  }
  return points.trimmed();
}

double Cpu(const FlightRecorder::Frame &f) { return f.cpu; }
double Mem(const FlightRecorder::Frame &f) { return f.memoryTotal ? 100.0 * f.memoryUsed / f.memoryTotal : 0; }
double Temp(const FlightRecorder::Frame &f) { return f.tempC; }  // 0…100 °C on the same axis

}  // namespace

QString Html(const std::deque<FlightRecorder::Frame> &frames, const QString &host) {
  const auto stats = FlightRecorder::Summarize(frames);
  const auto anomalies = FlightRecorder::Anomalies(frames);
  const QString from = frames.empty() ? QString() : frames.front().time.toLocalTime().toString("yyyy-MM-dd HH:mm:ss");
  const QString to = frames.empty() ? QString() : frames.back().time.toLocalTime().toString("HH:mm:ss");

  // Processes by name: per frame the sum over same-named processes, then over the session.
  struct Agg {
    double cpuSum = 0, cpuPeak = 0;
    quint64 rssPeak = 0;
    int countPeak = 0;
  };
  QHash<QString, Agg> byName;
  for (const auto &f : frames) {
    QHash<QString, Agg> frame;
    for (const ProcessInfo &p : f.processes) {
      Agg &a = frame[p.name];
      a.cpuSum += p.cpu;
      a.rssPeak += p.rss;
      ++a.countPeak;
    }
    for (auto it = frame.cbegin(); it != frame.cend(); ++it) {
      Agg &a = byName[it.key()];
      a.cpuSum += it->cpuSum;
      a.cpuPeak = qMax(a.cpuPeak, it->cpuSum);
      a.rssPeak = qMax(a.rssPeak, it->rssPeak);
      a.countPeak = qMax(a.countPeak, it->countPeak);
    }
  }
  QVector<QPair<QString, Agg>> top;
  for (auto it = byName.cbegin(); it != byName.cend(); ++it) top.push_back({it.key(), *it});
  std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.second.cpuSum > b.second.cpuSum; });
  if (top.size() > 15) top.resize(15);

  const double W = 1000, H = 220;
  QString markers;
  const int n = int(frames.size());
  for (const auto &a : anomalies) {
    const double x = n == 1 ? W / 2 : W * a.index / (n - 1);
    markers += Fill("<g><title>%1 · %2%3</title><path d='M%4 0 l6 10 h-12 z' class='mark'/>"
                    "<line x1='%4' y1='10' x2='%4' y2='%5' class='markline'/></g>",
                    {frames[a.index].time.toLocalTime().toString("HH:mm:ss"), E(a.text),
                     a.culprit.isEmpty() ? QString() : " · " + E(a.culprit), Num(x, 1), Num(H, 0)});
  }

  QString rows;
  for (const auto &a : anomalies)
    rows += Fill("<tr><td>%1</td><td>%2</td><td>%3</td></tr>",
                 {frames[a.index].time.toLocalTime().toString("HH:mm:ss"), E(a.text), E(a.culprit)});
  QString procs;
  for (const auto &[name, a] : top)
    procs += Fill("<tr><td>%1</td><td class='n'>%2%</td><td class='n'>%3%</td><td class='n'>%4</td><td class='n'>%5</td></tr>",
                  {E(name), Num(n ? a.cpuSum / n : 0, 1), Num(a.cpuPeak, 1), fs::humanBytes(a.rssPeak),
                   QString::number(a.countPeak)});

  const bool hasTemp = stats.maxTempC >= 0;
  return Fill(R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Regent flight report · %1</title>
<style>
:root{--bg:#f6f6f4;--fg:#1d1f21;--dim:#6b6f73;--line:#d6d6d2;--cpu:#2f7dd1;--mem:#9a4fd0;--temp:#d1652f;--bad:#c23b3b}
@media (prefers-color-scheme:dark){:root{--bg:#121416;--fg:#d8dadc;--dim:#80868b;--line:#2b2f33;--cpu:#5aa5f5;--mem:#c08af0;--temp:#f08a50;--bad:#f06a6a}}
body{background:var(--bg);color:var(--fg);font:14px/1.45 ui-monospace,"JetBrains Mono",monospace;margin:0;padding:24px 16px}
main{max-width:1040px;margin:0 auto}h1{font-size:20px;margin:0 0 4px}h2{font-size:15px;margin:28px 0 8px}
.dim{color:var(--dim)}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px;margin:16px 0}
.card{border:1px solid var(--line);padding:8px 10px}.card b{display:block;font-size:18px}
svg{width:100%;height:auto;border:1px solid var(--line)}.cpu{stroke:var(--cpu)}.mem{stroke:var(--mem)}.temp{stroke:var(--temp);stroke-dasharray:4 3}
polyline{fill:none;stroke-width:2}.grid50{stroke:var(--line)}.mark{fill:var(--bad)}.markline{stroke:var(--bad);stroke-width:1;opacity:.35}
table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid var(--line);padding:4px 6px;text-align:left}.n{text-align:right}
.legend span{margin-right:16px}.legend i{display:inline-block;width:14px;height:3px;vertical-align:middle;margin-right:6px}
</style></head><body><main>
<h1>Flight report · %1</h1>
<div class="dim">%2 – %3 · %4 samples · %5</div>
<div class="grid">
<div class="card"><span class="dim">CPU average</span><b>%6%</b></div>
<div class="card"><span class="dim">CPU peak</span><b>%7%</b></div>
<div class="card"><span class="dim">Memory peak</span><b>%8%</b></div>
<div class="card"><span class="dim">CPU temperature peak</span><b>%9</b></div>
<div class="card"><span class="dim">Moments flagged</span><b>%10</b></div>
</div>
<div class="legend dim"><span><i style="background:var(--cpu)"></i>CPU %</span><span><i style="background:var(--mem)"></i>Memory %</span>%11<span><i style="background:var(--bad)"></i>flagged moment (hover)</span></div>
<svg viewBox="0 -12 %12 %13" role="img" aria-label="CPU and memory over the session">
<line x1="0" y1="%14" x2="%12" y2="%14" class="grid50"/>
<polyline class="mem" points="%15"/><polyline class="cpu" points="%16"/>%17
%18
</svg>
<h2>Flagged moments</h2>
%19
<h2>Busiest processes</h2>
<table><tr><th>Process</th><th class="n">CPU avg</th><th class="n">CPU peak</th><th class="n">Memory peak</th><th class="n">Instances</th></tr>%20</table>
<p class="dim">Process CPU is a share of the whole machine; same-named processes are summed. Command lines are not included.
Generated by Regent.</p>
</main></body></html>
)HTML",
              {E(host), from, to, QString::number(stats.samples), fs::humanDuration(stats.durationMs / 1000.0),
               Num(stats.avgCpu, 1), Num(stats.maxCpu, 1), Num(stats.maxMemPct, 1),
               hasTemp ? Num(stats.maxTempC, 0) + " °C" : QString("—"), QString::number(anomalies.size()),
               hasTemp ? QString("<span><i style=\"background:var(--temp)\"></i>CPU °C</span>") : QString(),
               Num(W, 0), Num(H + 12, 0), Num(H / 2, 0), Polyline(frames, Mem, W, H), Polyline(frames, Cpu, W, H),
               hasTemp ? QString("<polyline class=\"temp\" points=\"%1\"/>").arg(Polyline(frames, Temp, W, H)) : QString(),
               markers,
               rows.isEmpty() ? QString("<p class=\"dim\">Nothing unusual: no high CPU, memory, pressure or temperature.</p>")
                              : "<table><tr><th>Time</th><th>What</th><th>Likely cause</th></tr>" + rows + "</table>",
               procs});
}

}  // namespace td::report
