# serviSynth roadmap

## Phase A — richer voice engine (still one timbre)

- [x] Independent wave select per oscillator (MiniMoog style): osc1 & osc2
      each choose sine / saw / square / tri / PWM / noise
- [x] PWM: PolyBLEP pulse with width param (5–95%)
- [x] Noise: xorshift32 white source per voice + osc-mix knob
- [x] Osc mixer: osc1/osc2 blend param (replaces fixed 0.7 sum)
- [ ] Vibrato: route the global LFO to pitch as well as cutoff
      (lfo_dest: cutoff / pitch / both)
- [ ] Filter key-tracking: cutoff follows note pitch (param, 0..1)
- [ ] IRAM_ATTR on synth_render hot path (+ fast-path poly_blep)
      → target 12–16 voices

## Phase B — 2-part multitimbral

- [x] synth_params -> patches[2] + fx; voice gets part_idx
- [x] MIDI channel -> part routing (ch1 -> part0, ch2 -> part1, else part0)
- [x] Shared voice pool, global LRU stealing across parts
- [x] Web UI: part tabs (P1/P2) — params scoped per part, state push
- [x] Shared bus FX + master stays global (per-channel CCs route per part)

## Phase C — FX chain (order: chorus -> delay -> reverb -> comp)

- [ ] Chorus: 2× LFO-modulated ~15–25ms delay lines, internal RAM, mix param
- [ ] Freeverb: 8 combs + 4 allpasses per channel in PSRAM (~250KB),
      roomsize / damp / wet params
- [ ] FX param namespace in web UI + params struct

## Phase D — persistence & polish

- [ ] Preset slots in NVS (save/load/name, 8–16 slots, per part)
- [ ] Voice steal quick-fade (kills steal clicks)
- [ ] OTA via the two app slots (needs partition tweak)

## Later / maybe

- 4 parts (same code path, #define + UI tabs)
- Wavetables / sample playback from the storage partition
- Per-part FX sends
