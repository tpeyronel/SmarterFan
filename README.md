# SmarterFan

Replacing the brain of a **Novohome NH-VTR500** ceiling fan (retractable blades, dimmable CCT LED, 433 MHz remote) with an ESP32, without touching its mains-side power electronics.

The stock controller works, but its firmware is locked behind an undocumented mask-ROM MCU: the lowest brightness step is still uncomfortably bright, there are only three fixed colour temperatures, and there is no way to talk to it other than the handheld remote. This project intercepts the two control paths that matter and hands them to an ESP32, while leaving the mains rectifier, the LED drivers, the isolation barrier and the motor power stage exactly as the manufacturer built them.

---

## ⚠️ Safety

**This board carries mains voltage and hangs over your head.**

- Roughly the left third of the PCB is mains-referenced. Treat every component in that region as live whenever the fan is plugged in.
- The bulk capacitors are rated 450 V and hold charge after disconnection. **Bleed them and verify with a meter before touching anything.**
- Do not connect an oscilloscope ground clip to the mains-referenced section. Use an isolation transformer, or a differential/isolated probe.
- Do not connect USB to the ESP32 while the board is powered from mains. The secondary is isolated but coupled to the line through the Y-capacitors (CY1, CY2), so it can float at a significant voltage relative to earth. Use an isolation transformer, a USB isolator, or flash over OTA.
- This modification is not certified, not fire-tested, and voids any warranty and possibly your insurance. **You are responsible for what you hang from your ceiling.**

If any of the above is unfamiliar, this is not a good first electronics project.

---

## Goals

| Goal | Status in this scope |
|---|---|
| Dim the LEDs far below the OEM minimum, down to off | ✅ In scope |
| Continuous colour temperature instead of 3 fixed tones | ✅ In scope |
| Control from Home Assistant | ✅ In scope |
| Expose as a standard fan + light via Matter / HomeKit | ✅ In scope |
| Sunrise / light alarm | ✅ In scope |
| Keep the original handheld remote working | ✅ In scope |
| **Slow the fan below its lowest OEM speed** | ❌ **Not in this scope** — see [Out of scope](#out-of-scope) |

**Be clear on that last row.** This project routes fan commands *through* the stock MCU, so you get the OEM's six speeds from your phone instead of only from the remote. The lowest speed is still the lowest speed. Going below it requires taking over the motor drive directly, which is a separate effort.

---

## Target hardware

- **Fan:** Novohome NH-VTR500 — 4 retractable blades, 30 W DC motor, 6 speeds, 56 W LED with 3 colour tones
- **Control board silkscreen:** `XH-FSD03D5-1A`, dated `2025-09-19`

Other fans may use the same board; the silkscreen is the thing to match. If yours differs, the *approach* still applies but every reference designator below will be wrong.

---

## How the stock board works

Reverse-engineered from photographs and probing. Confidence is marked per row — see [Verification status](#verification-status).

| Ref | Part | Function |
|---|---|---|
| U3 | `MT006MAPN 5702S` | Main MCU, TSSOP-28. No public datasheet, no reflash path. |
| U4, U5, U6 | `4614 GA8T25` ×3 | One half-bridge per motor phase (U/V/W) |
| U10, U11 | `MT9722S` | Mains-side LED drivers, one per colour channel |
| U2 (×3) | `PC817` | Optocouplers across the isolation barrier |
| Q1 | `WG D4N65SE` | 650 V MOSFET, primary switch of the main flyback |
| T1 | (red transformer) | Main flyback — the isolation barrier |
| D6 | `WG MBRD10200CT` | Secondary rectifier for the motor bus |
| Y1 + SOP-8 | `13.52 MHz` | 433 MHz superheterodyne receiver, feeds the `ANT` wire |
| R37, R38 | `102` (1 kΩ) | MCU → dimming optocoupler drive resistors |
| R33–R36 | `204` (200 kΩ) | High-value string feeding the third (mains-sense) optocoupler |
| R51, R52, R53 | `R050`, `R100` | Motor current-sense shunts |
| CE5, CE6 | 2.2 µF 450 V | Bulk caps for the two LED drivers |

### Block diagram

```mermaid
flowchart TB
    REM([433 MHz remote]) -.->|RF| RX[OEM RF receiver<br/>SOP-8 + Y1]
    RX -->|data| MCU[OEM MCU<br/>MT006MAPN]
    MCU -->|PWM ×2| OPT[2× PC817]
    MCU --> DRV[U4–U6 half-bridges]
    OPT --> LDRV[U10 / U11<br/>LED drivers]
    DRV --> MOT([BLDC motor, 30 W])
    LDRV --> LEDS([LED module, 56 W])
    MAINS([AC mains]) --> DB1[DB1 bridge<br/>~310 V DC]
    MAINS -.->|isolation barrier| PSU
    DB1 --> LDRV
    DB1 --> PSU[Q1 + T1 flyback]
    PSU --> BUS([Low-voltage DC bus])
    BUS --> DRV
```

Two facts drive the entire design of this project:

1. **The remote is RF, not IR.** A 13.52 MHz crystal next to an SOP-8 and a wire soldered to the `ANT` pad is a 433.92 MHz superhet receiver. No line of sight involved.
2. **The dimming floor is a firmware limit, not a hardware one.** The MCU sends PWM through two optocouplers into the LED drivers' dim inputs. Nothing in the analogue path forbids going lower — Novohome simply chose not to.

---

## How the modification works

Two independent interventions, both reversible.

### 1. LED dimming — take over completely

Remove `R37` and `R38`. This isolates the MCU from the two dimming optocouplers, leaving both resistor pads floating. The ESP32 then drives the optocoupler LEDs directly through its own series resistors.

Measured on the stock board:

```
MCU pin high level      4.48 V
After the 1 kΩ          1.12 V   (PC817 forward drop)
Drive current           (4.48 − 1.12) / 1000 = 3.36 mA
```

To reproduce that from a 3.3 V GPIO:

```
R = (3.3 − 1.12) / 0.00336 ≈ 650 Ω  →  620 Ω
```

The MCU keeps driving its now-disconnected pins. Harmless.

### 2. RF — relay through the ESP32

Remove the `102` resistor between the RF receiver's output and the MCU's input. The ESP32 then sits in the middle:

- **Receives** by tapping the RF receiver's output through a level shifter and decoding the OOK stream in software.
- **Transmits** by reconstructing packets and injecting them into the MCU's input, which cannot tell the difference.

This gives app control of the fan without touching the motor drive, and keeps the physical remote working — every command it sends is decoded by the ESP32 and either forwarded to the MCU (fan) or acted on directly (light).

```mermaid
flowchart TB
    REM([433 MHz remote]) -.->|RF| RX[OEM RF receiver]
    HA([Home Assistant / Matter]) <-->|Wi-Fi| ESP
    RX -->|shifter ch2<br/>4.48 → 3.3 V| ESP[ESP32-C3]
    ESP -->|shifter ch1<br/>3.3 → 4.48 V| MCU[OEM MCU]
    ESP -->|LEDC PWM ×2| OPT[2× PC817]
    MCU --> DRV[U4–U6 half-bridges]
    DRV --> MOT([BLDC motor])
    OPT --> LDRV[U10 / U11 LED drivers]
    LDRV --> LEDS([LED module])
```

**Tradeoff, stated plainly:** in relay mode the ESP32 becomes a single point of failure for the remote. If it hangs, the remote stops working entirely. Watchdog it. (Wi-Fi dropping out is *not* a problem — the relay runs on the RMT peripheral and never touches the network stack.)

---

## Hardware

### Bill of materials

| Item | Notes |
|---|---|
| **NodeMCU ESP32-C3 SuperMini** | The chosen board. Requires the antenna mod — see [antenna](#antenna). |
| BSS138 4-channel level shifter module | The ubiquitous $1 bidirectional I²C shifter. Two channels used. |
| Buck converter, **≥60 V input** → 5 V | ⚠️ MP1584 modules are 28 V max and are **not** suitable. |
| 2× 620 Ω resistor | Optocoupler drive |
| 1× 1 kΩ resistor | Series protection on the RF inject line |
| 1× 220–470 µF, **105 °C or polymer** | Bulk on 3V3. Standard 85 °C parts dry out in a hot canopy. |
| 1× 100 nF ceramic | HF decoupling, mounted at the module pins |
| Header sockets | Socket the ESP32 — don't solder it down |
| U.FL → SMA pigtail + 2.4 GHz antenna | Route it out of the canopy |

### Tools

Oscilloscope (not optional — you cannot do the RF work without one), multimeter, fine-tip soldering iron, hot air or tweezers for 0805 removal, **isolation transformer** (strongly recommended), USB isolator if flashing over USB while powered.

### Wiring

```
 LV DC bus ──[ buck, ≥60 V in → 5 V ]──→ ESP32 5V pin
                                            │
                                        onboard LDO
                                            │
                                       ESP32 3V3 ──→ shifter LV

 MCU VDD (4.48 V) ───────────────────────────────→ shifter HV

 secondary GND ───┬── ESP32 GND
                  └── shifter GND

 GPIO 7  ──[620 Ω]──→ opto-side pad of R37   (LED channel A)
 GPIO 10 ──[620 Ω]──→ opto-side pad of R38   (LED channel B)
 GPIO 5  ──→ shifter LV1 ─── HV1 ──[1 kΩ]──→ MCU RF input pad
 GPIO 6  ←── shifter LV2 ─── HV2 ←───────────  RF receiver output pad
```

**Critical points:**

- **`HV` comes from the MCU's own 4.48 V rail, not from your 5 V buck.** Driving the MCU's input above its supply forward-biases its ESD diode. Tap HV at the MCU's decoupling capacitor. Current draw is under 1 mA — the rail can handle it.
- **`LV` = 3.3 V, `HV` = 4.48 V.** Swapping the supply pins puts 4.48 V on your ESP32 GPIOs. Check the silkscreen twice.
- **Do not power the ESP32 from the fan's 4.48 V rail.** It was sized for an 8-bit MCU and an RF receiver, perhaps 20 mA. The C3 bursts to ~350 mA.
- **Keep the `1 kΩ` in the inject path.** It exists on the stock board for pin protection during power sequencing, contention limiting, EMC, and RF front-end isolation. All four reasons still apply to you.
- Mount your resistors on the adapter board, not as flying leads or tombstoned parts. This thing vibrates for years.

### Pin assignment (ESP32-C3 SuperMini)

| Signal | GPIO | Direction | Connects to |
|---|---|---|---|
| RF inject | **5** | out — RMT TX | shifter `LV1` → `HV1` → 1 kΩ → MCU RF input pad |
| RF sniff | **6** | in — RMT RX | shifter `LV2` → `HV2` → RF receiver output pad |
| LED channel A dim | **7** | out — LEDC | 620 Ω → opto-side pad of `R37` |
| LED channel B dim | **10** | out — LEDC | 620 Ω → opto-side pad of `R38` |
| Supply in | `5V` | — | buck output |
| Ground | `GND` | — | fan secondary GND, common with shifter and MCU |
| 3.3 V reference | `3V3` | out | shifter `LV` pin (~1 mA, reference only) |

Verify against your board's silkscreen — SuperMini clones vary.

#### Why these four

The board exposes GPIO 0–10, 20 and 21. Note that the classic-ESP32 "avoid GPIO 6–11" rule **does not apply** to the C3 — here the flash occupies 11–17.

| Pins | Why they're excluded |
|---|---|
| 11–17 | Internal flash |
| 18, 19 | USB D−/D+ — not broken out, and using them kills USB-JTAG |
| 2, 8, 9 | Strapping. 9 is boot mode and the BOOT button, 8 is the onboard LED and must be high at boot |
| 20, 21 | UART0 — keep for serial logs during bring-up |
| 0, 1, 3, 4 | **ADC1 channels.** Reserved: ADC2 is unusable while Wi-Fi is up, so these are the only analog inputs available if the shunts are ever read |
| **5, 6, 7, 10** | **Nothing to lose.** 5 is ADC2-only (already ruined by Wi-Fi); 6, 7 and 10 have no alternate function |

The RF pair sits on adjacent pins (5, 6) because both terminate at the level shifter, whose `LV1`/`LV2` are adjacent — one clean two-wire run. The LED lines gain nothing from adjacency; each goes through its own resistor to a different pad.

Keeping the RF inject line off strapping pins matters most: they glitch at boot, and that garbage goes straight into the MCU. Non-strapping C3 pins are high-Z at reset, so the shifter's pull-ups simply hold the MCU input high — no transitions, nothing decoded, harmless.

Peripheral-to-pin binding is a non-issue: LEDC and RMT both route through the GPIO matrix, so any peripheral reaches any pin.

`GPIO 8` drives the onboard LED (active low) and is high at boot, so it is safe to use as a status indicator once firmware is running. Worth wiring into your firmware as a Wi-Fi/relay heartbeat — it's the only diagnostic you get without a ladder.

### Antenna

The generic "C3 SuperMini" boards have a documented antenna problem: the SMD antenna is not at its manufacturer's specified location and sits too close to the ground plane, which shields rather than radiates. Reported improvement from a quarter-wave wire mod is 6–10 dB.

Since a ceiling canopy is a poor RF environment anyway and the antenna needs routing outside it regardless, this is nearly moot — that antenna is getting replaced either way. **This project treats the antenna mod as mandatory, not optional.**

Remove the SMD antenna and solder a ~31 mm quarter-wave wire, or an SMA/U.FL pigtail, to the feed point. Then **strain-relieve the joint properly** — epoxy or hot glue at the pad, and anchor the cable so nothing tugs on it. A hand-soldered joint on a fixture that vibrates continuously for years is a fatigue failure waiting to happen, and this is the one place where the SuperMini is genuinely worse than a board with a proper U.FL connector.

Verify with ESPHome's `wifi_signal` sensor from the actual canopy position before closing it up. Better than −70 dBm and you'll never think about it again.

### Power

Take power from the low-voltage DC bus (the one behind D6 with the 470 µF caps), not the logic rail. Two cautions:

- **Measure the bus first** and spec the buck well above it — 60 V input rating covers regenerative spikes when the motor decelerates.
- **Keep the switcher away from the RF section.** You are adding a switching converter to a board with a sensitive 433 MHz front end that you still depend on. Shielded inductor, filtering on both sides, mounted at the far end. Verify remote range after installation.

---

## Firmware

**Platform not yet chosen.** The requirements are the same either way:

| Need | Peripheral |
|---|---|
| 2× dimming PWM | LEDC (6 channels available, 2 used) |
| 433 MHz OOK receive | RMT RX (C3 has 2) |
| 433 MHz OOK transmit | RMT TX (C3 has 2) |
| Wi-Fi + BLE | Built in |

Use RMT for the OOK rather than bit-banging. Once you do, the C3's single core is irrelevant — LEDC and RMT run autonomously and the Wi-Fi stack cannot disturb their timing.

### Platform options

| | Pros | Cons |
|---|---|---|
| **ESPHome** | Ready-made `ledc`, `remote_receiver`, `remote_transmitter`; native HA integration; OTA and light transitions for free | Less control; awkward if the project ever grows beyond stage 1 |
| **Arduino / PlatformIO** | Full control, large library ecosystem | You build your own API, OTA and HA/Matter integration |
| **ESP-IDF** | Most control, first-class peripheral access | Steepest; you build everything |

### Notes that apply regardless

- **The RF data line carries continuous noise when idle.** The receiver's AGC ramps to full gain with no transmitter present, so its output is amplified band noise. Decode with sync-word and pulse-timing validation, never edge counting. Real packets have quantised pulse widths; noise does not.
- **The dimming floor is set by the PC817, not by PWM resolution.** The optocoupler cannot pass arbitrarily narrow pulses, so the practical minimum duty is where pulse width exceeds roughly 10–20 µs. 12-bit resolution already exceeds what the opto can use — don't chase bits.
- **Consider lowering the dim PWM frequency.** If the OEM runs it fast, narrow low-duty pulses may be getting swallowed by the optocoupler — which would mean the stock brightness floor is an optocoupler artefact. Sweep frequency and duty and find out. Stay above ~1 kHz to avoid stroboscopic beating against the blades.
- **Watchdog the relay path.** It is the only route from the remote to the fan once the `102` is removed.

---

## App

No custom app. Two standard integrations:

**Home Assistant** — the primary interface. A `light` entity with brightness and colour temperature, a `fan` entity with the six OEM speeds, and HA automations for the sunrise alarm. If firmware lands on ESPHome this is essentially free.

**Matter** — expose the same fan and light so Apple Home, Google Home and Alexa can drive them without going through HA. Note the C3 is Wi-Fi only, so this is Matter-over-Wi-Fi; Thread would need a C6 or H2.

The light alarm runs as a brightness transition and only needs occasional NTP sync, so it survives Wi-Fi being intermittent.

---

## Verification status

Honesty about what is actually known, since someone may try to rebuild this.

### ✅ Measured

- `R37`, `R38` = 1 kΩ, MCU → two optocouplers
- MCU output high = 4.48 V; post-resistor = 1.12 V; drive current = 3.36 mA
- RF receiver SOP-8 output → 1 kΩ → MCU input
- Continuous activity on the RF data line with no button pressed
- Blade mechanism contains gears **and ~10 cm springs**

### 🔶 Inferred, not confirmed

- Component identifications from silkscreen and package markings
- 433.92 MHz operation (from the 13.52 MHz crystal)
- Third optocoupler is a mains-side → MCU sense path (from the 200 kΩ string at R33–R36)
- Blade deployment is centrifugal, with the gear ring synchronising the four blades and the springs providing retraction
- The idle RF line activity is receiver AGC noise, not a real transmission

### ❓ Unknown — measure before building

- [ ] **Low-voltage DC bus voltage.** Everything about the buck depends on it. Nothing in this repo has measured it.
- [ ] LED channel forward voltage and current, per channel
- [ ] OEM dimming PWM frequency and duty range
- [ ] Whether a pull-down exists on the MCU's RF input pin. If one does, it fights the shifter's 10 kΩ pull-up and the high level will not clear V<sub>IH</sub> — use a push-pull `74HCT1G34` instead.
- [ ] `4614` pinout
- [ ] Third optocoupler's actual function
- [ ] The remote's protocol and command set

### Pre-flight checks

Before trusting the level shifter, with the `102` removed and the ESP32 GPIO held high:

```
Measure the MCU RF input pin.
  ~4.48 V  →  no pull-down, you're clear
  ~2.2 V   →  pull-down present, switch to a push-pull buffer
```

Then capture a real packet off the receiver output, replay yours, and overlay the two traces. Matching timings and a clean 0.7 V → 4.48 V swing means the MCU will accept it.

---

## Reverting to stock

Every modification is a resistor removal. To undo:

1. Re-fit `R37` and `R38` (1 kΩ, 0805)
2. Re-fit the `102` between the RF receiver output and the MCU input
3. Remove the adapter board

Keep the removed resistors. Buy spares.

---

## Out of scope

**Direct BLDC motor control.** Running the fan slower than its lowest OEM speed means bypassing the stock MCU and driving `U4`–`U6` yourself — six PWM outputs with hardware dead-time, which the ESP32-C3 cannot do (it has zero MCPWM units; an S3 or C6 would be required).

There is also a mechanical limit that no controller can beat. Blade deployment is centrifugal, so below some RPM the springs win and the blades retract. The gear ring makes that retraction symmetric rather than dangerous, but it is a real floor. The usable range is the hysteresis band between the deploy speed and the hold speed — measure both before assuming there is headroom.

**Firmware for the OEM MCU.** `MT006MAPN` is an undocumented mask-ROM part. There is no toolchain and no reflash path. This is why the project works by interception rather than by replacement.

---

## Status

🚧 Early. Reverse engineering largely done, no hardware built yet. See [Unknown](#-unknown--measure-before-building) for the measurements blocking the first build.

## License

TBD.

## Disclaimer

Provided as-is, for reference. Mains voltage, an overhead fixture, and an uncertified modification. If you build this, you accept the consequences.
