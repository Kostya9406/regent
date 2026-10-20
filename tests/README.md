# Tests

Every test is a small Qt program with its own `.pro` file. Run them all,
offscreen and with throwaway XDG directories (your settings are not touched):

```bash
tests/run-all.sh            # builds into a temporary directory
tests/run-all.sh /tmp/td    # or into a directory of your choice
```

Or one at a time:

```bash
mkdir -p /tmp/td-features && cd /tmp/td-features
qmake6 /path/to/regent/tests/features-test.pro && make -j"$(nproc)"
QT_QPA_PLATFORM=offscreen ./features-test
```

Needs `qt6-base` and a C++20 compiler; `page-recorder-test` also needs `ffmpeg`
with the libvpx-vp9 encoder. The offscreen platform has no tray or OpenGL, so
warnings about them are expected.

| Test | What it checks |
|---|---|
| `features-test` | alerts (hold time and margin, disk, memory, throttling, failed units at start), anomaly culprits, Flight Recorder file compatibility, `--fields`, unit names and cgroup sampling on a fake tree, palette matching, throttle verdict, all-core CPU and direct-I/O disk tests, process actions on every thread (nice, ionice, affinity, OOM) and refusal of a reused PID, plain-text tooltips, Hyprland dispatcher quoting |
| `window-test` | the whole window offscreen: navigation and Performance pop-out, every page at 700 px and full screen, the card overview (all columns, real parents, selection and context-menu target), Benchmarks history drawing, palette keys and Units, Flight Recorder dock, clean shutdown during a page recording |
| `flight-recorder-test` | bounded history, duplicate generations, 64-bit process identity, JSON round trip, rejected bad input, failed import keeps the old history |
| `flight-recorder-ui-test` | the Flight Recorder page: recording, history slider, plot drawing, confirmation before clearing or closing with unsaved history |
| `benchmarks-test` | every workload for 25 ms, cancellation before and during a run, temp-file cleanup for the disk test, a download from a loopback server started by the test (no public endpoint), teardown with active workers |
| `page-recorder-test` | page video recording through real ffmpeg/ffprobe: missing ffmpeg, invalid or existing targets, VP9/WebM output decoded frame by frame |

None of the tests act on other processes or on your desktop: process actions
only raise nice and the OOM score of the test itself.

Rendering every page to PNG without touching your settings:

```bash
QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME=$(mktemp -d) \
  REGENT_CAPTURE_SIZE=1280x800 ./build/regent --capture-pages /tmp/regent-pages
```

`REGENT_CAPTURE_PAGES=summary,performance` picks pages and
`REGENT_CAPTURE_DELAY=45000` waits that many milliseconds per page so graphs fill.
`REGENT_CAPTURE_HOSTNAME=omarchy` shows that name instead of the machine's, for
screenshots that will be published. Leave `XDG_RUNTIME_DIR` as it is: Hyprland's
socket lives there, and without it the window columns stay empty.
