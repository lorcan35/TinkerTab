# TinkerTab Streaming Architecture — Research & Recommendations

## Current Setup
- Dragon (Radxa Q6A) captures Chromium via CDP screencast → MJPEG multipart HTTP
- Tab5 (ESP32-P4) receives MJPEG, decodes with HW JPEG, renders to DPI framebuffer
- Touch events sent back via WebSocket

## Key Findings

### Protocol: MJPEG over HTTP multipart is optimal
- Native ESP32-P4 hardware JPEG decoder: 720p@88fps / 1080p@30fps capability
- Stateless, simple, every frame independently decodable (no keyframe dependencies)
- ~900 Kbps at 15fps — negligible on WiFi 6 (30-50 Mbps practical throughput)

### Resolution/Quality: 720x1280 @ quality 80
- Native Tab5 resolution, no scaling overhead
- ~75KB per frame, excellent visual quality for web content
- Quality 60 = readable but artifacts; 90 = overkill (+60% size, <5% perceptual gain)

### Task Architecture (optimal pipeline)
```
Core 1 (high priority):
  Network RX → JPEG HW decode → framebuffer copy

Core 0 (LVGL only):
  UI rendering, touch input
```

### Memory: 3-buffer ring in PSRAM, DMA-aligned
- Buffer A: incoming JPEG data (PSRAM, DMA-aligned)
- Buffer B: decoded frame (PSRAM, DMA-aligned)
- Buffer C: display framebuffer
- **CRITICAL**: esp_cache_msync() after every decode (no HW cache coherence on P4)

### Realistic FPS: 12-15 FPS sustainable
- JPEG decode: ~11-15ms per 720x1280 frame (hardware)
- Network: ~5-10ms per frame
- Framebuffer copy: ~20-25ms (overlaps with decode)
- **Bottleneck: Dragon CDP screencast at 15fps, not Tab5 hardware**

### Frame Drop Strategy: timestamp-based adaptive
- Skip frames older than 50ms
- Queue depth > 3 → drop oldest, keep latest
- Monitor decode time, log warnings if >30ms

### ESP-IDF Optimizations
1. `jpeg_alloc_decoder_mem()` for proper DMA alignment
2. Output format `JPEG_RAW_TYPE_RGB565` — decode directly to display format
3. `esp_cache_msync()` after every decode (ESP32-P4 has no HW cache coherence)
4. TCP_NODELAY on socket for low-latency frames
5. 256KB socket RX buffer to hold multiple frames
6. PSRAM at 200MHz quad SPI mode
7. Watchdog set to 30s (decode can take 15-25ms)
