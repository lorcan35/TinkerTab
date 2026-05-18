# K144 StackFlow components (out-of-tree)

Custom StackFlow units that get cross-compiled for K144's AX630C ARM64 and
ADB-pushed to `/opt/m5stack/bin/` on the module.  Tracked in TinkerTab so
the source lives next to its Tab5-side caller, but the build is run inside
M5Stack's StackFlow tree.

## main_ext_pcm — host PCM injection for wake-on-Tab5-mic (TT #131)

Bridges PCM frames received via UART (over the existing `llm_sys` JSON
bridge) onto K144's internal `sys.pcm` ZMQ bus so the stock `llm_asr`
(sherpa-ncnn streaming zipformer) consumes Tab5's mic audio instead of
K144's onboard mic.

### Build (next session)

```bash
# Outside the TinkerTab repo:
git clone https://github.com/m5stack/StackFlow ~/projects/StackFlow
cd ~/projects/StackFlow
git submodule update --init --depth 1

# Copy our unit into the StackFlow tree
cp -r ~/projects/TinkerTab/k144-stackflow/main_ext_pcm \
      projects/llm_framework/main_ext_pcm

# Edit projects/llm_framework/SConstruct to include 'main_ext_pcm' in
# the COMPONENTS list.

# Install build deps on dev host (one-time)
pip install scons parse kconfiglib
sudo apt install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# Run the build — M5Stack's SConstruct auto-downloads static_lib_v0.1.3.tar.gz
cd projects/llm_framework
scons
# Build outputs: build/llm_framework/main_ext_pcm/llm_ext_pcm
```

### Deploy

```bash
# 1. Push the cross-compiled binary
adb push build/llm_framework/main_ext_pcm/llm_ext_pcm \
        /opt/m5stack/bin/llm_ext_pcm
adb shell chmod +x /opt/m5stack/bin/llm_ext_pcm

# 2. Install the systemd unit so it auto-starts AND survives sys.reset
adb push main_ext_pcm/llm-ext-pcm.service /lib/systemd/system/llm-ext-pcm.service
adb shell systemctl daemon-reload
adb shell systemctl enable --now llm-ext-pcm.service
adb shell systemctl status llm-ext-pcm.service
```

### Verified working sequence (live on K144 2026-05-18)

End-to-end test: TCP-push PCM → ZMQ PUB → llm_asr SUB → sherpa-ncnn transcript.

```bash
# After deploy, reset the daemon to a clean state, then:

# 1. Boot llm_asr against sys.pcm. THIS BINDS audio's pub at
#    /tmp/llm/pcm.cap.socket, stealing our ext_pcm bind.
echo '{"request_id":"a","work_id":"asr","action":"setup","object":"asr.setup",
       "data":{"model":"sherpa-ncnn-streaming-zipformer-20M-2023-02-17",
               "input":["sys.pcm"],"response_format":"asr.utf-8.stream",
               "enoutput":true}}' | nc -q 1 localhost 10001

# 2. Tell audio to release its bind.  asr's ZMQ subscriber stays alive and
#    will auto-reconnect to whoever next binds the URL.
echo '{"request_id":"c","work_id":"audio","action":"cap_stop_all"}' | nc -q 1 localhost 10001

# 3. Restart ext_pcm so it reclaims /tmp/llm/pcm.cap.socket.
adb shell systemctl restart llm-ext-pcm.service

# 4. Push raw int16 LE 16 kHz mono PCM to TCP port 9999 (3200-byte frames =
#    100 ms each, real-time pacing).  ext_pcm forwards each frame on the ZMQ
#    PUB.  ASR transcripts stream out on asr.utf-8.stream.
ffmpeg -i your_speech.wav -ar 16000 -ac 1 -f s16le - | nc localhost 9999
```

Live verification (2026-05-18): pushed 5.18 s of neutts_a.wav → ASR emitted
`"it's sunday evening your meeting with sarace in fifty minutes"` — exact
match for the audio content.  ext_pcm log confirms `pumped 100 frames`.

Wake fires (when streaming TinkerTab's mic via UART relay) arrive on
`asr.utf-8.stream` exactly the same as today's K144-onboard-mic wake path.

### Plan doc + risks

See `TinkerTab/docs/PLAN-tab5-mic-to-k144-ext-pcm.md` for the full plan,
phase breakdown, and risk analysis.  Key risks:
- Audio unit conflict on `sys_pcm_cap_channel` binding (mitigated by binding-first ordering)
- Cross-compile toolchain compatibility with K144's libstdc++ vintage
- UART bandwidth saturation under sustained 16 kHz mono PCM
