# SmarterFan

Replacing the brain of a **Novohome NH-VTR500** ceiling fan (retractable blades, dimmable CCT LED, 433 MHz remote) with an ESP32, without touching its mains-side power electronics.

The stock firmware is locked in an undocumented mask-ROM MCU: the lowest brightness step is still uncomfortably bright, there are three fixed colour temperatures, and the only way in is the handheld remote. This project intercepts the two control paths that matter — the LED dimming lines and the RF receiver's output — and leaves the rectifier, LED drivers, isolation barrier and motor stage exactly as built.

**Status:** installed and running in the fan. Both interventions are done and confirmed on hardware — the ESP32 drives the LEDs far below the OEM floor, and fan commands relay through it to the OEM MCU. The remote still works. The sunrise alarm is written and validated but has not yet run against a real morning. What else remains is measurement, not construction: see [Still open](#still-open).

---

## ⚠️ Safety

**This board carries mains voltage and hangs over your head.**

- Roughly the left third of the PCB is mains-referenced. Treat it as live whenever the fan is plugged in.
- The bulk caps are rated 450 V and hold charge. **Bleed and verify with a meter before touching anything.**
- No scope ground clip on the mains-referenced section. Isolation transformer, or a differential probe.
- No USB to the ESP32 while the board is on mains. The secondary is coupled to the line through CY1/CY2 and can float. Use an isolation transformer, a USB isolator, or OTA.
- Uncertified, not fire-tested, voids your warranty and possibly your insurance. **You are responsible for what you hang from your ceiling.**

If any of that is unfamiliar, this is not a good first electronics project.

---

## Scope

In: dimming far below the OEM minimum down to off, continuous colour temperature, network control, sunrise alarm, and the original remote still working.

Out: **slowing the fan below its lowest OEM speed.** Fan commands are routed *through* the stock MCU, so you get the OEM's six speeds from the network instead of only from the remote. The lowest speed is still the lowest speed — see [Out of scope](#out-of-scope).

---

## Target hardware

- **Fan:** Novohome NH-VTR500 — 4 retractable blades, 30 W DC motor, 6 speeds, 56 W LED with 3 colour tones
- **Control board silkscreen:** `XH-FSD03D5-1A`, dated `2025-09-19`

Other fans may use the same board; the silkscreen is the thing to match. If yours differs the approach still applies, but every reference designator below will be wrong.

### The parts that matter

| Ref | Part | Function |
|---|---|---|
| U3 | `MT006MAPN 5702S` | Main MCU, TSSOP-28. No datasheet, no reflash path. |
| U10, U11 | `MT9722S` | Mains-side constant-current LED drivers, one per colour channel |
| U2 (×3) | `PC817` | Optocouplers across the isolation barrier |
| Y1 + SOP-8 | `13.52 MHz` | 433 MHz superheterodyne receiver, feeds the `ANT` wire |
| R37, R38 | `102` (1 kΩ) | MCU → dimming optocoupler drive resistors — **removed** |
| `102` at RF | 1 kΩ | Receiver output → MCU input — **removed** |
| U4, U5, U6 | `4614 GA8T25` ×3 | One half-bridge per motor phase, untouched |
| CE5, CE6 | 2.2 µF 450 V | Bulk caps for the two LED drivers |

Two facts drove the whole design: **the remote is RF, not IR** (a 13.52 MHz crystal beside an SOP-8 with a wire on the `ANT` pad is a 433.92 MHz superhet), and **the dimming floor is a firmware limit, not a hardware one** — nothing in the analogue path forbids going lower.

---

## How it works

Two independent interventions, both reversible, both just resistor removals.

```mermaid
flowchart TB
    REM([433 MHz remote]) -.->|RF| RX[OEM RF receiver]
    NET([Network]) <-->|Wi-Fi| ESP
    RX -->|shifter, 4.48 → 3.3 V| ESP[ESP32-C3]
    ESP -->|shifter, 3.3 → 4.48 V| MCU[OEM MCU]
    ESP -->|LEDC PWM ×2| OPT[2× PC817]
    MCU --> DRV[U4–U6 half-bridges]
    DRV --> MOT([BLDC motor])
    OPT --> LDRV[U10 / U11 LED drivers]
    LDRV --> LEDS([LED module])
```

**1. LED dimming — taken over completely.** `R37` and `R38` out isolates the MCU from the two dimming optocouplers, and the ESP32 drives their LEDs directly through its own resistors. The MCU keeps driving its now-disconnected pins; harmless. Sizing:

```
MCU high 4.48 V, post-resistor 1.12 V (PC817 Vf)  →  3.36 mA
From a 3.3 V GPIO: (3.3 − 1.12) / 0.00336 ≈ 650 Ω  →  620 Ω
```

Both pads carry `ledc` PWM at **250 Hz** (not the OEM's 1 kHz — see [Dimming](#dimming)), and the two channels are one `cwww` light: brightness on one axis, channel mix on the other. The mix is normalised by the larger fraction, so mid-colour-temperature is *both* channels at full rather than each at half, and 100% there is the full 56 W. Total output therefore roughly doubles toward the middle of the CCT sweep. That is deliberate — it is what reaching full output costs.

**2. RF — relayed through the ESP32.** The `102` between the receiver's output and the MCU's input comes out and the ESP32 sits in the middle: it taps the receiver through a level shifter and decodes the OOK in software, then reconstructs packets and injects them into the MCU's input, which cannot tell the difference. Fan commands are forwarded; the six light keys stop at the ESP32 and drive the LEDs directly.

**Tradeoff:** the ESP32 is now a single point of failure for the remote. If it hangs, the remote stops working. Watchdog it. Wi-Fi dropping out is *not* a problem — the relay runs on the RMT peripheral and never touches the network stack.

---

## Hardware

### Bill of materials

| Item | Notes |
|---|---|
| **NodeMCU ESP32-C3 SuperMini** | Requires the antenna mod — see [Antenna](#antenna). Socket it, don't solder it down. |
| BSS138 4-channel level shifter | The $1 bidirectional I²C module. Used on the inject line; it works on the sniff line too, though it lifts the pad's low from 0 V to 0.8 V. |
| Buck converter → 5 V | **MP1584.** The LV bus measures **24 V**, inside its 28 V rating — but see [Power](#power) for the margin. |
| 2× 620 Ω | Optocoupler drive |
| 1× 1 kΩ | Series protection on the RF inject line |
| 2× 470 µF, **105 °C or polymer** | Bulk, one on 3V3 and one on 5 V. 85 °C parts dry out in a hot canopy. |
| 2× 100 nF ceramic | One per rail, at the module pins |
| U.FL → SMA pigtail + 2.4 GHz antenna | Route it out of the canopy |
| 1× `CD4050B` *(optional)* | Alternative to a divider on the sniff line. A correctly sized divider works fine — at these timings source impedance is irrelevant. |

**Tools:** oscilloscope (not optional — you cannot do the RF work without one), multimeter, fine-tip iron, hot air or tweezers for 0805 removal, **isolation transformer**, USB isolator if flashing over USB while powered.

### Wiring

```
 LV DC bus (24 V) ──[ MP1584 → 5 V ]──→ ESP32 5V ──(LDO)── 3V3 ──→ shifter LV

 MCU VDD (4.48 V) ───────────────────────────────────────────────→ shifter HV

 secondary GND ───┬── ESP32 GND
                  └── shifter GND

 GPIO 7  ──[620 Ω]──→ opto-side pad of R37   (warm)
 GPIO 10 ──[620 Ω]──→ opto-side pad of R38   (cold)
 GPIO 5  ──→ shifter LV1 ─── HV1 ──[1 kΩ]──→ MCU RF input pad
 GPIO 6  ←──[10k]──┬── RF receiver output pad     (divider: 3.08 V)
                   [22k]
                    │
                   GND
```

| Signal | GPIO | Peripheral |
|---|---|---|
| RF inject | 5 | RMT TX |
| RF sniff | 6 | RMT RX |
| LED warm (`R37`) | 7 | LEDC |
| LED cold (`R38`) | 10 | LEDC |

These four because everything else is spoken for: 11–17 flash, 18/19 USB, 2/8/9 strapping, 20/21 UART0, and 0/1/3/4 are the only ADC1 channels (ADC2 is unusable with Wi-Fi up). Note the classic-ESP32 "avoid GPIO 6–11" rule does **not** apply to the C3. `GPIO 8` is the onboard LED, active low — wire it as a heartbeat, it's the only diagnostic you get without a ladder.

**Critical points:**

- **`HV` comes from the MCU's own 4.48 V rail, not from the 5 V buck.** Driving the MCU's input above its supply forward-biases its ESD diode. Tap at the MCU's decoupling cap; draw is under 1 mA.
- **`LV` = 3.3 V, `HV` = 4.48 V.** Swapping them puts 4.48 V on your GPIOs. Check the silkscreen twice.
- **Do not power the ESP32 from the fan's 4.48 V rail.** It was sized for an 8-bit MCU and an RF receiver, maybe 20 mA. The C3 bursts to ~350 mA.
- **Keep the 1 kΩ in the inject path** — pin protection during power sequencing, contention limiting, EMC, RF front-end isolation.
- Size any sniff divider so the high clears V<sub>IH</sub> = 2.475 V. 10k/22k gives 3.08 V; 10k/10k gives 2.24 V and is below spec.
- Mount resistors on the adapter board, not as flying leads. This thing vibrates for years.

### Power

Take power from the low-voltage DC bus (behind D6, with the 470 µF caps), not the logic rail. **The bus measures 24 V**, which is what makes an MP1584 usable here at all — it is a 28 V part, 30 V absolute maximum.

**That leaves about 4 V of headroom, and the load on the other end of the bus is a motor.** A decelerating BLDC pushes energy back, and the two 470 µF caps are what absorb it; how far the rail actually rises on a 6→off transition has [not been scoped](#still-open). If it clips 28 V the buck is out of spec, and above 30 V it fails — usually shorted, which puts 24 V on the ESP32. A part with real margin (an LM2596HV or a 60 V module) removes the question rather than answering it; a TVS or a zener clamp across the buck's input is the cheaper mitigation if the measurement comes back close.

**Keep the switcher away from the RF section**: shielded inductor, filtering both sides, mounted at the far end, and verify remote range after installation.

### Antenna

The generic C3 SuperMini boards have a documented antenna problem — the SMD antenna sits too close to the ground plane and shields rather than radiates; a quarter-wave wire mod is worth a reported 6–10 dB. Since the antenna has to leave the canopy anyway, **this project treats the mod as mandatory.** Remove the SMD antenna, solder a ~31 mm wire or a U.FL/SMA pigtail to the feed point, then **strain-relieve the joint** — epoxy at the pad, anchor the cable. A hand-soldered joint on a fixture that vibrates for years is a fatigue failure waiting to happen.

Verify with the `wifi_signal` sensor from the real canopy position before closing up. Better than −70 dBm and you'll never think about it again.

---

## Firmware

ESPHome. Two builds, both loading local components from `firmware/components`:

| Build | Needs | Does |
|---|---|---|
| `firmware/sniffer.yaml` | just the sniff tap — **no board modification** | Decodes the remote and prints it. Receive-only; this is the bring-up config. |
| `firmware/relay.yaml` | **both** modifications | The same decode, plus injection into the MCU's RF input, `fan_rf.send_key` for automations, the two LED channels as a `cwww` light driven from the remote's light keys, the [sunrise alarm](#sunrise-alarm), and a `web_server` page on the device. |

| Component | Does |
|---|---|
| `components/fan_rf` | The codec. **Capture → frames:** split on gaps over 5 ms, decode each chunk independently as 32 PWM bits, all-or-nothing. **Frame → packet:** split into prefix / key / counter / checksum and validate. **Packets → presses:** count identical codewords to tell a tap from a hold. Two more stages run it backwards for transmit. |
| `components/adc_logger` | Streams an ADC pin to the log continuously at 10 kSa/s, for looking at a pad as an analogue waveform. Wiring: `pad --[10k]-- GPIO3 --[10k]-- GND`. |

Every stage lives in `fan_rf/fan_rf_protocol.h`, free of ESPHome, Arduino and IDF dependencies, so it compiles on the host. `fan_rf.cpp` is plumbing.

The knobs are `relay.yaml`'s `substitutions:` block, documented in place.

### Two settings that matter

**`idle: 4ms`** is deliberately shorter than the 8.79 ms inter-frame gap, so each of the five repeats ends its own capture and a press is reported ~37 ms after its first frame instead of ~218 ms after the burst. A longer idle never terminates while a button is held — frames arrive every 41.1 ms — until `receive_symbols` fills and the driver truncates. The safe window is 865 µs (longest in-frame space) to 7.69 ms (preamble gap).

**`filter` must stay below 136 µs**, the shortest real symbol under worst-case AGC skew. At 200 µs the compressed early spaces merge away and frame 1 is destroyed. Don't reach for it to suppress noise either — noise median pulse width is 343 µs and overlaps the real symbols completely. `filter_symbols: 120` is the right tool: garbage bursts run to at most 95 symbols against a real packet's 166.

### Two defaults that reboot the fan

`api:` and `wifi:` each default to **`reboot_timeout: 15min`** — the first restarts the device when no API client has connected for that long, the second when the network has been unreachable. Both are `0s` in `relay.yaml`.

Otherwise, with no Home Assistant on the network, the ESP32 restarts **every 15 minutes, forever**, and each restart blanks both optocouplers for the few hundred ms the light takes to come back. It presents as an occasional flicker at *any* brightness, indistinguishable from a fault in the dim path. Two things hide it: attaching `esphome logs` is itself an API client, so watching stops it, and the `web_server` page does not count. `Uptime` back to seconds, alongside `Reset reason`, is the tell.

The defaults suit a sensor that exists to reach Home Assistant. Here the job that matters is relaying the remote, which never touches the network stack — and the cost of `0s` is that a wedged radio needs a power cycle at the wall.

### Press, hold and relay

The remote has no repeat code — a held button just resends the same frame, counter unchanged, every 41.1 ms. The first frame of a new codeword is the press; the next four are swallowed so a tap fires once (even when AGC eats frame 1 and only four arrive); everything past them is a hold. A run ends when the codeword changes or after `run_timeout` (250 ms), which is what stops the ninth press of a button — the counter wraps at 8 — reading as the same hold continuing.

Nothing is transmitted on the press event, because only what happens next says whether the button was tapped or held:

```
frame:    1   2   3   4   5   6   7   8
event:    P   .   .   .   .   R1  R2  R3
inject:   .   .   .   .   .   F   F   F      held  — starts at the first repeat
inject:   .   .   .   .   . F                tapped — starts at the deadline
```

`relay_decision` (230 ms) is that deadline and is pure added latency on every tap. It cannot go below `press_frames` × 41.1 ms = 205 ms. One received repeat every 41.1 ms against one injected frame occupying 41.4 ms keeps the stream in step — nothing queues, nothing overlaps.

**The ESP32 owns the transmitted counter.** With the `102` out the MCU hears only the ESP32, so there is one sequence reaching it. It advances once per injected press and holds for every frame of that press. It is not persisted across reboot: ESP32 and MCU share the fan's secondary rail, so a power cycle resets both, and they can only disagree after an ESP32-only restart — costing at most one ignored command.

`relay: fan` (the shipped setting) forwards only what the MCU still owns. `all` is a literal pass-through, for a board whose `R37`/`R38` are still fitted and where swallowing any key would break a working remote. Every button is classified by hand in `key_domain()` — `KEY` is a lookup table and nothing in a code predicts what it does:

| Domain | Keys | In `fan` mode |
|---|---|---|
| light | `bright ±`, `temp ±`, `light on/off`, `cycle full bright`, `night mode` | Stops at the ESP32, which drives the LEDs |
| fan | `fan 1`–`fan 6`, `fan off`, `fan forward`, `fan reverse` | Forwarded |
| both | `all off` | **Forwarded** — it has to reach the MCU to stop the fan — *and* handled locally to kill the light |
| user | `natural wind`, `2H`, `4H` | Never forwarded — see below |
| unknown | the 12 codes this remote has no button for | Forwarded; a code nobody has seen is not one to start swallowing |

**`natural wind`, `2H` and `4H` are claimed as spare buttons.** These are real fan functions — `natural wind` is a fan mode, and `2H`/`4H` are believed to be fan-off timers — that this build takes over because they are never used. They are withheld from the MCU, so their OEM behaviour is gone, and `relay.yaml` exposes each as a bare `Custom:` entity with no action attached, ready for an automation. Handing one back is a two-line change: move it to `DOMAIN_FAN` in `fan_rf_protocol.h` and drop its block from `relay.yaml`.

### Dimming

The `MT9722S` **gates rather than averages** its dim input — measured, since the PWM becomes visible as flicker below ~250 Hz. That is the good case: average light tracks duty, and what stops it is a minimum pulse *width*, not a minimum duty. So the lowest usable duty is `t_min × frequency`, which makes **frequency the dimming-depth knob and lower dimmer** — the opposite of the usual instinct. Dropping the carrier from the OEM's 1 kHz to 250 Hz makes the same pulse a quarter of the duty. `t_min` is [measured per optocoupler](#confirmed-on-hardware) — 3.4 µs on the warm channel, 3.9 µs on the cold — which is what puts `min_power` at 0.085% and 0.1% here.

What bounds the frequency from below: **visible flicker**, measured at ~250 Hz; **beating against the 100 Hz mains ripple** already on the LED current (CE5/CE6 are 2.2 µF feeding ~28 W a channel — nowhere near enough hold-up), so sit between harmonics rather than on one; and **stroboscopic beating against the blades**, per fan speed, [not yet checked](#still-open). 250 Hz sits exactly on the measured flicker floor and clears the harmonics by 50 Hz either side, but has no margin. The next stop up is ~350 Hz, at the cost of 40% more minimum duty.

The OEM's floor was a firmware choice, not an optocoupler artefact: 1 kHz is not fast, and its dimmest step was 7.4% duty — a 74 µs pulse, more than twenty times the shortest one the PC817 turns out to still pass.

### Host tooling

| Tool | Does |
|---|---|
| `tools/fan_rf_selftest.cpp` | Compiles the firmware's own header on the host and checks it: the key table against PROTOCOL.md, the checksum against every single-bit corruption, and the press/repeat state machine through synthesised bursts — clean, AGC-skewed, frame 1 destroyed, held, pure noise. Transmit is checked by feeding its output back through the receive path. |
| `tools/decode_fan_rf.py` | Decodes a captured log offline with generous tolerances, majority-votes repeats, and checks the counter increments by exactly one per press. |
| `tools/plot_adc.py`, `tools/adclog_to_csv.py` | Plot and convert `adc_logger` output. Dropped lines are reported and drawn as gaps, never closed up. |

```
c++ -std=c++17 -O2 -o /tmp/fan_rf_selftest tools/fan_rf_selftest.cpp
/tmp/fan_rf_selftest                       # 858 checks, no log needed
/tmp/fan_rf_selftest log.txt               # replay a capture
/tmp/fan_rf_selftest --dump log.txt        # what dump_frames would print
```

To record a new log, uncomment `dump: [raw]` in `sniffer.yaml` (both host tools parse those `Received Raw:` lines), flash, `esphome logs firmware/sniffer.yaml > log.txt`, then comment it out again — it prints a line per capture, noise included, and buries the frames it exists to show.

---

## Remote protocol

**Solved. [PROTOCOL.md](PROTOCOL.md) is the reference** — wire format, symbol timings, burst structure, the 32-bit frame (20-bit prefix, 5-bit key, 3-bit press counter, 4-bit checksum) and the key table for all 20 buttons.

Three properties shape everything else:

- **No rolling code.** The only state is a 3-bit press counter. Replay works; synthesising a command is a table lookup and a 4-bit XOR.
- **The AGC corrupts the first frame of every burst.** A decoder that only reads from the start of a capture — ESPHome's stock `rc_switch`, for one — loses the whole press. This is why `fan_rf` exists.
- **The line is never idle.** With nothing transmitting, the receiver's AGC output is amplified band noise, so validation must be on pulse timing and frame fields, never edge counting.

---

## Status

### Confirmed on hardware

- **The LED path works.** `R37`/`R38` out, 620 Ω from GPIO 7 and GPIO 10, 250 Hz, `min_power` 0.085% warm and 0.1% cold — and the light dims far below the OEM minimum, which was the point of the project. A 3.4 µs pulse against the OEM's dimmest 74 µs at four times the repetition rate: **0.085% duty against 7.4%, an 87× lower floor** on the warm channel, 75× on the cold.
- **`t_min` is 3.4 µs on one PC817 and 3.9 µs on the other, and the PC817 imposes it, not the `MT9722S`.** Swept each channel down at 250 Hz, one LEDC count (0.244 µs at the C3's 14 bits) at a time, from a temporary knob in Home Assistant: `R37` (warm) is steady at 14 counts — 3.42 µs, 0.085% — flickers at 13 (3.17 µs) and goes dark rather than dimmer at 12; `R38` (cold) is steady at 16 counts — 3.9 µs, 0.1% — and is dark at 15. Two optocouplers from the same reel, half a microsecond apart. The flicker at 13 is the edge itself: a pulse the opto passes on some cycles and not others. `min_power_r37` is **0.085%** and `min_power_r38` is **0.1%**, each on its own floor rather than above it. Nearly the whole range is in the last step: 1% down to 0.2% barely changes, then 0.2% → 0.1% does almost all the dimming — the signature of a switching-time limit rather than a duty one. Being a pulse width, it scales with the carrier, so what is left of the dimming range lives in `pwm_frequency`, and [flicker](#dimming) is what caps that.
- **The RF relay works in both directions.** The remote's light keys reach the decoder and drive the light; its fan keys are reconstructed and injected into the OEM MCU, which accepts them.
- Optocouplers are non-inverting from the ESP32's side (`led_inverted: "false"`); **`R38`/GPIO 10 is cold, `R37`/GPIO 7 is warm**.
- **An occasional flicker at any brightness was the ESP32 rebooting itself**, not the analogue path — `api:` and `wifi:` both default to `reboot_timeout: 15min`. See [Two defaults that reboot the fan](#two-defaults-that-reboot-the-fan).
- MCU output high 4.48 V, post-resistor 1.12 V, 3.36 mA. OEM dim PWM 1 kHz, lowest step 7.4% duty.
- **The low-voltage DC bus is 24 V** at rest, which is what the MP1584 runs from.
- Receiver output pad floors at 0 V unloaded, 0.8 V with a BSS138 attached (its pull-ups force ~620 µA in) — clears the MCU's 1.12 V V<sub>IL</sub>, not the ESP32's 0.825 V.
- Every button on the remote captured and decoded, all field checks passing.

### Inferred, not confirmed

- Component identifications from silkscreen and package markings; 433.92 MHz from the 13.52 MHz crystal.
- The third optocoupler is a mains-side → MCU sense path (from the 200 kΩ string at R33–R36).
- Blade deployment is centrifugal, the gear ring synchronising the four blades and ~10 cm springs providing retraction.

### Still open

- [ ] **The sunrise alarm against a real morning.** The config validates and builds, and `Sunrise: test` exercises the whole curve, but nothing has yet armed off a phone alarm and run the full half hour unattended. First thing to check is that the clock is actually set — `sntp` needs a route out of the LAN and `homeassistant` needs the VPN up.
- [ ] **Stroboscopic beating between the 250 Hz dim PWM and the blades**, at each of the six fan speeds. The one thing that could force the carrier back up.
- [ ] **Whether 250 Hz is flicker-free for other people.** It is the measured threshold, not a value with margin.
- [ ] **How far the 24 V bus rises when the motor decelerates.** The MP1584 is a 28 V part with a 30 V absolute maximum, so the headroom is ~4 V and the failure mode is not graceful — see [Power](#power). Scope the bus across a 6→off transition at the buck's input.
- [ ] **Whether the MCU dedupes on the press counter at all.** The counter policy is built on what the *remote* does; if the MCU ignores the field, the policy costs nothing either way. (A single injected frame *is* enough — taps relay through at `tap_frames: 1`.)
- [ ] Whether the 20-bit prefix is a per-remote ID, a protocol constant, or both. Separating them needs a second remote — until then, do not assume a synthesised frame is accepted by any other unit.
- [ ] LED channel forward voltage and current, per channel; OEM dimming *maximum* duty; `4614` pinout; the third optocoupler's function.

---

## Control

**Only the handheld remote has actually been exercised.** Everything below is configured but unused so far.

`web_server:` in `relay.yaml` serves `http://smarterfan-relay.local/` with a control for every entity — no server, no hub, nothing to maintain, and it is the whole interface if nothing else is set up. State only, no automations, unauthenticated unless `auth:` is added.

**Home Assistant** is the richer option and essentially free from ESPHome: a `light` with brightness and colour temperature, a `fan` with the six OEM speeds, and the handful of entities the [sunrise alarm](#sunrise-alarm) needs. Note the CCT slider is one-dimensional and normalised, so it cannot express every pair of channel levels — "warm at 100%, cool at 40%" is not a point on it. `light.control` with explicit `cold_white:`/`warm_white:` still reaches those. One entity is the right number; a second for the raw channels would be two lights fighting over the same hardware.

### The entities

| Entity | What it is |
|---|---|
| `light` **Light** | The two optocouplers as one CWWW light. Brightness and colour temperature, driven by the ESP32 — nothing here reaches the MCU |
| `fan` **Fan** | On/off, the six OEM speeds and direction, as `fan off` / `fan 1`–`fan 6` / `fan forward` / `fan reverse` injected into the MCU |
| `button` **Fan: …** | The nine fan keys individually — off, speeds 1–6, forward, reverse |
| `button` **Light: …** | The six light keys — brightness ±, warmer/cooler, full brightness, night mode |
| `binary_sensor` **Remote: …** | Momentary, one per light key: what the handheld remote is doing |
| `binary_sensor` **Custom: …** | `natural wind`, `2H`, `4H` — the three [claimed keys](#press-hold-and-relay), actionless and waiting for an automation |
| `datetime` **Alarm time** | What the [sunrise](#sunrise-alarm) runs up to. Written by Home Assistant from the phone's next alarm, and settable by hand from the web page |
| `switch` **Sunrise alarm** | Master enable. Off and nothing arms, whatever **Alarm time** says |
| `number` **Sunrise …** | Duration, end brightness, end colour temperature |
| `button` **Sunrise: …** | Test — the whole curve in a minute — and cancel |
| Diagnostic | Uptime, reset reason, Wi-Fi signal, sunrise progress |

**The fan entity is optimistic, and cannot be anything else.** The MCU reports nothing and the injection path is one-way into it, so the entity holds the last command anyone is known to have sent, not a reading. Two things keep that close to the truth: presses of the handheld remote are decoded here anyway, so they are mirrored into the entity as they go past, and each **Fan:** button transmits its key unconditionally — the entity itself sends nothing when asked for the state it already claims. Nothing is restored across a restart and nothing is transmitted at boot, so after an ESP32-only restart (an OTA, a watchdog) the entity reads off while the fan runs; any **Fan:** button, or one press of the remote, puts the two back together.

Speed and direction are separate keys, so a single call that changes both transmits two frames back to back — the second waits out the first, about 41 ms.

### Sunrise alarm

The light ramps from its dimmest and warmest up to full across the half hour
before your alarm goes off. **The fade runs on the ESP32**, and Home Assistant's
only job is to keep one `datetime` entity pointing at the phone's next alarm.

That is the same split as the RF relay, for the same reason. The alarm is set at
night, when the phone and Home Assistant are both awake and talking; the sunrise
happens hours later, when nothing but the ESP32 needs to be. A router that dies
at 03:00 costs nothing.

**The Android side is `AlarmManager`, by way of the Home Assistant Companion
app's `Next alarm` sensor** — an OS-level call that returns the next alarm
registered by *any* app, so Google Clock, the Samsung and Xiaomi clocks and
Sleep as Android all work with no per-app configuration and nothing installed
beyond the companion app most people already have. No webhook, no cloud, no
third-party service in the path. The sensor ships **disabled**;
**[docs/homeassistant.md](docs/homeassistant.md)** is the setup, the one
automation it needs, and the caveats.

Nothing about the fade is compiled in — duration, end brightness and end colour
temperature are `number` entities, and `Sunrise: test` runs the whole curve in a
minute so it can be judged without waiting for dawn.

| | |
|---|---|
| **Curve** | Linear in the light's brightness *state*, with deliberately nothing on top. The light's `gamma_correct` of 2.8 already bends output as p<sup>2.8</sup> — close enough to the cube law between luminance and perceived lightness that the ramp *looks* linear. A second curve would leave the first twenty minutes doing nothing visible. |
| **Colour** | Warmest → the end colour temperature, so the room reddens before it brightens. The 3800 K default is the **middle of the mireds range, where both channels reach full** and total output peaks. 6500 K is the cold channel alone: bluer, and about half the light. |
| **Steps** | One every 5 s, each with a 5 s transition, so the light interpolates continuously between them rather than stepping. That is ~360 state updates across a half hour where 1 Hz would be 1800, at identical smoothness. |
| **Ending one** | Any brightness or colour change during a fade cancels it — the remote, a button, a Home Assistant slider, the web page. There is no list of sources to maintain: they all arrive at the light's `on_state`, and what separates them from the fade's own steps is a shadow variable, the same trick the [fan entity](#the-entities) uses. |
| **Not starting one** | A sunrise will not start if the light is already on above its floor. An alarm has no business dimming a room somebody is sitting in. |
| **A reboot mid-fade** | Resumes at the right point rather than restarting or giving up. `Alarm time` is restored across a restart; the "already handled" marker deliberately is not. |
| **The clock** | `sntp` **and** `homeassistant`, both configured. Either alone is enough — both set the system clock — and neither is a given here, with Home Assistant off-site behind a VPN and no promise of a route to the internet on the LAN. |

The defaults, then, put a 30-minute fade here — `duty` being what the
optocoupler actually sees, after the light's gamma and the warm channel's
0.085% `min_power` floor:

| min | brightness state | duty | colour |
|---:|---:|---:|---:|
| 0 | 0.010 | **0.09%** — the measured floor | 2700 K |
| 5 | 0.175 | 0.84% | 2837 K |
| 10 | 0.340 | 4.96% | 2988 K |
| 15 | 0.505 | 14.84% | 3157 K |
| 20 | 0.670 | 32.64% | 3346 K |
| 25 | 0.835 | 60.39% | 3558 K |
| 30 | 1.000 | 100% | 3800 K |

Barely there for the first ten minutes, most of the work in the last third.
That is the gamma doing it, not a curve in the config.

One thing has to be got right: **`timezone:` in `relay.yaml` and Home
Assistant's own timezone must name the same zone.** The device stores wall-clock
time, not an epoch. They disagree silently, and the symptom is a sunrise a whole
number of hours out.

### Matter

**Matter** would let Apple/Google/Alexa drive the fan without HA, but **ESPHome has no Matter component** (checked against 2026.8.0, which ships `openthread` and nothing that speaks Matter), so it is a different firmware stack rather than a config change — and Matter still needs a commercial hub as controller. The C3 is Wi-Fi only in any case; Thread would need a C6 or H2.

---

## Reverting to stock

Every modification is a resistor removal:

1. Re-fit `R37` and `R38` (1 kΩ, 0805)
2. Re-fit the `102` between the RF receiver output and the MCU input
3. Remove the adapter board

Keep the removed resistors. Buy spares.

---

## Out of scope

**Direct BLDC motor control.** Running slower than the lowest OEM speed means bypassing the MCU and driving `U4`–`U6` yourself — six PWM outputs with hardware dead-time, which the C3 cannot do (zero MCPWM units; an S3 or C6 would be needed). There is also a mechanical floor no controller beats: blade deployment is centrifugal, so below some RPM the springs win and the blades retract. The gear ring keeps that symmetric rather than dangerous, but the usable range is only the hysteresis band between deploy speed and hold speed.

**Firmware for the OEM MCU.** `MT006MAPN` is an undocumented mask-ROM part — no toolchain, no reflash path. This is why the project works by interception rather than replacement.

---

## License

TBD.

## Disclaimer

Provided as-is, for reference. Mains voltage, an overhead fixture, and an uncertified modification. If you build this, you accept the consequences.
