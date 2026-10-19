# serviSynth

An ESP32-S3 polysynth served from the chip itself — browser UI over WiFi AP,
USB-MIDI keyboard in over OTG host mode, audio out through a PCM5102 I2S DAC.

## Features

- 8 voices, PolyBLEP oscillators (sine / saw / square / triangle), detuned dual-osc
- Per-voice TPT state-variable filter (cutoff / resonance), filter LFO, velocity → cutoff
- ADSR envelope, portamento, per-voice stereo spread
- Ping-pong delay FX (PSRAM-backed ring, damping in feedback)
- Web UI: softAP `esp32-synth` → http://192.168.4.1 (websocket-driven)
- USB-MIDI host on the native USB port: note on/off w/ velocity, pitch bend (±2 st),
  CC1 → filter LFO depth, CC7 → master, CC71 → resonance, CC74 → cutoff

## Wiring (PCM5102 module)

| ESP32-S3 | PCM5102 |
|----------|---------|
| GPIO 7   | SCK (MCLK, 256×fs) |
| GPIO 6   | BCK |
| GPIO 4   | LRCK |
| GPIO 5   | DIN |
| 3V3/5V   | VCC |
| GND      | GND |

Module jumpers: XSMT→H (unmute, required), FLT→L, DEMP→L, SCK pad left open.
For USB host: bridge the board's USB-OTG pad so VBUS reaches the device,
power the board via the COM/UART port, keyboard on the native USB port.

## Build

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

Flash/log over the COM (UART) port — the USB port belongs to the MIDI host once running.
