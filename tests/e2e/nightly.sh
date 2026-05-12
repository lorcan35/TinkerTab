#!/usr/bin/env bash
# W9-B (audit 2026-05-11): nightly Tab5 soak via the e2e harness.
#
# Cron-deployable wrapper around `runner.py story_stress --reboot`.
# Resolves the Tab5 URL through tests/e2e/discover.py first so a DHCP
# rotation doesn't break overnight runs.  Output goes to a dated dir
# under tests/e2e/runs/nightly/; the latest report path is written to
# runs/nightly/LATEST for downstream tooling to pick up.
#
# Install as a user cron (no sudo needed):
#
#   crontab -e
#   # Run at 03:00 nightly, log to journald via systemd-cat
#   0 3 * * * /home/rebelforce/projects/TinkerTab/tests/e2e/nightly.sh \
#             2>&1 | systemd-cat -t tab5-nightly
#
# Exits non-zero on harness failure so cron's MAILTO will surface it.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
E2E_DIR="$REPO_DIR/tests/e2e"
RUNS_DIR="$E2E_DIR/runs/nightly"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_DIR="$RUNS_DIR/$TS"
mkdir -p "$RUN_DIR"

cd "$E2E_DIR"

# Resolve Tab5 URL first so we fail fast with a clear message instead of
# hanging the soak waiting on a dead IP.
if ! URL=$(python3 discover.py 2>&1); then
   echo "[nightly $TS] FATAL: Tab5 not found"
   echo "$URL"
   exit 2
fi
echo "[nightly $TS] Tab5 located at $URL"
echo "$URL" > "$RUN_DIR/tab5_url"

# story_stress = the 10-min mixed nav+screenshot+chat cycle (~6 cycles,
# 76 steps).  --reboot starts from clean state so a fragmentation-
# accumulation regression has a stable baseline.
TAB5_URL="$URL" python3 runner.py story_stress --reboot 2>&1 \
   | tee "$RUN_DIR/run.log"
RC=${PIPESTATUS[0]}

# The runner already writes its own per-run dir under runs/.  Surface
# its location for downstream tooling + keep a stable LATEST pointer
# at this script's run dir.
ln -sfn "$RUN_DIR" "$RUNS_DIR/LATEST"

if [[ $RC -ne 0 ]]; then
   echo "[nightly $TS] FAIL rc=$RC"
   exit $RC
fi
echo "[nightly $TS] PASS"
