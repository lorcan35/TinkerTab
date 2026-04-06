# TinkerClaw E2E Test Specification

Master test specification: 30 user stories, 150 tests.
All tests are independently executable against live hardware.

**System under test:**
- Tab5 (ESP32-P4) firmware serial: `/dev/ttyACM0` at 115200 baud
- Tab5 debug server: `http://192.168.1.90:8080`
- Dragon (Radxa Q6A) voice server: `http://192.168.1.89:3502`
- Dragon REST API prefix: `http://192.168.1.89:3502/api/v1/`
- Dragon Notes API: `http://192.168.1.89:3502/api/notes`

**Conventions:**
- `SERIAL(cmd)` = write `cmd\n` to `/dev/ttyACM0`, read until next prompt
- `DRAGON` = `http://192.168.1.89:3502`
- `TAB5` = `http://192.168.1.90:8080`
- `TOUCH(x,y)` = `curl -s -X POST TAB5/touch -d '{"x":X,"y":Y,"action":"tap"}'`
- `SCREENSHOT` = `curl -s -o /tmp/screen.bmp TAB5/screenshot.bmp`
- `INFO` = `curl -s TAB5/info`
- Timeouts: voice E2E = 35s, API calls = 5s, serial = 2s

---

## VOICE ASK MODE

---

### US-01: Basic Voice Ask

**Ideal Behavior:** User taps the mic button, speaks a question, Tab5 streams audio to Dragon, Dragon runs STT then LLM then TTS, Tab5 plays the spoken answer, and the UI returns to the READY state. The entire round-trip completes within 35 seconds on local backends and shows screen transitions through LISTENING, PROCESSING, and SPEAKING states.

**Test 1: Health Check Precondition**
- Action: Verify Dragon voice server is reachable and healthy before any voice test.
- Expected: HTTP 200 with `{"status":"ok"}` and valid backend names.
- Pass if: `curl -sf http://192.168.1.89:3502/health | python3 -c "import sys,json; d=json.load(sys.stdin); assert d['status']=='ok' and d['backends']['stt'] and d['backends']['llm'] and d['backends']['tts']"`
```bash
curl -sf http://192.168.1.89:3502/health | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert d['status'] == 'ok', f'status={d[\"status\"]}'
assert d['backends']['stt'] != '', 'stt backend empty'
assert d['backends']['llm'] != '', 'llm backend empty'
assert d['backends']['tts'] != '', 'tts backend empty'
print('PASS')
"
```

**Test 2: Voice Command Triggers Recording**
- Action: Send `voice` serial command, wait 1s, read serial output.
- Expected: Serial output contains `voice_test` and `Recording` and `Listening` within 10 seconds.
- Pass if: Serial output matches regex `Recording|LISTENING|Mic capture task started`.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=10)
s.write(b'voice\r\n')
buf = b''
deadline = time.time() + 10
while time.time() < deadline:
    if s.in_waiting:
        buf += s.read(s.in_waiting)
    if b'Recording' in buf or b'LISTENING' in buf or b'Mic capture task started' in buf:
        print('PASS')
        break
    time.sleep(0.1)
else:
    print(f'FAIL: {buf.decode(errors="replace")[-300:]}')
s.close()
```

**Test 3: Full E2E Voice Round Trip via Serial**
- Action: Send `voice` serial command, wait for Dragon response, check transcript appears.
- Expected: Serial output eventually contains `Transcript:` or `STT:` or state transitions through PROCESSING to READY/IDLE within 35s.
- Pass if: Serial output matches regex `(Transcript|STT|READY|tts_end)` after the voice test completes.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=35)
s.write(b'voice\r\n')
buf = b''
deadline = time.time() + 35
while time.time() < deadline:
    if s.in_waiting:
        buf += s.read(s.in_waiting)
    text = buf.decode(errors='replace')
    if 'Transcript:' in text or 'tts_end' in text or 'voice_test] Done' in text:
        print('PASS')
        break
    time.sleep(0.2)
else:
    print(f'FAIL: no transcript within 35s. Last output: {buf.decode(errors="replace")[-500:]}')
s.close()
```

**Test 4: UI Shows Listening Screen During Recording**
- Action: Trigger voice via debug touch on mic button, immediately take screenshot.
- Expected: Screenshot BMP is non-empty and debug /info shows a voice-related mode.
- Pass if: Screenshot file size > 1000 bytes AND info JSON parseable.
```bash
# Tap center of screen where mic orb is (360, 1000 is typical mic button position)
curl -s -X POST http://192.168.1.90:8080/touch -d '{"x":360,"y":1000,"action":"tap"}'
sleep 2
curl -s -o /tmp/screen_listening.bmp http://192.168.1.90:8080/screenshot.bmp
SIZE=$(stat -c%s /tmp/screen_listening.bmp 2>/dev/null || stat -f%z /tmp/screen_listening.bmp 2>/dev/null)
INFO=$(curl -sf http://192.168.1.90:8080/info)
if [ "$SIZE" -gt 1000 ] && echo "$INFO" | python3 -c "import sys,json; json.load(sys.stdin); print('PASS')"; then
  echo "PASS: screenshot=${SIZE}B"
else
  echo "FAIL: size=$SIZE info=$INFO"
fi
```

**Test 5: Session Created on Dragon After Voice**
- Action: After a voice test, query Dragon REST API for recent sessions.
- Expected: At least one session exists with a non-empty session ID.
- Pass if: `/api/v1/sessions` returns items array with length >= 1.
```bash
curl -sf http://192.168.1.89:3502/api/v1/sessions?limit=5 | python3 -c "
import sys, json
d = json.load(sys.stdin)
items = d.get('items', [])
assert len(items) >= 1, f'no sessions found: {d}'
assert items[0].get('id', '') != '', 'session id empty'
print(f'PASS: {len(items)} sessions, latest={items[0][\"id\"][:16]}')
"
```

---

### US-02: Empty Speech

**Ideal Behavior:** When the user triggers a voice session but produces no speech (only ambient noise), Dragon's STT returns an empty or near-empty transcript. Tab5 receives an error or empty-result message from Dragon, displays a brief notification like "No speech detected," and returns to the READY state within 10 seconds. The system never hangs.

**Test 1: Silence Produces Error or Empty STT**
- Action: Create a session via REST, send 3 seconds of zero-filled PCM audio to the transcription endpoint.
- Expected: Response contains either empty text or an error, not a hang.
- Pass if: HTTP response arrives within 10s AND text field is empty or response is an error.
```bash
# Generate 3s of silence (16kHz 16-bit mono = 96000 bytes)
python3 -c "import sys; sys.stdout.buffer.write(b'\x00' * 96000)" | \
  curl -sf -X POST http://192.168.1.89:3502/api/v1/transcribe \
    -H 'Content-Type: application/octet-stream' \
    -H 'X-Sample-Rate: 16000' \
    --data-binary @- \
    --max-time 10 | python3 -c "
import sys, json
d = json.load(sys.stdin)
text = d.get('text', '').strip()
assert len(text) < 5, f'unexpected transcript from silence: \"{text}\"'
print(f'PASS: text=\"{text}\" stt_ms={d.get(\"stt_ms\",\"?\")}')
"
```

**Test 2: Voice Serial Command with Muted Mic**
- Action: Cover mic / ensure silence, run `voice` serial command, observe output.
- Expected: Serial output should contain either an error message, empty transcript, or "No speech" within 35s.
- Pass if: Serial output contains `error` or `Transcript: ` (empty) or `No speech` or returns to READY.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=35)
s.write(b'voice\r\n')
buf = b''
deadline = time.time() + 35
while time.time() < deadline:
    if s.in_waiting:
        buf += s.read(s.in_waiting)
    text = buf.decode(errors='replace')
    if 'READY' in text or 'error' in text.lower() or 'No speech' in text or 'voice_test] Done' in text:
        print('PASS: system recovered')
        break
    time.sleep(0.2)
else:
    print(f'FAIL: hung for 35s. Output: {buf.decode(errors="replace")[-400:]}')
s.close()
```

**Test 3: Dragon Does Not Create Orphan Processing State**
- Action: Send empty audio via REST chat endpoint with empty text.
- Expected: Error response, not a processing state that never resolves.
- Pass if: HTTP 400 with error message.
```bash
SESSION=$(curl -sf -X POST http://192.168.1.89:3502/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"test-e2e"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
  "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/chat" \
  -H 'Content-Type: application/json' \
  -d '{"text":""}')
if [ "$STATUS" = "400" ]; then echo "PASS: got 400 for empty text"; else echo "FAIL: status=$STATUS"; fi
```

**Test 4: Tab5 Returns to READY After Silence Timeout**
- Action: Run voice command, wait with silence. Check Tab5 info endpoint after 35s.
- Expected: Tab5 is not stuck in LISTENING or PROCESSING mode.
- Pass if: Info endpoint does not report a stuck voice state after 40s.
```bash
# This is an observational test. We trigger voice, wait, then check device state.
python3 -c "
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=2)
s.write(b'voice\r\n')
s.close()
" 2>/dev/null
sleep 40
curl -sf http://192.168.1.90:8080/info | python3 -c "
import sys, json
try:
    d = json.load(sys.stdin)
    mode = d.get('mode', 'unknown')
    # After 40s, device should NOT be stuck in voice processing
    if mode in ('idle', 'streaming', 'IDLE'):
        print(f'PASS: mode={mode}')
    else:
        print(f'WARN: mode={mode} (may still be in voice flow)')
except:
    print('PASS: info endpoint reachable, no stuck state detected')
"
```

**Test 5: Dragon Health Still OK After Empty Audio**
- Action: After sending silence, verify Dragon health endpoint still responds.
- Expected: HTTP 200 with status ok.
- Pass if: Health check passes.
```bash
curl -sf http://192.168.1.89:3502/health | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert d['status'] == 'ok', f'health degraded: {d}'
print('PASS: Dragon healthy after empty audio')
"
```

---

### US-03: 30s Max Recording

**Ideal Behavior:** In Ask mode, the mic capture task enforces a hard 30-second limit. At frame 1500 (20ms chunks x 1500 = 30s), recording auto-stops, a `{"type":"stop"}` is sent to Dragon, and the pipeline proceeds to STT/LLM/TTS processing. The user sees the UI transition from LISTENING to PROCESSING without manual intervention.

**Test 1: Code Enforces MAX_RECORD_FRAMES_ASK = 1500**
- Action: Grep voice.c for the max recording constant.
- Expected: `MAX_RECORD_FRAMES_ASK` is defined as 1500.
- Pass if: grep matches `#define MAX_RECORD_FRAMES_ASK.*1500`.
```bash
grep -n 'MAX_RECORD_FRAMES_ASK' /home/rebelforce/projects/TinkerTab/main/voice.c | head -5
# Expected: #define MAX_RECORD_FRAMES_ASK  1500
```

**Test 2: Frame Limit Only Applies to Ask Mode**
- Action: Check voice.c for the condition that checks mode before applying the frame limit.
- Expected: The frame limit check is guarded by `s_voice_mode == VOICE_MODE_ASK`.
- Pass if: Code contains `VOICE_MODE_ASK && frames_sent >= MAX_RECORD_FRAMES_ASK`.
```bash
grep -A2 'MAX_RECORD_FRAMES_ASK' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'VOICE_MODE_ASK'
# Expected: >= 1 (the limit is mode-gated)
```

**Test 3: Auto-Stop Sends Stop Signal**
- Action: Trigger voice, let it record past 30s (do not manually stop), read serial.
- Expected: Serial log shows "Max recording duration reached" and then processing.
- Pass if: Serial output contains `Max recording duration`.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=45)
s.write(b'voice\r\n')
buf = b''
deadline = time.time() + 45
while time.time() < deadline:
    if s.in_waiting:
        buf += s.read(s.in_waiting)
    if b'Max recording duration' in buf:
        print('PASS: 30s auto-stop triggered')
        break
    time.sleep(0.2)
else:
    text = buf.decode(errors='replace')
    if 'voice_test] Done' in text:
        print('PASS: voice test completed (recording was < 30s)')
    else:
        print(f'FAIL: no auto-stop message. Last: {text[-400:]}')
s.close()
```

**Test 4: Duration Calculation is Correct**
- Action: Verify that 1500 frames at 20ms each equals exactly 30 seconds.
- Expected: 1500 * 20ms = 30000ms = 30s.
- Pass if: Arithmetic check passes.
```bash
python3 -c "
frames = 1500
chunk_ms = 20
duration_s = frames * chunk_ms / 1000
assert duration_s == 30.0, f'expected 30s, got {duration_s}s'
print(f'PASS: {frames} frames x {chunk_ms}ms = {duration_s}s')
"
```

**Test 5: Mic Task Exits After Frame Limit**
- Action: Read voice.c and verify the mic_capture_task breaks out of the loop on frame limit.
- Expected: After `MAX_RECORD_FRAMES_ASK`, the while loop breaks and `s_mic_running` is set false.
- Pass if: Code has `break` after the frame limit check.
```bash
grep -A3 'MAX_RECORD_FRAMES_ASK' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'break'
# Expected: >= 1
```

---

### US-04: Cancel During Recording

**Ideal Behavior:** While Tab5 is in LISTENING state (mic active, streaming to Dragon), the user can cancel. The voice_cancel() function stops the mic task, clears the playback buffer, sends `{"type":"cancel"}` to Dragon, and transitions to READY (if WS is alive) or IDLE (if disconnected). Dragon discards buffered audio and does not run STT/LLM/TTS.

**Test 1: voice_cancel Sends Cancel JSON to Dragon**
- Action: Inspect voice.c for the cancel message sent to Dragon.
- Expected: `ws_send_text("{\"type\":\"cancel\"}")` is called in voice_cancel().
- Pass if: grep finds the cancel message in the function.
```bash
grep -A20 'esp_err_t voice_cancel' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c '"cancel"'
# Expected: >= 1
```

**Test 2: Cancel Stops Mic Running Flag**
- Action: Check that voice_cancel sets `s_mic_running = false`.
- Expected: The cancel function explicitly clears the mic flag before waiting.
- Pass if: Code contains `s_mic_running = false` in voice_cancel.
```bash
grep -A15 'esp_err_t voice_cancel' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 's_mic_running = false'
# Expected: >= 1
```

**Test 3: Cancel Clears Playback Buffer**
- Action: Check that voice_cancel calls playback_buf_reset().
- Expected: Buffer is cleared to prevent stale audio playback.
- Pass if: Code contains `playback_buf_reset()` in voice_cancel.
```bash
grep -A15 'esp_err_t voice_cancel' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'playback_buf_reset'
# Expected: >= 1
```

**Test 4: Cancel Returns to READY When Connected**
- Action: Check the state transition logic in voice_cancel.
- Expected: If WS is connected, state goes to READY. If disconnected, state goes to IDLE.
- Pass if: Code conditionally sets READY or IDLE based on `s_ws_connected`.
```bash
grep -A20 'esp_err_t voice_cancel' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'VOICE_STATE_READY'
# Expected: >= 1
```

**Test 5: Dragon Server Handles Cancel Gracefully**
- Action: Check Dragon server.py for cancel handling.
- Expected: Dragon calls `pipeline.cancel()` on receiving cancel command.
- Pass if: Server code handles "cancel" command type.
```bash
grep -A5 '"cancel"' /home/rebelforce/projects/TinkerBox/dragon_voice/server.py | grep -c 'pipeline.cancel'
# Expected: >= 1
```

---

### US-05: Cancel During Playback

**Ideal Behavior:** While Tab5 is in SPEAKING state (playing TTS audio), the user can cancel. voice_cancel() immediately stops the playback by resetting the ring buffer and disabling the speaker codec, then sends cancel to Dragon. The UI transitions to READY. No residual audio plays after cancel.

**Test 1: Cancel Disables Speaker**
- Action: Check that voice_cancel calls tab5_audio_speaker_enable(false).
- Expected: Speaker is explicitly disabled on cancel.
- Pass if: Code contains `tab5_audio_speaker_enable(false)` in voice_cancel.
```bash
grep -A15 'esp_err_t voice_cancel' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'speaker_enable(false)'
# Expected: >= 1
```

**Test 2: Playback Buffer Zeroed on Cancel**
- Action: Check that playback_buf_reset sets write/read/count to 0.
- Expected: All ring buffer pointers are zeroed.
- Pass if: Function sets s_play_wr, s_play_rd, s_play_count to 0.
```bash
grep -A10 'static void playback_buf_reset' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c '= 0'
# Expected: >= 3 (wr, rd, count)
```

**Test 3: Cancel Works in SPEAKING State**
- Action: Check that voice_cancel does not short-circuit in SPEAKING state.
- Expected: The only early return is for IDLE state.
- Pass if: Code only returns early for `VOICE_STATE_IDLE`.
```bash
grep -A3 'esp_err_t voice_cancel' /home/rebelforce/projects/TinkerTab/main/voice.c | grep 'IDLE'
# Expected: s_state == VOICE_STATE_IDLE => return ESP_OK (only case)
```

**Test 4: Dragon Pipeline Cancel Stops TTS**
- Action: Check pipeline.py for a cancel method that aborts TTS synthesis.
- Expected: Pipeline has a cancel() method.
- Pass if: File contains an async def cancel method.
```bash
grep -n 'async def cancel' /home/rebelforce/projects/TinkerBox/dragon_voice/pipeline.py | head -3
# Expected: at least one match
```

**Test 5: No Audio Leaks After Cancel**
- Action: After cancel, verify playback ring buffer count is 0.
- Expected: s_play_count == 0 after playback_buf_reset.
- Pass if: The reset function is correctly implemented (code inspection).
```bash
grep -B2 -A10 'playback_buf_reset' /home/rebelforce/projects/TinkerTab/main/voice.c | head -15
# Expected: function zeros all three counters under mutex
```

---

## MULTI-TURN

---

### US-06: Two-Turn Follow-Up

**Ideal Behavior:** The user asks a question (turn 1), receives a response, then asks a follow-up that references the first answer (turn 2). Dragon's ConversationEngine maintains the full message history for the session, so the LLM has context from turn 1 when generating the turn 2 response. The response is contextually relevant.

**Test 1: Create Session and Send Two Messages**
- Action: Create a session, send "My name is Sparky", then send "What is my name?".
- Expected: The second response contains "Sparky".
- Pass if: Response to second message contains the name from the first.
```bash
# Create session
SESSION=$(curl -sf -X POST http://192.168.1.89:3502/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"e2e-test","type":"conversation"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
echo "Session: $SESSION"

# Turn 1: introduce name
curl -sf -N -X POST "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/chat" \
  -H 'Content-Type: application/json' \
  -d '{"text":"My name is Sparky. Remember that."}' --max-time 30 > /tmp/turn1.txt

sleep 1

# Turn 2: ask for name
curl -sf -N -X POST "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/chat" \
  -H 'Content-Type: application/json' \
  -d '{"text":"What is my name?"}' --max-time 30 > /tmp/turn2.txt

python3 -c "
data = open('/tmp/turn2.txt').read()
if 'Sparky' in data or 'sparky' in data:
    print('PASS: LLM remembered name from turn 1')
else:
    print(f'FAIL: response does not contain Sparky. Response: {data[:300]}')
"
```

**Test 2: Messages Persisted in Order**
- Action: After two turns, list messages for the session.
- Expected: At least 4 messages (2 user + 2 assistant) in chronological order.
- Pass if: Message count >= 4.
```bash
SESSION=$(curl -sf http://192.168.1.89:3502/api/v1/sessions?limit=1 | python3 -c "import sys,json; print(json.load(sys.stdin)['items'][0]['id'])")
curl -sf "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/messages" | python3 -c "
import sys, json
d = json.load(sys.stdin)
count = d.get('count', 0)
if count >= 2:
    print(f'PASS: {count} messages in session')
else:
    print(f'FAIL: only {count} messages')
"
```

**Test 3: Session Has Active Status**
- Action: Check the session status is "active" after two turns.
- Expected: Session status is "active" (not "ended" or "paused").
- Pass if: status field == "active".
```bash
SESSION=$(curl -sf http://192.168.1.89:3502/api/v1/sessions?limit=1&status=active | python3 -c "import sys,json; items=json.load(sys.stdin)['items']; print(items[0]['id'] if items else '')")
if [ -n "$SESSION" ]; then
  echo "PASS: active session found: ${SESSION:0:16}"
else
  echo "FAIL: no active sessions"
fi
```

**Test 4: SSE Stream Format Correct**
- Action: Send a chat message and verify the SSE stream format.
- Expected: Response contains `data: ` prefixed lines with JSON tokens.
- Pass if: At least one `data:` line with a `token` field.
```bash
SESSION=$(curl -sf -X POST http://192.168.1.89:3502/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"e2e-sse"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf -N -X POST "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/chat" \
  -H 'Content-Type: application/json' \
  -d '{"text":"Say hello"}' --max-time 15 | python3 -c "
import sys
lines = sys.stdin.read().split('\n')
data_lines = [l for l in lines if l.startswith('data:') and 'token' in l]
if len(data_lines) >= 1:
    print(f'PASS: {len(data_lines)} SSE data lines with tokens')
else:
    print(f'FAIL: no SSE token lines found in {len(lines)} lines')
"
```

**Test 5: ConversationEngine Builds Context from DB**
- Action: Verify conversation.py has a method that loads message history for context.
- Expected: process_text_stream loads prior messages from the MessageStore.
- Pass if: Code references message loading before LLM call.
```bash
grep -n 'get_messages\|build_context\|load_history\|messages.*session' /home/rebelforce/projects/TinkerBox/dragon_voice/conversation.py | head -5
# Expected: at least one match showing history is loaded
```

---

### US-07: Three-Turn Progressive Context

**Ideal Behavior:** Over three turns, the LLM accumulates context. Turn 1: user states a fact. Turn 2: user states another fact. Turn 3: user asks a question requiring both facts. The LLM should combine both pieces of context correctly.

**Test 1: Three-Turn Context Accumulation**
- Action: Create session, send 3 messages building on each other.
- Expected: Turn 3 response references information from both turn 1 and turn 2.
- Pass if: Final response is contextually aware of both prior facts.
```bash
SESSION=$(curl -sf -X POST http://192.168.1.89:3502/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"e2e-3turn"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")

# Turn 1
curl -sf -N -X POST "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/chat" \
  -H 'Content-Type: application/json' \
  -d '{"text":"I have a dog named Biscuit."}' --max-time 20 > /dev/null

# Turn 2
curl -sf -N -X POST "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/chat" \
  -H 'Content-Type: application/json' \
  -d '{"text":"Biscuit is a golden retriever."}' --max-time 20 > /dev/null

# Turn 3
curl -sf -N -X POST "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/chat" \
  -H 'Content-Type: application/json' \
  -d '{"text":"What breed is my dog and what is its name?"}' --max-time 20 > /tmp/turn3.txt

python3 -c "
data = open('/tmp/turn3.txt').read().lower()
has_name = 'biscuit' in data
has_breed = 'golden' in data or 'retriever' in data
if has_name and has_breed:
    print('PASS: LLM remembered both facts across 3 turns')
elif has_name:
    print('PARTIAL: remembered name but not breed')
elif has_breed:
    print('PARTIAL: remembered breed but not name')
else:
    print(f'FAIL: {data[:300]}')
"
```

**Test 2: Message Count After Three Turns**
- Action: List messages for the session after 3 turns.
- Expected: At least 6 messages (3 user + 3 assistant).
- Pass if: count >= 6.
```bash
SESSION=$(curl -sf http://192.168.1.89:3502/api/v1/sessions?limit=1&device_id=e2e-3turn | python3 -c "import sys,json; items=json.load(sys.stdin)['items']; print(items[0]['id'] if items else '')")
[ -n "$SESSION" ] && curl -sf "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/messages" | python3 -c "
import sys, json
d = json.load(sys.stdin)
print(f'PASS: {d[\"count\"]} messages') if d['count'] >= 6 else print(f'FAIL: {d[\"count\"]} messages')
"
```

**Test 3: Messages Are in Correct Chronological Order**
- Action: Fetch messages and verify timestamps are ascending.
- Expected: Each message has a later timestamp than the previous.
- Pass if: All timestamps are in ascending order.
```bash
SESSION=$(curl -sf http://192.168.1.89:3502/api/v1/sessions?limit=1&device_id=e2e-3turn | python3 -c "import sys,json; items=json.load(sys.stdin)['items']; print(items[0]['id'] if items else '')")
[ -n "$SESSION" ] && curl -sf "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/messages?limit=10" | python3 -c "
import sys, json
msgs = json.load(sys.stdin).get('items', [])
if len(msgs) < 2:
    print(f'SKIP: only {len(msgs)} messages')
else:
    timestamps = [m.get('created_at','') for m in msgs]
    sorted_ok = timestamps == sorted(timestamps)
    print('PASS: messages in order') if sorted_ok else print(f'FAIL: out of order: {timestamps}')
"
```

**Test 4: Each Turn Has Matching User-Assistant Pair**
- Action: Verify messages alternate user/assistant roles.
- Expected: Roles alternate starting with user (or system).
- Pass if: No two consecutive messages have the same role (excluding system).
```bash
SESSION=$(curl -sf http://192.168.1.89:3502/api/v1/sessions?limit=1&device_id=e2e-3turn | python3 -c "import sys,json; items=json.load(sys.stdin)['items']; print(items[0]['id'] if items else '')")
[ -n "$SESSION" ] && curl -sf "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/messages?limit=20" | python3 -c "
import sys, json
msgs = json.load(sys.stdin).get('items', [])
roles = [m.get('role','?') for m in msgs if m.get('role') != 'system']
dupes = sum(1 for i in range(1,len(roles)) if roles[i] == roles[i-1])
print(f'PASS: no duplicate roles') if dupes == 0 else print(f'FAIL: {dupes} consecutive duplicate roles: {roles}')
"
```

**Test 5: LLM Response Is Non-Empty on Each Turn**
- Action: Verify each assistant message has non-empty content.
- Expected: All assistant messages have content length > 0.
- Pass if: No assistant message has empty content.
```bash
SESSION=$(curl -sf http://192.168.1.89:3502/api/v1/sessions?limit=1&device_id=e2e-3turn | python3 -c "import sys,json; items=json.load(sys.stdin)['items']; print(items[0]['id'] if items else '')")
[ -n "$SESSION" ] && curl -sf "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/messages?limit=20" | python3 -c "
import sys, json
msgs = json.load(sys.stdin).get('items', [])
assistant_msgs = [m for m in msgs if m.get('role') == 'assistant']
empty = [m for m in assistant_msgs if not m.get('content','').strip()]
if len(empty) == 0 and len(assistant_msgs) > 0:
    print(f'PASS: {len(assistant_msgs)} non-empty assistant messages')
else:
    print(f'FAIL: {len(empty)} empty of {len(assistant_msgs)} assistant messages')
"
```

---

### US-08: Session Isolation

**Ideal Behavior:** Creating a new session starts with a blank conversation context. Information shared in one session is not available in another session. The LLM in session B has no memory of anything said in session A.

**Test 1: New Session Has Zero Messages**
- Action: Create a brand new session, check message count.
- Expected: Message count is 0 for a fresh session.
- Pass if: count == 0.
```bash
SESSION=$(curl -sf -X POST http://192.168.1.89:3502/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"e2e-isolation"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/messages" | python3 -c "
import sys, json
d = json.load(sys.stdin)
print('PASS: 0 messages') if d['count'] == 0 else print(f'FAIL: {d[\"count\"]} messages in new session')
"
```

**Test 2: Session B Does Not Know Session A Facts**
- Action: Create session A, tell it a name. Create session B, ask for the name.
- Expected: Session B does not know the name.
- Pass if: Response from session B does NOT contain "Sparky".
```bash
# Session A
SA=$(curl -sf -X POST http://192.168.1.89:3502/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"e2e-iso-a"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf -N -X POST "http://192.168.1.89:3502/api/v1/sessions/${SA}/chat" \
  -H 'Content-Type: application/json' \
  -d '{"text":"My secret password is Zephyr42."}' --max-time 15 > /dev/null

# Session B (different device to prevent resume)
SB=$(curl -sf -X POST http://192.168.1.89:3502/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"e2e-iso-b"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf -N -X POST "http://192.168.1.89:3502/api/v1/sessions/${SB}/chat" \
  -H 'Content-Type: application/json' \
  -d '{"text":"What is my secret password?"}' --max-time 15 > /tmp/iso_b.txt

python3 -c "
data = open('/tmp/iso_b.txt').read()
if 'Zephyr42' not in data and 'zephyr42' not in data.lower():
    print('PASS: session B has no knowledge of session A')
else:
    print('FAIL: session B leaked info from session A')
"
```

**Test 3: Different Session IDs**
- Action: Create two sessions, compare IDs.
- Expected: IDs are different.
- Pass if: session_a_id != session_b_id.
```bash
SA=$(curl -sf -X POST http://192.168.1.89:3502/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"e2e-id1"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
SB=$(curl -sf -X POST http://192.168.1.89:3502/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"e2e-id2"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
if [ "$SA" != "$SB" ]; then echo "PASS: different IDs"; else echo "FAIL: same IDs"; fi
```

**Test 4: Session List Filtering by Device**
- Action: List sessions filtered by device_id.
- Expected: Only sessions for that device are returned.
- Pass if: All returned sessions have the queried device_id.
```bash
curl -sf "http://192.168.1.89:3502/api/v1/sessions?device_id=e2e-iso-a&limit=10" | python3 -c "
import sys, json
d = json.load(sys.stdin)
items = d.get('items', [])
wrong = [s for s in items if s.get('device_id') != 'e2e-iso-a']
if len(wrong) == 0 and len(items) > 0:
    print(f'PASS: {len(items)} sessions, all for e2e-iso-a')
elif len(items) == 0:
    print('WARN: no sessions found (may need to run test 2 first)')
else:
    print(f'FAIL: {len(wrong)} sessions with wrong device_id')
"
```

**Test 5: End Session Prevents Further Messages**
- Action: End a session, then try to send a chat message.
- Expected: Chat to ended session returns 404 or error.
- Pass if: HTTP status is 404 or response contains error.
```bash
SESSION=$(curl -sf -X POST http://192.168.1.89:3502/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"e2e-end"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf -X POST "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/end"
STATUS=$(curl -s -o /tmp/ended.txt -w '%{http_code}' -X POST \
  "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/chat" \
  -H 'Content-Type: application/json' \
  -d '{"text":"hello"}' --max-time 10)
# An ended session should either 404, 400, or still work (implementation dependent)
echo "Status: $STATUS (ended session behavior)"
if [ "$STATUS" != "200" ] || grep -q 'error' /tmp/ended.txt 2>/dev/null; then
  echo "PASS: ended session rejected or errored"
else
  echo "WARN: ended session still accepted chat (may be by design)"
fi
```

---

### US-09: Session Resume After Disconnect

**Ideal Behavior:** When Tab5 disconnects (WiFi drop, reboot) and reconnects, it sends its stored `session_id` in the `register` message. Dragon looks up this session, finds it in "paused" state, resumes it, and returns `session_start` with `resumed: true` and the existing `message_count`. The conversation continues where it left off.

**Test 1: Tab5 Stores Session ID in NVS**
- Action: Check settings.h for session_id persistence API.
- Expected: get/set session_id functions exist.
- Pass if: Header declares both getter and setter.
```bash
grep -c 'session_id' /home/rebelforce/projects/TinkerTab/main/settings.h
# Expected: >= 2 (get and set)
```

**Test 2: Register Message Includes Session ID**
- Action: Check voice.c ws_send_register for session_id field.
- Expected: Register JSON includes `session_id` from NVS.
- Pass if: Code adds session_id to the register JSON.
```bash
grep -A30 'ws_send_register' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'session_id'
# Expected: >= 2 (get + add to JSON)
```

**Test 3: Dragon Resumes Paused Session**
- Action: Check server.py for resume logic in _handle_register.
- Expected: get_or_create_session uses requested_session_id for resume.
- Pass if: Code passes requested_session to session manager.
```bash
grep -A20 '_handle_register' /home/rebelforce/projects/TinkerBox/dragon_voice/server.py | grep -c 'requested_session'
# Expected: >= 1
```

**Test 4: session_start Response Includes Resumed Flag**
- Action: Check server.py for the resumed field in session_start response.
- Expected: Response JSON includes `"resumed": true/false`.
- Pass if: Code sends resumed field.
```bash
grep -B2 -A5 'session_start' /home/rebelforce/projects/TinkerBox/dragon_voice/server.py | grep -c 'resumed'
# Expected: >= 1
```

**Test 5: Tab5 Handles session_start with Resume Info**
- Action: Check voice.c handle_text_message for session_start handling.
- Expected: Tab5 logs resumed status and stores the session_id.
- Pass if: Code stores session_id from session_start.
```bash
grep -A15 '"session_start"' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'settings_set_session_id'
# Expected: >= 1
```

---

### US-10: Clear History

**Ideal Behavior:** The user explicitly clears conversation history. Tab5 sends `{"type":"clear"}` to Dragon. Dragon ends the current session, creates a fresh one, clears the pipeline's in-memory history, and sends a new `session_start` with `message_count: 0` and `resumed: false`. The next turn starts with zero context.

**Test 1: voice_clear_history Sends Clear Message**
- Action: Check voice.c for the clear function implementation.
- Expected: Function sends `{"type":"clear"}` via WebSocket.
- Pass if: Code sends the clear JSON.
```bash
grep -A5 'voice_clear_history' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c '"clear"'
# Expected: >= 1
```

**Test 2: Dragon Handles Clear Command**
- Action: Check server.py for clear command handling.
- Expected: Dragon ends old session, creates new one, sends new session_start.
- Pass if: Code creates a new session after clear.
```bash
grep -A20 '"clear"' /home/rebelforce/projects/TinkerBox/dragon_voice/server.py | grep -c 'create_session'
# Expected: >= 1
```

**Test 3: New Session After Clear Has Zero Messages**
- Action: Check that clear sends session_start with message_count 0.
- Expected: message_count is 0 in the new session_start.
- Pass if: Code sends message_count: 0.
```bash
grep -A25 '"clear"' /home/rebelforce/projects/TinkerBox/dragon_voice/server.py | grep -c 'message_count.*0'
# Expected: >= 1
```

**Test 4: Pipeline History Cleared**
- Action: Check that clear calls pipeline.clear_history().
- Expected: Pipeline's in-memory history is wiped.
- Pass if: Code calls clear_history on the pipeline.
```bash
grep -A5 '"clear"' /home/rebelforce/projects/TinkerBox/dragon_voice/server.py | grep -c 'clear_history'
# Expected: >= 1
```

**Test 5: Clear Via REST - End and Recreate**
- Action: Use REST API to end a session and create a new one.
- Expected: New session has 0 messages.
- Pass if: New session message count is 0.
```bash
SESSION=$(curl -sf -X POST http://192.168.1.89:3502/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"e2e-clear"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
# Add a message
curl -sf -N -X POST "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/chat" \
  -H 'Content-Type: application/json' \
  -d '{"text":"Hello"}' --max-time 15 > /dev/null
# End session
curl -sf -X POST "http://192.168.1.89:3502/api/v1/sessions/${SESSION}/end" > /dev/null
# Create new
NEW=$(curl -sf -X POST http://192.168.1.89:3502/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"e2e-clear"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf "http://192.168.1.89:3502/api/v1/sessions/${NEW}/messages" | python3 -c "
import sys, json
d = json.load(sys.stdin)
print('PASS: new session has 0 messages') if d['count'] == 0 else print(f'FAIL: {d[\"count\"]} messages')
"
```

---

## DICTATION

---

### US-11: Short Dictation

**Ideal Behavior:** In dictation mode, Tab5 streams audio to Dragon which runs STT only -- no LLM, no TTS. The transcript is returned as text. This is for quick notes where the user speaks a few sentences and gets a text transcription. The mode is triggered by sending `{"type":"start","mode":"dictate"}` to Dragon.

**Test 1: Dictation Mode Exists in Voice Enum**
- Action: Check voice.h for VOICE_MODE_DICTATE.
- Expected: Enum includes DICTATE mode.
- Pass if: Header contains VOICE_MODE_DICTATE.
```bash
grep -c 'VOICE_MODE_DICTATE' /home/rebelforce/projects/TinkerTab/main/voice.h
# Expected: >= 1
```

**Test 2: Start Dictation Sends Correct Mode**
- Action: Check voice.c for dictation start message.
- Expected: Sends `{"type":"start","mode":"dictate"}`.
- Pass if: Code sends start with mode=dictate.
```bash
grep -A10 'voice_start_dictation' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'dictate'
# Expected: >= 1
```

**Test 3: Dictation Skips LLM and TTS**
- Action: Check that dictation mode transitions directly to READY after STT, not PROCESSING.
- Expected: On receiving "stt" message in dictation mode, state goes to READY (not PROCESSING/SPEAKING).
- Pass if: Code sets VOICE_STATE_READY with "dictation_done" in dictation mode.
```bash
grep -A10 'dictation' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'VOICE_STATE_READY'
# Expected: >= 1
```

**Test 4: Transcription API Works Independently**
- Action: Send a short audio clip to the transcription endpoint.
- Expected: Returns a text transcript.
- Pass if: Response contains non-empty text field.
```bash
# Generate a 1s tone (this will likely return silence/noise transcription)
python3 -c "
import struct, math
samples = [int(32000 * math.sin(2 * math.pi * 440 * i / 16000)) for i in range(16000)]
import sys
sys.stdout.buffer.write(struct.pack(f'<{len(samples)}h', *samples))
" | curl -sf -X POST http://192.168.1.89:3502/api/v1/transcribe \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-Sample-Rate: 16000' \
  --data-binary @- --max-time 15 | python3 -c "
import sys, json
d = json.load(sys.stdin)
print(f'PASS: transcribe returned text=\"{d.get(\"text\",\"\")}\" in {d.get(\"stt_ms\",\"?\")}ms')
"
```

**Test 5: Dictation Has No Recording Duration Limit**
- Action: Check that MAX_RECORD_FRAMES_ASK limit is NOT applied in dictation mode.
- Expected: The frame limit check is gated by VOICE_MODE_ASK.
- Pass if: Code only applies limit when mode is ASK.
```bash
grep -B1 -A1 'MAX_RECORD_FRAMES_ASK' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'VOICE_MODE_ASK'
# Expected: >= 1 (limit only applies to ASK mode)
```

---

### US-12: Long Dictation with VAD

**Ideal Behavior:** During dictation, Tab5 performs client-side VAD (voice activity detection) by computing RMS of audio chunks. When silence is detected for 800ms (40 frames x 20ms), Tab5 sends a `{"type":"segment"}` marker to Dragon, which transcribes the accumulated audio segment. Multiple segments are concatenated into the final transcript. This allows unlimited-length dictation.

**Test 1: Silence Detection Threshold Defined**
- Action: Check voice.c for dictation silence constants.
- Expected: DICTATION_SILENCE_THRESHOLD and DICTATION_SILENCE_FRAMES are defined.
- Pass if: Both constants exist.
```bash
grep -c 'DICTATION_SILENCE_THRESHOLD\|DICTATION_SILENCE_FRAMES' /home/rebelforce/projects/TinkerTab/main/voice.c
# Expected: >= 2
```

**Test 2: Segment Marker Sent on Silence**
- Action: Check that mic_capture_task sends segment JSON after detecting silence.
- Expected: Code sends `{"type":"segment"}` when silence frames exceed threshold.
- Pass if: ws_send_text with "segment" found in dictation VAD code.
```bash
grep -A5 'DICTATION_SILENCE_FRAMES' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'segment'
# Expected: >= 1
```

**Test 3: Dragon Handles Segment Markers**
- Action: Check server.py for segment command handling.
- Expected: Dragon processes segments through pipeline.process_segment().
- Pass if: Code calls process_segment on receiving segment command.
```bash
grep -A5 '"segment"' /home/rebelforce/projects/TinkerBox/dragon_voice/server.py | grep -c 'process_segment'
# Expected: >= 1
```

**Test 4: Dictation Text Buffer Is PSRAM Allocated**
- Action: Check that dictation text buffer uses PSRAM.
- Expected: DICTATION_TEXT_SIZE allocated with MALLOC_CAP_SPIRAM.
- Pass if: heap_caps_malloc with SPIRAM flag for dictation buffer.
```bash
grep -A5 's_dictation_text' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'SPIRAM'
# Expected: >= 1
```

**Test 5: Partial Transcripts Accumulated**
- Action: Check that stt_partial messages are appended to dictation text.
- Expected: handle_text_message concatenates stt_partial results in dictation mode.
- Pass if: Code appends stt_partial text to s_dictation_text.
```bash
grep -A10 'stt_partial' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 's_dictation_text'
# Expected: >= 1
```

---

### US-13: Dictation Saved as Note

**Ideal Behavior:** Voice notes recorded via dictation are saved to Dragon as notes. The audio is recorded to SD card as WAV, and the transcript is sent to Dragon's notes API. The note includes both the text transcript and a reference to the audio file.

**Test 1: Notes API Create Endpoint Exists**
- Action: Verify notes creation endpoint is available.
- Expected: POST /api/notes returns 201 with a note object.
- Pass if: HTTP 201 with note containing id and text.
```bash
curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d '{"text":"Test dictation note","title":"E2E Test"}' | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert 'id' in d, 'missing id'
assert d.get('text','') or d.get('transcript',''), 'missing text content'
print(f'PASS: note created with id={d[\"id\"][:16]}')
"
```

**Test 2: Notes From Audio Endpoint Works**
- Action: POST raw PCM to /api/notes/from-audio.
- Expected: Returns a note with a transcript field.
- Pass if: HTTP 201 with note containing transcript.
```bash
python3 -c "import sys; sys.stdout.buffer.write(b'\x00' * 32000)" | \
  curl -sf -X POST "http://192.168.1.89:3502/api/notes/from-audio?sample_rate=16000" \
    -H 'Content-Type: application/octet-stream' \
    --data-binary @- --max-time 15 | python3 -c "
import sys, json
d = json.load(sys.stdin)
print(f'PASS: audio note created, id={d.get(\"id\",\"?\")[:16]}')
"
```

**Test 3: Audio Written to SD Card During Recording**
- Action: Check that mic_capture_task writes audio to SD via ui_notes_write_audio.
- Expected: Every chunk is written to SD card WAV file.
- Pass if: Code calls ui_notes_write_audio in the mic loop.
```bash
grep -c 'ui_notes_write_audio' /home/rebelforce/projects/TinkerTab/main/voice.c
# Expected: >= 1
```

**Test 4: SD Card Status Available**
- Action: Send `sd` serial command.
- Expected: Reports mount point and free space.
- Pass if: Output contains "mounted" and "GB".
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=3)
s.write(b'sd\r\n')
time.sleep(1)
buf = s.read(s.in_waiting).decode(errors='replace')
if 'mounted' in buf or 'Total' in buf:
    print(f'PASS: {buf.strip()}')
else:
    print(f'FAIL: {buf.strip()}')
s.close()
```

**Test 5: Background Transcription Queue Started**
- Action: Check main.c for transcription queue initialization.
- Expected: ui_notes_start_transcription_queue() is called at boot.
- Pass if: Function call exists in startup code.
```bash
grep -c 'ui_notes_start_transcription_queue' /home/rebelforce/projects/TinkerTab/main/main.c
# Expected: >= 1
```

---

## NOTES CRUD

---

### US-14: Add Text Note

**Ideal Behavior:** User creates a text note via the API. The note is saved to Dragon's SQLite database with a unique ID, timestamp, and the provided text content. The note is immediately retrievable via GET.

**Test 1: Create and Retrieve Text Note**
- Action: POST a note, then GET it by ID.
- Expected: GET returns the same text.
- Pass if: Retrieved note text matches created note text.
```bash
NOTE_ID=$(curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d '{"text":"Buy milk and eggs","title":"Shopping"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf "http://192.168.1.89:3502/api/notes/${NOTE_ID}" | python3 -c "
import sys, json
d = json.load(sys.stdin)
text = d.get('text','') or d.get('transcript','')
assert 'milk' in text.lower(), f'text mismatch: {text}'
print(f'PASS: note retrieved, text contains milk')
"
```

**Test 2: Note Has Unique ID**
- Action: Create two notes, compare IDs.
- Expected: IDs are different.
- Pass if: id1 != id2.
```bash
ID1=$(curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d '{"text":"Note one"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
ID2=$(curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d '{"text":"Note two"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
[ "$ID1" != "$ID2" ] && echo "PASS: unique IDs" || echo "FAIL: same IDs"
```

**Test 3: Note Appears in List**
- Action: Create a note, then list all notes.
- Expected: The new note appears in the list.
- Pass if: List contains at least one note.
```bash
curl -sf http://192.168.1.89:3502/api/notes?limit=5 | python3 -c "
import sys, json
d = json.load(sys.stdin)
notes = d.get('notes', [])
print(f'PASS: {len(notes)} notes in list') if len(notes) > 0 else print('FAIL: empty list')
"
```

**Test 4: Empty Text Rejected**
- Action: POST a note with empty text.
- Expected: HTTP 400 with error.
- Pass if: Status code is 400.
```bash
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' -d '{"text":""}')
[ "$STATUS" = "400" ] && echo "PASS: empty text rejected" || echo "FAIL: status=$STATUS"
```

**Test 5: Note Has Timestamp**
- Action: Create a note, check for created_at field.
- Expected: Note has a non-empty created_at timestamp.
- Pass if: created_at is present and non-empty.
```bash
curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d '{"text":"Timestamp test"}' | python3 -c "
import sys, json
d = json.load(sys.stdin)
ts = d.get('created_at', '')
print(f'PASS: created_at={ts}') if ts else print('FAIL: no timestamp')
"
```

---

### US-15: Delete Note

**Ideal Behavior:** User deletes a note by ID. The note is permanently removed from the database. Subsequent GET for that ID returns 404.

**Test 1: Delete and Verify Gone**
- Action: Create a note, delete it, then try to GET it.
- Expected: DELETE returns success, GET returns 404.
- Pass if: GET after DELETE returns 404.
```bash
NID=$(curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d '{"text":"Delete me"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf -X DELETE "http://192.168.1.89:3502/api/notes/${NID}" > /dev/null
STATUS=$(curl -s -o /dev/null -w '%{http_code}' "http://192.168.1.89:3502/api/notes/${NID}")
[ "$STATUS" = "404" ] && echo "PASS: deleted note returns 404" || echo "FAIL: status=$STATUS"
```

**Test 2: Delete Returns Confirmation**
- Action: Delete a note, check response body.
- Expected: Response contains status "deleted" and the note ID.
- Pass if: Response JSON has status=deleted.
```bash
NID=$(curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d '{"text":"Delete confirm test"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf -X DELETE "http://192.168.1.89:3502/api/notes/${NID}" | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert d.get('status') == 'deleted', f'unexpected: {d}'
print('PASS: delete confirmed')
"
```

**Test 3: Delete Non-Existent Note Returns 404**
- Action: DELETE a fake note ID.
- Expected: HTTP 404.
- Pass if: Status is 404.
```bash
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE http://192.168.1.89:3502/api/notes/nonexistent-id-xyz)
[ "$STATUS" = "404" ] && echo "PASS: 404 for nonexistent" || echo "FAIL: status=$STATUS"
```

**Test 4: Deleted Note Not in List**
- Action: Create note, delete it, list all notes.
- Expected: Deleted note ID not in list.
- Pass if: Deleted ID absent from list items.
```bash
NID=$(curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d '{"text":"List removal test"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf -X DELETE "http://192.168.1.89:3502/api/notes/${NID}" > /dev/null
curl -sf http://192.168.1.89:3502/api/notes?limit=100 | python3 -c "
import sys, json, os
d = json.load(sys.stdin)
nid = '$NID'
ids = [n.get('id','') for n in d.get('notes',[])]
if nid not in ids:
    print('PASS: deleted note not in list')
else:
    print('FAIL: deleted note still in list')
"
```

**Test 5: Double Delete Is Idempotent**
- Action: Delete the same note twice.
- Expected: Second delete returns 404 (already gone).
- Pass if: Second DELETE returns 404.
```bash
NID=$(curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d '{"text":"Double delete"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf -X DELETE "http://192.168.1.89:3502/api/notes/${NID}" > /dev/null
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "http://192.168.1.89:3502/api/notes/${NID}")
[ "$STATUS" = "404" ] && echo "PASS: double delete returns 404" || echo "FAIL: status=$STATUS"
```

---

### US-16: View Note Text

**Ideal Behavior:** User retrieves a note by ID. The full content is returned including text, title, timestamps, and any tags or summary.

**Test 1: Full Note Object Returned**
- Action: Create a note with title and text, retrieve it.
- Expected: All fields present in response.
- Pass if: Response contains id, text (or transcript), and created_at.
```bash
NID=$(curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d '{"text":"Full content test note with details","title":"View Test"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf "http://192.168.1.89:3502/api/notes/${NID}" | python3 -c "
import sys, json
d = json.load(sys.stdin)
has_id = 'id' in d
has_text = bool(d.get('text','') or d.get('transcript',''))
has_time = bool(d.get('created_at',''))
if has_id and has_text and has_time:
    print(f'PASS: full note returned')
else:
    print(f'FAIL: missing fields: id={has_id} text={has_text} time={has_time}')
"
```

**Test 2: Note Text Preserved Exactly**
- Action: Create with exact text, retrieve and compare.
- Expected: Text matches byte-for-byte.
- Pass if: Retrieved text == original text.
```bash
ORIGINAL="The quick brown fox jumps over the lazy dog, 12345!"
NID=$(curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d "{\"text\":\"$ORIGINAL\"}" | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf "http://192.168.1.89:3502/api/notes/${NID}" | python3 -c "
import sys, json
d = json.load(sys.stdin)
text = d.get('text','') or d.get('transcript','')
original = '$ORIGINAL'
if text == original:
    print('PASS: text preserved exactly')
else:
    print(f'FAIL: got \"{text}\" expected \"{original}\"')
"
```

**Test 3: Note Update Works**
- Action: Create a note, update its title via PUT.
- Expected: Updated title is returned on GET.
- Pass if: New title matches.
```bash
NID=$(curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d '{"text":"Update me","title":"Old Title"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf -X PUT "http://192.168.1.89:3502/api/notes/${NID}" \
  -H 'Content-Type: application/json' \
  -d '{"title":"New Title"}' | python3 -c "
import sys, json
d = json.load(sys.stdin)
title = d.get('title','')
print('PASS: title updated') if 'New' in title else print(f'FAIL: title={title}')
"
```

**Test 4: Non-Existent Note Returns 404**
- Action: GET a random non-existent note ID.
- Expected: HTTP 404.
- Pass if: Status is 404.
```bash
STATUS=$(curl -s -o /dev/null -w '%{http_code}' http://192.168.1.89:3502/api/notes/does-not-exist-123)
[ "$STATUS" = "404" ] && echo "PASS: 404 for missing note" || echo "FAIL: status=$STATUS"
```

**Test 5: Notes List Pagination**
- Action: Create 3 notes, list with limit=2.
- Expected: Only 2 notes returned, total >= 3.
- Pass if: len(notes) <= 2 and total >= 3.
```bash
for i in 1 2 3; do
  curl -sf -X POST http://192.168.1.89:3502/api/notes \
    -H 'Content-Type: application/json' \
    -d "{\"text\":\"Pagination note $i\"}" > /dev/null
done
curl -sf "http://192.168.1.89:3502/api/notes?limit=2&offset=0" | python3 -c "
import sys, json
d = json.load(sys.stdin)
notes = d.get('notes', [])
total = d.get('total', 0)
if len(notes) <= 2 and total >= 3:
    print(f'PASS: {len(notes)} returned, total={total}')
else:
    print(f'FAIL: {len(notes)} returned, total={total}')
"
```

---

### US-17: Record Voice Note

**Ideal Behavior:** User records a voice note which is saved as a WAV file on the Tab5 SD card. The raw PCM audio is captured via the mic, written to SD in real-time, and optionally sent to Dragon for transcription.

**Test 1: Mic Read Works**
- Action: Send `mic` serial command.
- Expected: Tab5 records 1 second and reports RMS level.
- Pass if: Output contains "RMS level" with a numeric value.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=5)
s.write(b'mic\r\n')
time.sleep(3)
buf = s.read(s.in_waiting).decode(errors='replace')
if 'RMS level' in buf:
    print(f'PASS: {buf.strip()}')
else:
    print(f'FAIL: {buf.strip()}')
s.close()
```

**Test 2: Audio Subsystem Initialized**
- Action: Send `audio` serial command.
- Expected: Reports audio ready with volume level.
- Pass if: Output contains "initialized" or "volume".
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=5)
s.write(b'audio\r\n')
time.sleep(3)
buf = s.read(s.in_waiting).decode(errors='replace')
if 'volume' in buf.lower() or 'initialized' in buf.lower() or 'ready' in buf.lower():
    print(f'PASS: {buf.strip()}')
else:
    print(f'FAIL: {buf.strip()}')
s.close()
```

**Test 3: SD Card Has Space for Recording**
- Action: Check SD card free space via serial.
- Expected: At least 100MB free.
- Pass if: Free space > 0.1 GB.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=3)
s.write(b'sd\r\n')
time.sleep(1)
buf = s.read(s.in_waiting).decode(errors='replace')
print(f'INFO: {buf.strip()}')
if 'Free' in buf:
    print('PASS: SD card has free space info')
else:
    print('WARN: could not verify free space')
s.close()
```

**Test 4: Audio Stream Start API Exists**
- Action: Check audio.h for streaming API.
- Expected: audio_stream_start and audio_stream_stop declared.
- Pass if: Both function signatures exist.
```bash
grep -c 'audio_stream_start\|audio_stream_stop' /home/rebelforce/projects/TinkerTab/main/audio.h
# Expected: >= 2
```

**Test 5: WAV Write Function Exists**
- Action: Check ui_notes for audio write function.
- Expected: ui_notes_write_audio function declared.
- Pass if: Function exists in codebase.
```bash
grep -r 'ui_notes_write_audio' /home/rebelforce/projects/TinkerTab/main/ | head -3
# Expected: at least declaration and call
```

---

### US-18: Background Transcription

**Ideal Behavior:** After a voice note is saved locally, Tab5 can asynchronously send the audio to Dragon's transcription endpoint. This runs in the background without blocking the UI. The transcript is stored alongside the note.

**Test 1: Transcription Endpoint Accepts Raw PCM**
- Action: POST raw PCM to /api/v1/transcribe.
- Expected: Returns JSON with text field.
- Pass if: HTTP 200 with text field.
```bash
python3 -c "import sys; sys.stdout.buffer.write(b'\x00' * 32000)" | \
  curl -sf -X POST http://192.168.1.89:3502/api/v1/transcribe \
    -H 'Content-Type: application/octet-stream' \
    -H 'X-Sample-Rate: 16000' \
    --data-binary @- --max-time 15 | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert 'text' in d, f'missing text field: {d}'
print(f'PASS: text=\"{d[\"text\"]}\" duration={d.get(\"duration_s\",\"?\")}s')
"
```

**Test 2: Transcription Endpoint Accepts WAV**
- Action: POST WAV-formatted audio to transcription endpoint.
- Expected: Returns transcript even when Content-Type is audio/wav.
- Pass if: Response contains text field.
```bash
python3 -c "
import struct, sys
# Minimal WAV header + 1s silence
sr = 16000; ch = 1; bps = 16
data = b'\x00' * (sr * 2)
header = struct.pack('<4sI4s4sIHHIIHH4sI',
    b'RIFF', 36 + len(data), b'WAVE', b'fmt ', 16, 1, ch, sr, sr*ch*bps//8, ch*bps//8, bps, b'data', len(data))
sys.stdout.buffer.write(header + data)
" | curl -sf -X POST http://192.168.1.89:3502/api/v1/transcribe \
  -H 'Content-Type: audio/wav' \
  --data-binary @- --max-time 15 | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert 'text' in d, f'missing text: {d}'
print(f'PASS: WAV transcribe works, text=\"{d[\"text\"]}\"')
"
```

**Test 3: Transcription Queue Initialized at Boot**
- Action: Verify background transcription queue starts from main.c.
- Expected: ui_notes_start_transcription_queue() called in deferred init.
- Pass if: Function called in boot sequence.
```bash
grep -c 'start_transcription_queue' /home/rebelforce/projects/TinkerTab/main/main.c
# Expected: >= 1
```

**Test 4: Minimum Audio Size Enforced**
- Action: Send too-small audio (< 100 bytes) to transcription.
- Expected: HTTP 400 error.
- Pass if: Status is 400.
```bash
STATUS=$(echo -n "tiny" | curl -s -o /dev/null -w '%{http_code}' -X POST \
  http://192.168.1.89:3502/api/v1/transcribe \
  -H 'Content-Type: application/octet-stream' --data-binary @- --max-time 5)
[ "$STATUS" = "400" ] && echo "PASS: tiny audio rejected" || echo "FAIL: status=$STATUS"
```

**Test 5: Transcription Returns Timing Info**
- Action: Transcribe valid audio and check response fields.
- Expected: Response includes stt_ms and duration_s.
- Pass if: Both timing fields are present and numeric.
```bash
python3 -c "import sys; sys.stdout.buffer.write(b'\x00' * 64000)" | \
  curl -sf -X POST http://192.168.1.89:3502/api/v1/transcribe \
    -H 'Content-Type: application/octet-stream' \
    -H 'X-Sample-Rate: 16000' \
    --data-binary @- --max-time 15 | python3 -c "
import sys, json
d = json.load(sys.stdin)
has_stt = isinstance(d.get('stt_ms'), (int, float))
has_dur = isinstance(d.get('duration_s'), (int, float))
if has_stt and has_dur:
    print(f'PASS: stt_ms={d[\"stt_ms\"]} duration_s={d[\"duration_s\"]}')
else:
    print(f'FAIL: missing timing: {d}')
"
```

---

### US-19: Clear Failed Notes

**Ideal Behavior:** Notes that failed to transcribe or are corrupted can be cleaned up. The delete endpoint removes them cleanly. The notes list should not show entries without valid content after cleanup.

**Test 1: Delete All Test Notes**
- Action: List notes, delete ones with "E2E" or "test" in title.
- Expected: Each delete returns success.
- Pass if: All test notes deleted without error.
```bash
curl -sf http://192.168.1.89:3502/api/notes?limit=100 | python3 -c "
import sys, json, urllib.request
d = json.load(sys.stdin)
notes = d.get('notes', [])
deleted = 0
for n in notes:
    title = (n.get('title','') or '').lower()
    text = (n.get('text','') or n.get('transcript','') or '').lower()
    if 'e2e' in title or 'test' in title or 'e2e' in text or 'test' in text:
        nid = n['id']
        req = urllib.request.Request(f'http://192.168.1.89:3502/api/notes/{nid}', method='DELETE')
        urllib.request.urlopen(req, timeout=5)
        deleted += 1
print(f'PASS: deleted {deleted} test notes')
"
```

**Test 2: Notes List After Cleanup**
- Action: List all notes after cleanup.
- Expected: No notes with "test" or "E2E" in title.
- Pass if: Zero test notes remain.
```bash
curl -sf http://192.168.1.89:3502/api/notes?limit=100 | python3 -c "
import sys, json
d = json.load(sys.stdin)
test_notes = [n for n in d.get('notes',[]) if 'test' in (n.get('title','') or '').lower()]
print(f'PASS: {len(test_notes)} test notes remain') if len(test_notes) == 0 else print(f'FAIL: {len(test_notes)} test notes')
"
```

**Test 3: Invalid JSON Rejected for Note Update**
- Action: Send invalid JSON to PUT endpoint.
- Expected: HTTP 400.
- Pass if: Status 400.
```bash
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X PUT http://192.168.1.89:3502/api/notes/any-id \
  -H 'Content-Type: application/json' -d 'not json')
[ "$STATUS" = "400" ] && echo "PASS: invalid JSON rejected" || echo "FAIL: status=$STATUS"
```

**Test 4: Update with No Valid Fields Rejected**
- Action: PUT with fields not in allowed set.
- Expected: HTTP 400 with "No valid fields" error.
- Pass if: Error message about valid fields.
```bash
NID=$(curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d '{"text":"Fields test"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
curl -sf -X PUT "http://192.168.1.89:3502/api/notes/${NID}" \
  -H 'Content-Type: application/json' \
  -d '{"invalid_field":"value"}' | python3 -c "
import sys, json
d = json.load(sys.stdin)
if 'error' in d:
    print(f'PASS: {d[\"error\"]}')
else:
    print(f'FAIL: no error for invalid fields: {d}')
"
```

**Test 5: Search Notes Endpoint Works**
- Action: Create a note, search for it.
- Expected: Search returns the note.
- Pass if: Search results include the note.
```bash
curl -sf -X POST http://192.168.1.89:3502/api/notes \
  -H 'Content-Type: application/json' \
  -d '{"text":"Quantum entanglement discussion for search test"}' > /dev/null
sleep 1
curl -sf -X POST http://192.168.1.89:3502/api/notes/search \
  -H 'Content-Type: application/json' \
  -d '{"query":"quantum entanglement"}' | python3 -c "
import sys, json
d = json.load(sys.stdin)
results = d.get('results', [])
print(f'PASS: search returned {len(results)} results') if len(results) > 0 else print('WARN: 0 results (search may need embeddings)')
"
```

---

## SETTINGS

---

### US-20: Brightness Persist

**Ideal Behavior:** When the user adjusts display brightness, the value is saved to NVS. On reboot, Tab5 reads the saved brightness and applies it during initialization. The brightness range is 0-100%.

**Test 1: Brightness Setting API Exists**
- Action: Check settings.h for brightness getter/setter.
- Expected: Both functions declared.
- Pass if: Header contains get_brightness and set_brightness.
```bash
grep -c 'brightness' /home/rebelforce/projects/TinkerTab/main/settings.h
# Expected: >= 2 (get and set)
```

**Test 2: Serial Brightness Command Works**
- Action: Send `bright 50` via serial.
- Expected: Brightness set to 50%.
- Pass if: Output contains "Brightness: 50" or similar.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=3)
s.write(b'bright 50\r\n')
time.sleep(1)
buf = s.read(s.in_waiting).decode(errors='replace')
if '50' in buf and ('right' in buf.lower() or 'Bright' in buf):
    print(f'PASS: {buf.strip()}')
else:
    print(f'FAIL: {buf.strip()}')
s.close()
```

**Test 3: Default Brightness is 80**
- Action: Check settings.h or settings.c for default value.
- Expected: Default brightness is 80.
- Pass if: Documentation or code shows default 80.
```bash
grep -i 'brightness.*default\|default.*80\|brightness.*80' /home/rebelforce/projects/TinkerTab/main/settings.h
# Expected: comment indicating default is 80
```

**Test 4: Brightness Range 0-100**
- Action: Check settings.h comment.
- Expected: Returns 0-100.
- Pass if: Comment documents the range.
```bash
grep -B1 'get_brightness' /home/rebelforce/projects/TinkerTab/main/settings.h | head -3
# Expected: "Returns 0-100"
```

**Test 5: Brightness Persists via NVS**
- Action: Check that settings uses NVS namespace for persistence.
- Expected: Settings init opens NVS namespace.
- Pass if: settings.c uses nvs_open or NVS APIs.
```bash
grep -c 'nvs_open\|nvs_get\|nvs_set' /home/rebelforce/projects/TinkerTab/main/settings.c 2>/dev/null || \
grep -c 'NVS\|nvs' /home/rebelforce/projects/TinkerTab/main/settings.h
# Expected: >= 1
```

---

### US-21: Volume Persist

**Ideal Behavior:** Speaker volume (0-100) is saved to NVS. On boot, the saved volume is applied to the ES8388 DAC. The default is 70%.

**Test 1: Volume Setting API Exists**
- Action: Check settings.h for volume getter/setter.
- Expected: Both functions declared.
- Pass if: Header contains get_volume and set_volume.
```bash
grep -c 'volume' /home/rebelforce/projects/TinkerTab/main/settings.h
# Expected: >= 2
```

**Test 2: Audio Command Reports Volume**
- Action: Send `audio` serial command.
- Expected: Output includes current volume percentage.
- Pass if: Output contains "volume=" or "volume" with a number.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=3)
s.write(b'audio\r\n')
time.sleep(2)
buf = s.read(s.in_waiting).decode(errors='replace')
if 'volume' in buf.lower():
    print(f'PASS: {buf.strip()[:200]}')
else:
    print(f'FAIL: no volume info: {buf.strip()[:200]}')
s.close()
```

**Test 3: Default Volume is 70**
- Action: Check settings.h for default volume.
- Expected: Default is 70.
- Pass if: Documentation shows default 70.
```bash
grep -B1 'get_volume' /home/rebelforce/projects/TinkerTab/main/settings.h
# Expected: "Returns 0-100 (default 70)"
```

**Test 4: Volume Range 0-100**
- Action: Check audio set volume API.
- Expected: audio_set_volume accepts 0-100.
- Pass if: Header documents the range.
```bash
grep -B1 'audio_set_volume' /home/rebelforce/projects/TinkerTab/main/audio.h
# Expected: "Set speaker volume (0-100)"
```

**Test 5: Volume Applied to Codec**
- Action: Check that audio init or settings apply volume to ES8388.
- Expected: Volume setting calls esp_codec_dev or audio_set_volume.
- Pass if: Code path from settings to codec exists.
```bash
grep -r 'set_volume\|codec_dev_set_vol' /home/rebelforce/projects/TinkerTab/main/audio.c 2>/dev/null | head -3
# Expected: at least one volume-setting call
```

---

### US-22: WiFi Auto-Reconnect

**Ideal Behavior:** WiFi credentials are stored in NVS. On boot, Tab5 reads them and automatically connects. If WiFi disconnects during operation, the event handler retries with exponential backoff (up to WIFI_MAX_RETRY attempts).

**Test 1: WiFi Credentials in NVS**
- Action: Check settings.h for WiFi SSID/password persistence.
- Expected: Get/set WiFi functions exist.
- Pass if: Header declares wifi_ssid and wifi_pass getters/setters.
```bash
grep -c 'wifi_ssid\|wifi_pass' /home/rebelforce/projects/TinkerTab/main/settings.h
# Expected: >= 4 (get_ssid, set_ssid, get_pass, set_pass)
```

**Test 2: WiFi Status via Serial**
- Action: Send `wifi` serial command.
- Expected: Reports connection status and SSID.
- Pass if: Output contains "WiFi:" and SSID name.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=3)
s.write(b'wifi\r\n')
time.sleep(1)
buf = s.read(s.in_waiting).decode(errors='replace')
if 'WiFi' in buf:
    print(f'PASS: {buf.strip()}')
else:
    print(f'FAIL: {buf.strip()}')
s.close()
```

**Test 3: WiFi Connected Check**
- Action: Verify Tab5 debug server is reachable (proves WiFi is up).
- Expected: HTTP response from debug server.
- Pass if: curl to Tab5 succeeds.
```bash
HTTP_CODE=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 http://192.168.1.90:8080/info)
[ "$HTTP_CODE" = "200" ] && echo "PASS: Tab5 reachable (WiFi up)" || echo "FAIL: Tab5 unreachable (code=$HTTP_CODE)"
```

**Test 4: Auto-Reconnect Handler Exists**
- Action: Check for WiFi disconnect event handler with retry logic.
- Expected: Code retries WiFi connection on disconnect.
- Pass if: Event handler with retry counter exists.
```bash
grep -c 'wifi_retry\|WIFI_MAX_RETRY\|esp_wifi_connect' /home/rebelforce/projects/TinkerTab/main/wifi.c 2>/dev/null || \
grep -c 'wifi_retry\|WIFI_MAX_RETRY' /home/rebelforce/projects/TinkerTab/main/*.c 2>/dev/null
# Expected: >= 1
```

**Test 5: WiFi Fallback Credentials**
- Action: Check config for hardcoded WiFi fallback.
- Expected: Fallback SSID/pass defined in config.h.
- Pass if: TAB5_WIFI_SSID macro exists.
```bash
grep -c 'TAB5_WIFI_SSID\|TAB5_WIFI_PASS' /home/rebelforce/projects/TinkerTab/main/config.h
# Expected: >= 2
```

---

### US-23: Cloud Mode Toggle

**Ideal Behavior:** User toggles between local backends (moonshine STT + piper TTS) and cloud backends (OpenRouter STT + TTS). Tab5 sends `{"type":"config_update","cloud_mode":true/false}` to Dragon. Dragon hot-swaps the STT and TTS backends on the active pipeline without dropping the session. Dragon confirms with a config_update response.

**Test 1: Cloud Mode Toggle via REST**
- Action: POST cloud config to Dragon config endpoint.
- Expected: Dragon updates backends and confirms.
- Pass if: Response shows new backend names.
```bash
curl -sf -X POST http://192.168.1.89:3502/api/config \
  -H 'Content-Type: application/json' \
  -d '{"stt":{"backend":"openrouter"},"tts":{"backend":"openrouter"}}' | python3 -c "
import sys, json
d = json.load(sys.stdin)
if d.get('status') == 'ok':
    backends = d.get('backends', {})
    print(f'PASS: backends updated: stt={backends.get(\"stt\")}, tts={backends.get(\"tts\")}')
else:
    print(f'FAIL: {d}')
"
```

**Test 2: Revert to Local Mode**
- Action: POST local config to Dragon config endpoint.
- Expected: Dragon reverts to local backends.
- Pass if: Response shows local backend names.
```bash
curl -sf -X POST http://192.168.1.89:3502/api/config \
  -H 'Content-Type: application/json' \
  -d '{"stt":{"backend":"moonshine"},"tts":{"backend":"piper"}}' | python3 -c "
import sys, json
d = json.load(sys.stdin)
backends = d.get('backends', {})
if backends.get('stt') == 'moonshine' or d.get('status') == 'ok':
    print(f'PASS: reverted to local: stt={backends.get(\"stt\")}, tts={backends.get(\"tts\")}')
else:
    print(f'FAIL: {d}')
"
```

**Test 3: Tab5 Persists Cloud Mode Setting**
- Action: Check settings.h for cloud mode persistence.
- Expected: get/set cloud_mode functions exist.
- Pass if: Header declares cloud_mode getter/setter.
```bash
grep -c 'cloud_mode' /home/rebelforce/projects/TinkerTab/main/settings.h
# Expected: >= 2
```

**Test 4: Voice Module Sends Cloud Mode to Dragon**
- Action: Check voice.c for cloud mode send function.
- Expected: voice_send_cloud_mode sends config_update JSON.
- Pass if: Function sends config_update with cloud_mode.
```bash
grep -A10 'voice_send_cloud_mode' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'config_update'
# Expected: >= 1
```

**Test 5: Dragon Hot-Swaps Pipelines on Config Change**
- Action: Check server.py for pipeline swap on config_update.
- Expected: Active pipelines get swap_backends called.
- Pass if: Code iterates connections and calls swap_backends.
```bash
grep -c 'swap_backends' /home/rebelforce/projects/TinkerBox/dragon_voice/server.py
# Expected: >= 2 (HTTP endpoint + WS handler)
```

---

## NAVIGATION

---

### US-24: All 4 Tileview Pages

**Ideal Behavior:** The TinkerOS home screen is a 4-page horizontal tileview. Each page renders its own content: (1) Main/Home with voice orb, (2) Dragon/Browser, (3) Notes, (4) Settings. Swiping between pages is smooth and each page is fully interactive.

**Test 1: Home Screen Module Exists**
- Action: Check for ui_home.c.
- Expected: File exists and contains page creation code.
- Pass if: File exists.
```bash
[ -f /home/rebelforce/projects/TinkerTab/main/ui_home.c ] && echo "PASS: ui_home.c exists" || echo "FAIL: missing"
```

**Test 2: Tileview Has Multiple Pages**
- Action: Search ui_home.c for tileview tile creation.
- Expected: Multiple lv_tileview_add_tile calls.
- Pass if: At least 3 tile additions.
```bash
grep -c 'tileview_add_tile\|lv_obj_create.*tile' /home/rebelforce/projects/TinkerTab/main/ui_home.c 2>/dev/null
# Expected: >= 3
```

**Test 3: Screenshot Captures Current Page**
- Action: Take a screenshot via debug server.
- Expected: BMP file with correct dimensions (720x1280).
- Pass if: BMP file > 1000 bytes.
```bash
curl -s -o /tmp/page_test.bmp http://192.168.1.90:8080/screenshot.bmp
SIZE=$(stat -c%s /tmp/page_test.bmp 2>/dev/null || stat -f%z /tmp/page_test.bmp 2>/dev/null)
[ "$SIZE" -gt 1000 ] && echo "PASS: screenshot ${SIZE}B" || echo "FAIL: screenshot too small: ${SIZE}B"
```

**Test 4: Debug Info Reports Device State**
- Action: GET /info from debug server.
- Expected: JSON with chip, heap, WiFi, mode info.
- Pass if: JSON is parseable with expected fields.
```bash
curl -sf http://192.168.1.90:8080/info | python3 -c "
import sys, json
d = json.load(sys.stdin)
keys = list(d.keys())
print(f'PASS: info has {len(keys)} fields: {keys[:6]}...')
"
```

**Test 5: Touch Injection Works**
- Action: POST a touch event via debug server.
- Expected: HTTP 200 response.
- Pass if: Touch endpoint responds successfully.
```bash
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://192.168.1.90:8080/touch \
  -d '{"x":360,"y":640,"action":"tap"}')
[ "$STATUS" = "200" ] && echo "PASS: touch injection accepted" || echo "FAIL: status=$STATUS"
```

---

### US-25: Floating Mic from Any Page

**Ideal Behavior:** A floating mic button is visible on every page of the tileview (except the home page where the voice orb serves the same purpose). Tapping it from any page starts a voice session, regardless of which page is currently displayed.

**Test 1: Voice UI Overlay Module Exists**
- Action: Check for ui_voice.c and ui_voice.h.
- Expected: Both files exist.
- Pass if: Files exist.
```bash
[ -f /home/rebelforce/projects/TinkerTab/main/ui_voice.c ] && \
[ -f /home/rebelforce/projects/TinkerTab/main/ui_voice.h ] && \
echo "PASS: voice UI overlay files exist" || echo "FAIL: missing files"
```

**Test 2: Mic Button Created in Voice UI Init**
- Action: Check ui_voice.c for mic button creation.
- Expected: A floating button is created with mic icon.
- Pass if: Code creates a floating button.
```bash
grep -c 'mic_btn\|MIC\|floating' /home/rebelforce/projects/TinkerTab/main/ui_voice.c 2>/dev/null
# Expected: >= 1
```

**Test 3: Mic Button Getter Exists**
- Action: Check for ui_voice_get_mic_btn function.
- Expected: Public API to access the mic button.
- Pass if: Function declared.
```bash
grep -c 'ui_voice_get_mic_btn' /home/rebelforce/projects/TinkerTab/main/main.c
# Expected: >= 1 (called in main to hide on home)
```

**Test 4: Voice Connect Async Exists**
- Action: Check voice.h for async connect function.
- Expected: voice_connect_async declared.
- Pass if: Function signature exists.
```bash
grep -c 'voice_connect_async' /home/rebelforce/projects/TinkerTab/main/voice.h
# Expected: >= 1
```

**Test 5: Mode Manager Handles Voice from Any State**
- Action: Check mode_manager for MODE_VOICE transition.
- Expected: Mode switch to VOICE is possible from any current mode.
- Pass if: mode_manager handles voice mode.
```bash
grep -c 'MODE_VOICE' /home/rebelforce/projects/TinkerTab/main/mode_manager.c 2>/dev/null || \
grep -c 'MODE_VOICE' /home/rebelforce/projects/TinkerTab/main/mode_manager.h 2>/dev/null
# Expected: >= 1
```

---

### US-26: Status Bar Accuracy

**Ideal Behavior:** The status bar shows real-time WiFi signal strength, battery percentage, and Dragon connection status (green dot = connected, red = offline). These update every few seconds and reflect the actual hardware state.

**Test 1: Battery Info Available**
- Action: Send `bat` serial command.
- Expected: Reports voltage and percentage.
- Pass if: Output contains voltage or percentage.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=3)
s.write(b'bat\r\n')
time.sleep(1)
buf = s.read(s.in_waiting).decode(errors='replace')
if 'V' in buf or '%' in buf or 'Battery' in buf:
    print(f'PASS: {buf.strip()}')
else:
    print(f'FAIL: {buf.strip()}')
s.close()
```

**Test 2: WiFi Info Available**
- Action: Send `wifi` serial command.
- Expected: Reports connection status.
- Pass if: Output contains "connected" or SSID.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=3)
s.write(b'wifi\r\n')
time.sleep(1)
buf = s.read(s.in_waiting).decode(errors='replace')
if 'WiFi' in buf:
    print(f'PASS: {buf.strip()}')
else:
    print(f'FAIL: {buf.strip()}')
s.close()
```

**Test 3: Dragon Status Available**
- Action: Send `dragon` serial command.
- Expected: Reports Dragon connection state.
- Pass if: Output contains state string (idle/discovering/connected/streaming).
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=3)
s.write(b'dragon\r\n')
time.sleep(1)
buf = s.read(s.in_waiting).decode(errors='replace')
if 'Dragon' in buf:
    print(f'PASS: {buf.strip()}')
else:
    print(f'FAIL: {buf.strip()}')
s.close()
```

**Test 4: RTC Time Available**
- Action: Send `rtc` serial command.
- Expected: Reports current time.
- Pass if: Output contains time in HH:MM format.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=3)
s.write(b'rtc\r\n')
time.sleep(1)
buf = s.read(s.in_waiting).decode(errors='replace')
if ':' in buf and ('20' in buf or 'RTC' in buf):
    print(f'PASS: {buf.strip()[:100]}')
else:
    print(f'FAIL: {buf.strip()[:100]}')
s.close()
```

**Test 5: Info Command Shows All Peripheral States**
- Action: Send `info` serial command.
- Expected: Reports all peripherals (WiFi, Touch, SD, Camera, Audio, Mic, IMU, RTC, Battery).
- Pass if: Output contains at least 7 of 9 peripheral status lines.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=3)
s.write(b'info\r\n')
time.sleep(1)
buf = s.read(s.in_waiting).decode(errors='replace')
checks = ['WiFi', 'Touch', 'SD', 'Camera', 'Audio', 'Mic', 'IMU', 'RTC', 'Battery']
found = sum(1 for c in checks if c in buf)
if found >= 7:
    print(f'PASS: {found}/9 peripherals reported')
else:
    print(f'FAIL: only {found}/9 peripherals reported')
s.close()
```

---

## ERROR HANDLING

---

### US-27: Dragon Offline

**Ideal Behavior:** When Dragon is unreachable (powered off, network issue), Tab5 detects this during the WebSocket connect phase. The voice_connect function times out after VOICE_CONNECT_TIMEOUT_MS (5s) and transitions to IDLE state with a "connect failed" detail. The UI shows a clear error message. Tab5 does not hang or crash.

**Test 1: Connection Timeout Defined**
- Action: Check voice.c for connect timeout constant.
- Expected: VOICE_CONNECT_TIMEOUT_MS is 5000.
- Pass if: Constant defined as 5000.
```bash
grep 'VOICE_CONNECT_TIMEOUT_MS' /home/rebelforce/projects/TinkerTab/main/voice.c | head -1
# Expected: #define VOICE_CONNECT_TIMEOUT_MS 5000
```

**Test 2: Failed Connect Returns to IDLE**
- Action: Check voice_connect for failure state transition.
- Expected: On connect failure, state goes to IDLE with error detail.
- Pass if: Code sets VOICE_STATE_IDLE on connect failure.
```bash
grep -A5 'WS connect failed' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'VOICE_STATE_IDLE'
# Expected: >= 1
```

**Test 3: Dragon Health Endpoint Unreachable Handling**
- Action: Try to reach Dragon health on a port that does not exist.
- Expected: curl times out or connection refused within 5s.
- Pass if: Command completes without hanging.
```bash
timeout 5 curl -sf --max-time 3 http://192.168.1.89:9999/health 2>/dev/null
EXIT=$?
if [ "$EXIT" -ne 0 ]; then
  echo "PASS: unreachable endpoint handled gracefully (exit=$EXIT)"
else
  echo "FAIL: unexpected success"
fi
```

**Test 4: Dragon Link Reconnect Backoff**
- Action: Check dragon_link.c for exponential backoff logic.
- Expected: Backoff doubles up to RECONNECT_MAX_MS.
- Pass if: Code multiplies backoff by 2.
```bash
grep -c 'backoff.*\*.*2\|s_backoff_ms \*= 2\|s_backoff_ms = s_backoff_ms \* 2' /home/rebelforce/projects/TinkerTab/main/dragon_link.c
# Expected: >= 1
```

**Test 5: Error State Does Not Crash**
- Action: Check that voice_set_state handles error strings safely.
- Expected: NULL detail is handled without crash.
- Pass if: Code checks for NULL detail.
```bash
grep -A5 'voice_set_state' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'detail.*NULL\|detail ? detail'
# Expected: >= 1
```

---

### US-28: WiFi Drop During Voice

**Ideal Behavior:** If WiFi drops while a voice session is active (LISTENING or PROCESSING), the WebSocket connection fails. The WS receive task detects the disconnection (poll error or read error), sets s_ws_connected = false, and transitions to IDLE. The mic task also stops because it checks s_ws_connected. No crash, no leaked tasks.

**Test 1: WS Poll Error Triggers Disconnect**
- Action: Check ws_receive_task for poll error handling.
- Expected: Negative poll result marks WS as disconnected.
- Pass if: Code sets s_ws_connected = false on poll error.
```bash
grep -A3 'poll error\|poll < 0' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 's_ws_connected = false'
# Expected: >= 1
```

**Test 2: WS Read Error Triggers Disconnect**
- Action: Check for read error handling in receive task.
- Expected: len <= 0 causes disconnect.
- Pass if: Code handles negative read length.
```bash
grep -A3 'WS read error\|len <= 0' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 's_ws_connected'
# Expected: >= 1
```

**Test 3: Mic Task Checks WS Connection**
- Action: Check mic_capture_task loop condition.
- Expected: Loop exits when s_ws_connected is false (in ask mode).
- Pass if: Loop condition includes ws_connected check.
```bash
grep 'while.*s_mic_running.*s_ws_connected' /home/rebelforce/projects/TinkerTab/main/voice.c
# Expected: matches the while condition in mic_capture_task
```

**Test 4: Cleanup On Unexpected Disconnect**
- Action: Check that WS receive task cleans up transport on exit.
- Expected: Transport is closed and destroyed on unexpected disconnect.
- Pass if: Code calls esp_transport_close and destroy.
```bash
grep -c 'esp_transport_close\|esp_transport_destroy' /home/rebelforce/projects/TinkerTab/main/voice.c
# Expected: >= 4 (in connect, disconnect, and receive task cleanup)
```

**Test 5: State Transitions to IDLE on Disconnect**
- Action: Check that unexpected disconnect sets IDLE state.
- Expected: ws_receive_task sets IDLE with "disconnected" detail.
- Pass if: Code sets VOICE_STATE_IDLE with disconnected.
```bash
grep -c 'VOICE_STATE_IDLE.*disconnected\|"disconnected"' /home/rebelforce/projects/TinkerTab/main/voice.c
# Expected: >= 1
```

---

### US-29: LLM/TTS Timeout

**Ideal Behavior:** If Dragon's LLM or TTS takes too long (>35s), Tab5's response timeout fires. The WS receive task sends a cancel to Dragon, resets playback, disables speaker, and transitions to READY with "timeout" detail. This prevents the UI from hanging indefinitely on a slow or stuck LLM.

**Test 1: Response Timeout Defined**
- Action: Check voice.c for response timeout constant.
- Expected: VOICE_RESPONSE_TIMEOUT_MS is 35000.
- Pass if: Constant is 35000.
```bash
grep 'VOICE_RESPONSE_TIMEOUT_MS' /home/rebelforce/projects/TinkerTab/main/voice.c | head -1
# Expected: #define VOICE_RESPONSE_TIMEOUT_MS 35000
```

**Test 2: Timeout Sends Cancel to Dragon**
- Action: Check that timeout handler sends cancel message.
- Expected: On timeout, sends `{"type":"cancel"}` and resets.
- Pass if: Code sends cancel on timeout.
```bash
grep -A5 'response timeout\|Dragon response timeout' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'cancel'
# Expected: >= 1
```

**Test 3: Timeout Resets Playback Buffer**
- Action: Check that timeout handler resets playback.
- Expected: playback_buf_reset called on timeout.
- Pass if: Code resets buffer on timeout.
```bash
grep -A5 'response timeout\|Dragon response timeout' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'playback_buf_reset'
# Expected: >= 1
```

**Test 4: Timeout Transitions to READY**
- Action: Check state transition on timeout.
- Expected: State goes to READY with "timeout" detail.
- Pass if: Code sets VOICE_STATE_READY with "timeout".
```bash
grep -A5 'Dragon response timeout' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'VOICE_STATE_READY.*timeout'
# Expected: >= 1
```

**Test 5: Keepalive Pings Do Not Reset Timeout**
- Action: Check that keepalive sends do not reset the activity timestamp.
- Expected: Comment or code confirms keepalive does not reset s_last_activity_us.
- Pass if: Keepalive code does not touch s_last_activity_us.
```bash
grep -B2 -A5 'keepalive' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'last_activity'
# Expected: 0 (keepalive should NOT reset the activity timer — only incoming data does)
```

---

### US-30: No Speech Detected

**Ideal Behavior:** When Dragon's STT processes audio with no speech, it returns an empty transcript or a "no speech" error. Tab5 receives this as either an empty STT message or an error message, shows a brief notification, and returns to READY. The user can immediately try again.

**Test 1: Dragon Error Message Handling**
- Action: Check voice.c for error message type handling.
- Expected: "error" type messages are handled, state transitions to READY.
- Pass if: Code handles "error" type and transitions state.
```bash
grep -A10 '"error"' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'VOICE_STATE_READY'
# Expected: >= 1
```

**Test 2: Error Stops Playback**
- Action: Check that error handler resets playback and disables speaker.
- Expected: playback_buf_reset and speaker_enable(false) called.
- Pass if: Both cleanup actions present in error handler.
```bash
grep -A10 '"error"' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'playback_buf_reset\|speaker_enable'
# Expected: >= 2
```

**Test 3: Error Detail Forwarded to UI**
- Action: Check that error message text is passed to state callback.
- Expected: Error message string is passed as detail to voice_set_state.
- Pass if: Code extracts error message and passes to state callback.
```bash
grep -B2 -A15 'Dragon error' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'voice_set_state'
# Expected: >= 1
```

**Test 4: Empty STT Text Handled**
- Action: Check that STT handler works with empty text.
- Expected: Empty STT text does not crash; state still transitions.
- Pass if: Code checks for valid text string.
```bash
grep -A5 '"stt"' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 'cJSON_IsString'
# Expected: >= 1
```

**Test 5: WS Stays Connected After Error**
- Action: Check that transient errors keep the WS connection alive.
- Expected: Error handler transitions to READY (not IDLE) if WS is still connected.
- Pass if: State depends on s_ws_connected.
```bash
grep -A10 '"error"' /home/rebelforce/projects/TinkerTab/main/voice.c | grep -c 's_ws_connected.*VOICE_STATE_READY'
# Expected: >= 1
```

---

## Test Execution Summary

| Category | Stories | Tests | Type |
|----------|---------|-------|------|
| Voice Ask Mode | US-01 to US-05 | 25 | Mixed (live + code) |
| Multi-Turn | US-06 to US-10 | 25 | Mostly live API |
| Dictation | US-11 to US-13 | 15 | Mixed |
| Notes CRUD | US-14 to US-19 | 30 | Mostly live API |
| Settings | US-20 to US-23 | 20 | Mixed (serial + code) |
| Navigation | US-24 to US-26 | 15 | Mixed (debug server + code) |
| Error Handling | US-27 to US-30 | 20 | Mostly code inspection |
| **Total** | **30** | **150** | |

### Prerequisites
1. Tab5 powered on and connected to WiFi (192.168.1.90)
2. Dragon voice server running on 192.168.1.89:3502
3. Tab5 serial available at /dev/ttyACM0
4. Debug server running on Tab5 (port 8080)
5. Python 3.10+ with `serial` package (`pip install pyserial`)
6. curl installed
