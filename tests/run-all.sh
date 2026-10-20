#!/bin/bash
# Builds and runs every test offscreen, with throwaway XDG directories so
# nothing touches your settings. Usage: tests/run-all.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/regent-tests.XXXXXX")} || exit 1
[ -n "$out" ] || exit 1
env=$out/env
mkdir -p "$env"/{cfg,data,state,cache,tmp} || exit 1
export XDG_CONFIG_HOME=$env/cfg XDG_DATA_HOME=$env/data XDG_STATE_HOME=$env/state XDG_CACHE_HOME=$env/cache TMPDIR=$env/tmp
export QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME=generic QT_STYLE_OVERRIDE=Fusion
failed=0
for t in flight-recorder-test flight-recorder-ui-test benchmarks-test page-recorder-test features-test window-test; do
  mkdir -p "$out/$t"
  if (cd "$out/$t" && qmake6 "$root/tests/$t.pro" && make -j"$(nproc)") >"$out/$t.build.log" 2>&1 &&
     (cd "$out/$t" && timeout 600 "./$t") >"$out/$t.log" 2>&1; then
    echo "PASS  $t"
  else
    echo "FAIL  $t  (logs: $out/$t.build.log, $out/$t.log)"; failed=1
  fi
done
exit $failed
