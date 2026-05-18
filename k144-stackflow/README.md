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
adb push build/llm_framework/main_ext_pcm/llm_ext_pcm \
        /opt/m5stack/bin/llm_ext_pcm
adb shell chmod +x /opt/m5stack/bin/llm_ext_pcm
adb shell systemctl restart sys-llm.service   # restarts StackFlow daemon
```

### Use

From Tab5 (Tier-2 firmware change in voice_wake_stream.c, see plan doc):

```json
{"action":"setup", "work_id":"ext_pcm",
 "data":{"sys_pcm_cap_channel":"ipc:///tmp/llm/pcm.cap.socket"}}

{"action":"setup", "work_id":"asr",
 "object":"asr.setup",
 "data":{"model":"sherpa-ncnn-streaming-zipformer-20M-2023-02-17",
         "input":["sys.pcm"], "response_format":"asr.utf-8.stream",
         "enoutput":true}}

# Then stream PCM frames:
{"action":"inference", "work_id":"ext_pcm.NNNN",
 "object":"audio.pcm.base64", "data":"<base64 int16 LE 16kHz mono>"}
```

Wake fires arrive on `asr.utf-8.stream` exactly the same as today's K144-onboard-mic
wake path.

### Plan doc + risks

See `TinkerTab/docs/PLAN-tab5-mic-to-k144-ext-pcm.md` for the full plan,
phase breakdown, and risk analysis.  Key risks:
- Audio unit conflict on `sys_pcm_cap_channel` binding (mitigated by binding-first ordering)
- Cross-compile toolchain compatibility with K144's libstdc++ vintage
- UART bandwidth saturation under sustained 16 kHz mono PCM
