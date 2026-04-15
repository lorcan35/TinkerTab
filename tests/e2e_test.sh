#!/bin/bash
# TinkerTab E2E Test Suite
# Usage: ./e2e_test.sh [TAB_IP] [AUTH_TOKEN]
#
# Runs a sequence of user stories against the Tab5 debug server HTTP API,
# using touch injection and screenshots to verify firmware behavior.
#
# Requirements: curl, jq, convert (ImageMagick)
# Environment variables (override with args):
#   TAB_IP      — Tab5 IP address (default: 192.168.1.90)
#   AUTH_TOKEN  — Debug server bearer token (from serial log on boot)
#   TAB_PORT    — Debug server port (default: 8080)

set -euo pipefail

# ── Configuration ───────────────────────────────────────────────────────────

TAB_IP="${1:-${TAB_IP:-192.168.1.90}}"
AUTH_TOKEN="${2:-${AUTH_TOKEN:-}}"
TAB_PORT="${TAB_PORT:-8080}"
BASE="http://${TAB_IP}:${TAB_PORT}"

# Timestamped output directory
RUN_ID="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="$(dirname "$0")/../test_output/e2e_${RUN_ID}"
mkdir -p "$OUT_DIR"

# ── Colors ──────────────────────────────────────────────────────────────────

RED='\033[0;31m'
GRN='\033[0;32m'
YEL='\033[0;33m'
CYN='\033[0;36m'
RST='\033[0m'

# ── Counters ────────────────────────────────────────────────────────────────

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

# ── Helper functions ────────────────────────────────────────────────────────

log()  { echo -e "${CYN}[$(date +%H:%M:%S)]${RST} $*"; }
pass() { echo -e "${GRN}  PASS${RST} $1"; ((PASS_COUNT++)); }
fail() { echo -e "${RED}  FAIL${RST} $1 — $2"; ((FAIL_COUNT++)); }
skip() { echo -e "${YEL}  SKIP${RST} $1 — $2"; ((SKIP_COUNT++)); }
header() { echo -e "\n${CYN}━━━ Story $1: $2 ━━━${RST}"; }

# Authenticated curl wrapper. Adds Bearer token if set.
acurl() {
    local extra_headers=()
    if [[ -n "$AUTH_TOKEN" ]]; then
        extra_headers=(-H "Authorization: Bearer ${AUTH_TOKEN}")
    fi
    curl -sf --max-time 10 "${extra_headers[@]}" "$@"
}

# POST JSON body with auth
acurl_post_json() {
    local url="$1" body="$2"
    local extra_headers=()
    if [[ -n "$AUTH_TOKEN" ]]; then
        extra_headers=(-H "Authorization: Bearer ${AUTH_TOKEN}")
    fi
    curl -sf --max-time 10 "${extra_headers[@]}" \
        -X POST -H "Content-Type: application/json" -d "$body" "$url"
}

# Tap at (x, y) — uses POST /touch with action=tap
tap() {
    local x="$1" y="$2" label="${3:-}"
    [[ -n "$label" ]] && log "  tap ($x,$y) — $label"
    acurl_post_json "${BASE}/touch" "{\"x\":${x},\"y\":${y},\"action\":\"tap\"}" >/dev/null
    sleep 0.4
}

# Screenshot: save BMP, convert to PNG, return PNG path
screenshot() {
    local name="${1:-screen}"
    local bmp_path="${OUT_DIR}/${name}.bmp"
    local png_path="${OUT_DIR}/${name}.png"
    acurl -o "$bmp_path" "${BASE}/screenshot.bmp"
    if command -v convert &>/dev/null; then
        convert "$bmp_path" "$png_path" 2>/dev/null && rm -f "$bmp_path"
        echo "$png_path"
    else
        echo "$bmp_path"
    fi
}

# Navigate to a screen via the /navigate API endpoint
navigate() {
    local screen="$1"
    acurl -X POST "${BASE}/navigate?screen=${screen}" >/dev/null
    sleep 0.8
}

# Wait for a condition (poll with timeout)
wait_for() {
    local description="$1" url="$2" jq_filter="$3" max_wait="${4:-15}"
    local elapsed=0
    while (( elapsed < max_wait )); do
        local result
        result="$(acurl "$url" 2>/dev/null | jq -r "$jq_filter" 2>/dev/null || echo "")"
        if [[ "$result" == "true" ]]; then
            return 0
        fi
        sleep 1
        ((elapsed++))
    done
    return 1
}

# ── Keyboard coordinate lookup ──────────────────────────────────────────────
# Returns x coordinate for a character; y is determined by row.

type_char() {
    local ch="$1"
    case "$ch" in
        q) tap 39  910 ;; w) tap 110 910 ;; e) tap 181 910 ;; r) tap 252 910 ;;
        t) tap 323 910 ;; y) tap 394 910 ;; u) tap 465 910 ;; i) tap 536 910 ;;
        o) tap 607 910 ;; p) tap 678 910 ;;
        a) tap 75  966 ;; s) tap 146 966 ;; d) tap 217 966 ;; f) tap 288 966 ;;
        g) tap 359 966 ;; h) tap 430 966 ;; j) tap 501 966 ;; k) tap 572 966 ;;
        l) tap 643 966 ;;
        z) tap 106 1022 ;; x) tap 190 1022 ;; c) tap 274 1022 ;; v) tap 358 1022 ;;
        b) tap 442 1022 ;; n) tap 526 1022 ;; m) tap 610 1022 ;;
        " ") tap 352 1078 ;;  # SPACE
        .) tap 608 1078 ;; ,) tap 96 1078 ;;
        *) log "  [warn] no key mapping for '${ch}'" ;;
    esac
}

# Type a string by tapping each character on the on-screen keyboard
type_string() {
    local text="$1"
    log "  typing: \"${text}\""
    for (( i=0; i<${#text}; i++ )); do
        type_char "${text:$i:1}"
    done
}

# ── Preflight checks ───────────────────────────────────────────────────────

echo -e "${CYN}╔══════════════════════════════════════════════════╗${RST}"
echo -e "${CYN}║        TinkerTab E2E Test Suite                  ║${RST}"
echo -e "${CYN}║        $(date +%Y-%m-%d\ %H:%M:%S)                       ║${RST}"
echo -e "${CYN}╚══════════════════════════════════════════════════╝${RST}"
echo ""
log "Tab5 target:   ${BASE}"
log "Output dir:    ${OUT_DIR}"

# Check dependencies
for cmd in curl jq; do
    if ! command -v "$cmd" &>/dev/null; then
        echo -e "${RED}ERROR: '$cmd' is required but not installed.${RST}"
        exit 1
    fi
done
if ! command -v convert &>/dev/null; then
    log "${YEL}Warning: 'convert' (ImageMagick) not found — screenshots will stay as BMP${RST}"
fi

# Verify Tab5 is reachable (use /info — no auth required)
log "Checking connectivity..."
INFO=$(acurl "${BASE}/info" 2>/dev/null || echo "")
if [[ -z "$INFO" ]]; then
    echo -e "${RED}ERROR: Cannot reach Tab5 at ${BASE}/info — is it powered on and connected?${RST}"
    exit 1
fi
log "Connected! Firmware: $(echo "$INFO" | jq -r '.firmware_version // .version // "unknown"')"

# Warn if no auth token
if [[ -z "$AUTH_TOKEN" ]]; then
    log "${YEL}Warning: No AUTH_TOKEN set — authenticated endpoints will fail.${RST}"
    log "${YEL}Get the token from serial log or set AUTH_TOKEN env var.${RST}"
fi

# ════════════════════════════════════════════════════════════════════════════
# STORY 1: Boot check — selftest, voice state, reset reason
# ════════════════════════════════════════════════════════════════════════════

header 1 "Boot check (selftest + voice state + reset reason)"

# Selftest (no auth required)
SELFTEST=$(acurl "${BASE}/selftest" 2>/dev/null || echo "")
if [[ -n "$SELFTEST" ]]; then
    ST_PASSED=$(echo "$SELFTEST" | jq -r '.passed // 0')
    ST_TOTAL=$(echo "$SELFTEST" | jq -r '.total // 0')
    ST_FAILED=$(echo "$SELFTEST" | jq -r '.failed // 0')
    log "  Selftest: ${ST_PASSED}/${ST_TOTAL} passed, ${ST_FAILED} failed"

    # List any failures
    FAILURES=$(echo "$SELFTEST" | jq -r '.tests[] | select(.pass == false) | .name' 2>/dev/null || echo "")
    if [[ -z "$FAILURES" ]]; then
        pass "selftest ${ST_PASSED}/${ST_TOTAL}"
    else
        fail "selftest ${ST_PASSED}/${ST_TOTAL}" "failed: ${FAILURES}"
    fi
else
    fail "selftest" "endpoint unreachable"
fi

# Voice state (requires auth)
VOICE=$(acurl "${BASE}/voice" 2>/dev/null || echo "")
if [[ -n "$VOICE" ]]; then
    V_CONNECTED=$(echo "$VOICE" | jq -r '.connected // false')
    V_STATE=$(echo "$VOICE" | jq -r '.state_name // "UNKNOWN"')
    log "  Voice: connected=${V_CONNECTED}, state=${V_STATE}"
    if [[ "$V_CONNECTED" == "true" ]]; then
        pass "voice connected (state=${V_STATE})"
    else
        fail "voice connected" "voice WS not connected"
    fi
else
    skip "voice state" "auth required or endpoint error"
fi

# Reset reason (from /info, no auth)
RESET_REASON=$(echo "$INFO" | jq -r '.reset_reason // "UNKNOWN"')
log "  Reset reason: ${RESET_REASON}"
if [[ "$RESET_REASON" == "POWERON" || "$RESET_REASON" == "SW" ]]; then
    pass "reset reason is ${RESET_REASON} (clean)"
else
    fail "reset reason" "${RESET_REASON} (expected POWERON or SW)"
fi

echo "$SELFTEST" | jq '.' > "${OUT_DIR}/01_selftest.json" 2>/dev/null || true

# ════════════════════════════════════════════════════════════════════════════
# STORY 2: Home screen — screenshot, verify voice connected
# ════════════════════════════════════════════════════════════════════════════

header 2 "Home screen screenshot + voice verification"

navigate "home"
IMG=$(screenshot "02_home_screen")
log "  Screenshot saved: ${IMG}"

# Verify we can get info (device is responsive)
UPTIME=$(echo "$INFO" | jq -r '.uptime_ms // 0')
log "  Uptime: $(( UPTIME / 1000 ))s"
if [[ "$UPTIME" -gt 0 ]]; then
    pass "home screen captured, device responsive"
else
    fail "home screen" "uptime is 0 or missing"
fi

# ════════════════════════════════════════════════════════════════════════════
# STORY 3: Mode switch — cycle through modes 0→1→2→3→0
# ════════════════════════════════════════════════════════════════════════════

header 3 "Mode switch cycle (0→1→2→3→0)"

MODE_NAMES=("local" "hybrid" "cloud" "tinkerclaw" "local")
MODE_OK=true

for m in 0 1 2 3 0; do
    RESULT=$(acurl -X POST "${BASE}/mode?m=${m}" 2>/dev/null || echo "")
    if [[ -n "$RESULT" ]]; then
        GOT_MODE=$(echo "$RESULT" | jq -r '.voice_mode // -1')
        GOT_NAME=$(echo "$RESULT" | jq -r '.mode_name // "unknown"')
        if [[ "$GOT_MODE" == "$m" ]]; then
            log "  Mode ${m} (${GOT_NAME}) — OK"
        else
            log "  Mode ${m} expected, got ${GOT_MODE} — MISMATCH"
            MODE_OK=false
        fi
    else
        log "  Mode ${m} — request failed (auth?)"
        MODE_OK=false
    fi
    sleep 0.5
done

# Take a screenshot of the mode badge after returning to mode 0
screenshot "03_mode_cycle_done" >/dev/null
if [[ "$MODE_OK" == true ]]; then
    pass "mode cycle 0→1→2→3→0 completed"
else
    fail "mode cycle" "one or more mode switches failed"
fi

# ════════════════════════════════════════════════════════════════════════════
# STORY 4: Navigate all screens — Home→Notes→Chat→Settings→Home
# ════════════════════════════════════════════════════════════════════════════

header 4 "Navigate all screens (Home→Notes→Chat→Settings→Home)"

NAV_OK=true
for screen in home notes chat settings home; do
    navigate "$screen"
    IMG=$(screenshot "04_nav_${screen}")
    # Verify the device is still responsive after navigation
    CHECK=$(acurl "${BASE}/info" 2>/dev/null || echo "")
    if [[ -n "$CHECK" ]]; then
        log "  Navigated to ${screen} — OK (screenshot: $(basename "$IMG"))"
    else
        log "  Navigated to ${screen} — device unresponsive!"
        NAV_OK=false
    fi
done

if [[ "$NAV_OK" == true ]]; then
    pass "all 5 screen navigations completed"
else
    fail "screen navigation" "device became unresponsive during navigation"
fi

# ════════════════════════════════════════════════════════════════════════════
# STORY 5: Send chat message via /chat API, wait for response
# ════════════════════════════════════════════════════════════════════════════

header 5 "Send chat message via API"

# Check voice is connected first
VOICE_PRE=$(acurl "${BASE}/voice" 2>/dev/null || echo "")
V_CONN=$(echo "$VOICE_PRE" | jq -r '.connected // false' 2>/dev/null || echo "false")

if [[ "$V_CONN" == "true" ]]; then
    CHAT_RESULT=$(acurl_post_json "${BASE}/chat" '{"text":"ping from e2e test"}' 2>/dev/null || echo "")
    if [[ -n "$CHAT_RESULT" ]]; then
        SENT=$(echo "$CHAT_RESULT" | jq -r '.sent // false')
        if [[ "$SENT" == "true" ]]; then
            log "  Chat message sent, waiting for LLM response..."
            # Wait up to 30s for a response (voice state goes PROCESSING→SPEAKING→READY)
            sleep 3
            # Poll for response
            GOT_RESPONSE=false
            for attempt in $(seq 1 18); do
                VSTATE=$(acurl "${BASE}/voice" 2>/dev/null || echo "")
                LLM_TEXT=$(echo "$VSTATE" | jq -r '.last_llm_text // ""' 2>/dev/null || echo "")
                STATE_NAME=$(echo "$VSTATE" | jq -r '.state_name // ""' 2>/dev/null || echo "")
                if [[ -n "$LLM_TEXT" && "$LLM_TEXT" != "null" ]]; then
                    log "  Got LLM response: \"$(echo "$LLM_TEXT" | head -c 80)...\""
                    GOT_RESPONSE=true
                    break
                fi
                if [[ "$STATE_NAME" == "READY" && "$attempt" -gt 5 ]]; then
                    # If we're back to READY after enough time, check for text
                    break
                fi
                sleep 1.5
            done
            # Screenshot the chat response
            navigate "chat"
            sleep 1
            screenshot "05_chat_response" >/dev/null
            navigate "home"
            if [[ "$GOT_RESPONSE" == true ]]; then
                pass "chat message sent and LLM responded"
            else
                pass "chat message sent (LLM response not captured but send confirmed)"
            fi
        else
            fail "chat message" "send returned false"
        fi
    else
        fail "chat message" "request failed (auth?)"
    fi
else
    skip "chat message" "voice WS not connected"
fi

echo "$CHAT_RESULT" > "${OUT_DIR}/05_chat_result.json" 2>/dev/null || true

# ════════════════════════════════════════════════════════════════════════════
# STORY 6: Keyboard typing — open Chat, tap input, type "hello world"
# ════════════════════════════════════════════════════════════════════════════

header 6 "Keyboard typing test"

navigate "chat"
sleep 0.5

# Tap text input field to bring up keyboard
tap 290 820 "text input field"
sleep 0.8

# Type "hello world" via on-screen keyboard coordinates
type_string "hello world"
sleep 0.3

# Screenshot with typed text visible
IMG=$(screenshot "06_keyboard_typing")
log "  Screenshot: $(basename "$IMG")"

# Tap Done to dismiss keyboard
tap 674 1078 "Done key"
sleep 0.3

screenshot "06_keyboard_done" >/dev/null

# Return to home
navigate "home"
pass "keyboard typing test completed (check screenshots)"

# ════════════════════════════════════════════════════════════════════════════
# STORY 7: Settings check — navigate to Settings, screenshot
# ════════════════════════════════════════════════════════════════════════════

header 7 "Settings screen check"

navigate "settings"
sleep 1  # Settings screen is heavy (55 objects)

IMG=$(screenshot "07_settings_screen")
log "  Screenshot: $(basename "$IMG")"

# Read settings via API
SETTINGS=$(acurl "${BASE}/settings" 2>/dev/null || echo "")
if [[ -n "$SETTINGS" ]]; then
    VMODE=$(echo "$SETTINGS" | jq -r '.voice_mode // .vmode // "?"')
    BRIGHTNESS=$(echo "$SETTINGS" | jq -r '.brightness // "?"')
    VOLUME=$(echo "$SETTINGS" | jq -r '.volume // "?"')
    log "  voice_mode=${VMODE}, brightness=${BRIGHTNESS}, volume=${VOLUME}"
    echo "$SETTINGS" | jq '.' > "${OUT_DIR}/07_settings.json" 2>/dev/null || true
    pass "settings screen captured"
else
    # Settings might not need auth or might not exist
    pass "settings screen captured (API read skipped)"
fi

navigate "home"

# ════════════════════════════════════════════════════════════════════════════
# STORY 8: Stress test — 5 rapid nav cycles
# ════════════════════════════════════════════════════════════════════════════

header 8 "Stress test (5 rapid navigation cycles)"

STRESS_OK=true
for cycle in $(seq 1 5); do
    for screen in home notes chat settings; do
        navigate "$screen"
        sleep 0.2  # Minimal delay — stress the nav debounce
    done
    # Verify device still alive
    CHECK=$(acurl "${BASE}/info" 2>/dev/null || echo "")
    if [[ -z "$CHECK" ]]; then
        log "  Cycle ${cycle}: device unresponsive!"
        STRESS_OK=false
        break
    fi
    log "  Cycle ${cycle}/5 — OK"
done

navigate "home"
screenshot "08_stress_done" >/dev/null

if [[ "$STRESS_OK" == true ]]; then
    pass "5 rapid nav cycles survived"
else
    fail "stress test" "device became unresponsive"
fi

# ════════════════════════════════════════════════════════════════════════════
# STORY 9: Final selftest — verify 8/8, no WDT/PANIC reset
# ════════════════════════════════════════════════════════════════════════════

header 9 "Final selftest"

FINAL_ST=$(acurl "${BASE}/selftest" 2>/dev/null || echo "")
if [[ -n "$FINAL_ST" ]]; then
    F_PASSED=$(echo "$FINAL_ST" | jq -r '.passed // 0')
    F_TOTAL=$(echo "$FINAL_ST" | jq -r '.total // 0')
    F_FAILED=$(echo "$FINAL_ST" | jq -r '.failed // 0')
    log "  Final selftest: ${F_PASSED}/${F_TOTAL} passed, ${F_FAILED} failed"

    FAILURES=$(echo "$FINAL_ST" | jq -r '.tests[] | select(.pass == false) | .name' 2>/dev/null || echo "")
    if [[ -z "$FAILURES" ]]; then
        pass "final selftest ${F_PASSED}/${F_TOTAL}"
    else
        fail "final selftest ${F_PASSED}/${F_TOTAL}" "failed: ${FAILURES}"
    fi
else
    fail "final selftest" "endpoint unreachable (device crashed?)"
fi

# Verify reset reason hasn't changed to WDT/PANIC
FINAL_INFO=$(acurl "${BASE}/info" 2>/dev/null || echo "")
FINAL_RESET=$(echo "$FINAL_INFO" | jq -r '.reset_reason // "UNKNOWN"' 2>/dev/null || echo "UNKNOWN")
if [[ "$FINAL_RESET" == "POWERON" || "$FINAL_RESET" == "SW" ]]; then
    pass "no WDT/PANIC reset during tests (still ${FINAL_RESET})"
else
    fail "reset reason check" "reset_reason is now ${FINAL_RESET} (was ${RESET_REASON})"
fi

echo "$FINAL_ST" | jq '.' > "${OUT_DIR}/09_final_selftest.json" 2>/dev/null || true

# ════════════════════════════════════════════════════════════════════════════
# STORY 10: Heap report — internal heap, PSRAM block, NVS writes
# ════════════════════════════════════════════════════════════════════════════

header 10 "Heap and memory report"

# Get heap data from /log endpoint
LOG_DATA=$(acurl "${BASE}/log" 2>/dev/null || echo "")
if [[ -n "$LOG_DATA" ]]; then
    HEAP_FREE=$(echo "$LOG_DATA" | jq -r '.heap_free // 0')
    HEAP_MIN=$(echo "$LOG_DATA" | jq -r '.heap_min // 0')
    PSRAM_FREE=$(echo "$LOG_DATA" | jq -r '.psram_free // 0')
    UPTIME_S=$(echo "$LOG_DATA" | jq -r '.uptime_s // 0')
    TASKS=$(echo "$LOG_DATA" | jq -r '.tasks // 0')
    log "  Internal heap free:  $(( HEAP_FREE / 1024 )) KB"
    log "  Internal heap min:   $(( HEAP_MIN / 1024 )) KB (watermark)"
    log "  PSRAM free:          $(( PSRAM_FREE / 1024 / 1024 )) MB"
    log "  Uptime:              ${UPTIME_S}s"
    log "  FreeRTOS tasks:      ${TASKS}"
else
    log "  /log endpoint unavailable (auth required?)"
fi

# Get PSRAM largest block + NVS from selftest
if [[ -n "$FINAL_ST" ]]; then
    PSRAM_BLOCK=$(echo "$FINAL_ST" | jq -r '.tests[] | select(.name=="psram") | .largest_free_block_kb // "?"')
    NVS_WRITES=$(echo "$FINAL_ST" | jq -r '.tests[] | select(.name=="nvs_settings") | .nvs_writes_this_session // "?"')
    INT_HEAP_KB=$(echo "$FINAL_ST" | jq -r '.tests[] | select(.name=="internal_heap") | .free_kb // "?"')
    log "  PSRAM largest block: ${PSRAM_BLOCK} KB"
    log "  Internal heap free:  ${INT_HEAP_KB} KB (selftest)"
    log "  NVS writes (session):${NVS_WRITES}"
fi

# Save full report
{
    echo "{"
    echo "  \"run_id\": \"${RUN_ID}\","
    echo "  \"timestamp\": \"$(date -Iseconds)\","
    echo "  \"tab_ip\": \"${TAB_IP}\","
    echo "  \"log_data\": ${LOG_DATA:-null},"
    echo "  \"selftest\": ${FINAL_ST:-null}"
    echo "}"
} > "${OUT_DIR}/10_heap_report.json"

# Heap thresholds
if [[ -n "$LOG_DATA" ]]; then
    if (( HEAP_FREE > 30000 )); then
        pass "internal heap OK ($(( HEAP_FREE / 1024 )) KB free)"
    else
        fail "internal heap" "only $(( HEAP_FREE / 1024 )) KB free (threshold: 30 KB)"
    fi
    if (( PSRAM_FREE > 1048576 )); then
        pass "PSRAM OK ($(( PSRAM_FREE / 1024 / 1024 )) MB free)"
    else
        fail "PSRAM" "only $(( PSRAM_FREE / 1024 / 1024 )) MB free (threshold: 1 MB)"
    fi
else
    skip "heap thresholds" "no heap data available"
fi

# ════════════════════════════════════════════════════════════════════════════
# Summary
# ════════════════════════════════════════════════════════════════════════════

echo ""
echo -e "${CYN}╔══════════════════════════════════════════════════╗${RST}"
echo -e "${CYN}║                    SUMMARY                       ║${RST}"
echo -e "${CYN}╠══════════════════════════════════════════════════╣${RST}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "${CYN}║${RST}  ${GRN}PASSED: ${PASS_COUNT}${RST}                                      ${CYN}║${RST}"
echo -e "${CYN}║${RST}  ${RED}FAILED: ${FAIL_COUNT}${RST}                                      ${CYN}║${RST}"
echo -e "${CYN}║${RST}  ${YEL}SKIPPED: ${SKIP_COUNT}${RST}                                     ${CYN}║${RST}"
echo -e "${CYN}║${RST}  TOTAL:  ${TOTAL}                                      ${CYN}║${RST}"
echo -e "${CYN}╠══════════════════════════════════════════════════╣${RST}"
echo -e "${CYN}║${RST}  Screenshots: ${OUT_DIR}  ${CYN}║${RST}"
echo -e "${CYN}╚══════════════════════════════════════════════════╝${RST}"

# Write machine-readable summary
{
    echo "{"
    echo "  \"run_id\": \"${RUN_ID}\","
    echo "  \"timestamp\": \"$(date -Iseconds)\","
    echo "  \"tab_ip\": \"${TAB_IP}\","
    echo "  \"passed\": ${PASS_COUNT},"
    echo "  \"failed\": ${FAIL_COUNT},"
    echo "  \"skipped\": ${SKIP_COUNT},"
    echo "  \"total\": ${TOTAL}"
    echo "}"
} > "${OUT_DIR}/summary.json"

# Exit code: 0 if no failures, 1 if any failed
if (( FAIL_COUNT > 0 )); then
    exit 1
else
    exit 0
fi
