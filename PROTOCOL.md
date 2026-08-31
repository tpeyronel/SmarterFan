# SmarterFan RF Remote Protocol

## 1. Wire format

Plain OOK at 433.92 MHz. The OEM receiver demodulates, so what appears at its
output pad is a baseband two-level signal. Bits are PWM-coded on a single tick:

```
tick   = 252 us
bit 0  = 1 tick mark + 3 tick space   (252 / 756 us)
bit 1  = 3 tick mark + 1 tick space   (756 / 252 us)
period = 4 ticks = 1008 us
```

Measured over 10592 symbols the bit period is **1009.1 µs, sd 3.3 µs**. That is a
crystal. The period is rigid; only the *split* between mark and space moves.

### Burst structure

One press transmits:

```
[335 µs mark][7.69 ms gap]                            preamble, once
5 × [32 data bits][~330 µs stop mark][8.79 ms gap]    the frame, repeated
```

The inter-frame gap is 8787 µs ± 10 µs over 120 measurements. Total airtime for
one press is ≈ 215 ms, and a held button emits one frame every 41.1 ms.

The longest space *inside* a decodable frame is 865 µs, so any split threshold
between roughly 1 ms and the 7.69 ms preamble gap separates a burst into frames
cleanly. `fan_rf` splits at 5 ms.

### AGC skew

The receiver's AGC is at full gain when a burst arrives and takes about two
frames to settle. While it settles it stretches marks and eats spaces:

|                   | frame 1 | frame 2 | frame 3 | frame 5 |
|-------------------|---------|---------|---------|---------|
| short mark, mean  | 303 µs  | 278 µs  | 270 µs  | 266 µs  |
| short space, mean | 209 µs  | 231 µs  | 240 µs  | 244 µs  |
| short space, min  | 136 µs  | 175 µs  | 208 µs  | 216 µs  |

The bias is common-mode — `mark − space` runs +89 µs in frame 1 against +22 µs in
frame 5, on both the short and the long pair — so it cancels in `mark + space`
and leaves the period untouched. De-biased, every frame position recovers
255/754 µs.

Three consequences for a receiver:

* **Match the period, not the widths.** Matching each pulse loosely (252/756 µs
  at 50%, a sanity rail) and the bit period tightly (1009 µs at 5%, about 15 σ)
  decodes frame 1. Four absolute width windows cannot: the short space alone
  spans 136–312 µs, a 39% spread that no choice of nominal covers at a sane
  tolerance.
* **Decode every frame independently.** Frame 1 is the corrupted one. A decoder
  that only attempts a decode from the start of a capture throws away four good
  frames behind one bad one.
* **Any glitch filter must sit below 136 µs**, the shortest real symbol under
  worst-case skew. At 200 µs the AGC-compressed early spaces fall under the
  threshold and are merged away, destroying frame 1 outright.

### Idle noise

With no transmitter present the AGC ramps to full gain and the pad carries
amplified band noise continuously — the line is not idle between presses.
Validate on pulse timing and on the frame fields below; never on edge counting.
Real symbols are quantised to the tick, noise is not.

## 2. Frame layout

32 bits, MSB first, exactly as printed by the dumper:

```
 bit  31                                  11        6      3   0
      ┌──────────────────────┬───────────┬─────────┬────────────┐
      │        prefix        │    KEY    │   CNT   │     CS     │
      │       20 bits        │  5 bits   │ 3 bits  │   4 bits   │
      └──────────────────────┴───────────┴─────────┴────────────┘
       10100001110110000010     kkkkk       ccc        ssss
       └───── 0xA1D82 ──────┘
```

| Field    | Bit range (numeric) | Position in the dumped string | Width | Meaning |
|----------|---------------------|-------------------------------|-------|---------|
| `prefix` | 31..12              | chars 0–19                    | 20    | constant `0xA1D82` |
| `KEY`    | 11..7               | chars 20–24                   | 5     | which button |
| `CNT`    | 6..4                | chars 25–27                   | 3     | press counter |
| `CS`     | 3..0                | chars 28–31                   | 4     | checksum |

```
word = (0xA1D82 << 12) | (KEY << 7) | (CNT << 4) | CS
```

### prefix — 20 bits, constant

Always `0xA1D82`. Transmitter/system identity, possibly with a fixed protocol or
device-type field folded in. A single-remote capture cannot split "remote ID" from
"protocol constant" — that needs a second remote.

### KEY — 5 bits, the button

One value per physical button, from the fixed table in §4.

### CNT — 3 bits, press counter

`CNT` increments by one on every *button press* (not every repeat frame) and wraps at
8. All repeats of a single press carry the same `CNT`; pressing the same button twice
in a row produces two *different* 32-bit words.

The counter is free-running and persistent in the remote: it is not reset per button
and it survives idle time.

Consequences for a receiver:

* Do **not** dedupe on the whole word. Dedupe on `KEY` within a burst, and treat a
  change in `CNT` as a genuine new press — that is exactly what the counter is for
  (it lets the fan tell "held/repeated frame" from "pressed again").
* Do **not** treat `KEY|CNT` as an 8-bit command code. There are 8 valid words per
  button.
* A `CNT` gap larger than one between consecutive received presses means presses were
  transmitted but missed by the receiver.

### CS — 4-bit checksum

```
CS = (word >> 8 & 0xF) ^ (word >> 4 & 0xF) ^ 0x6
   = (KEY >> 1) ^ (((KEY & 1) << 3) | CNT) ^ 0x6
```

i.e. XOR the two payload nibbles together with the constant `0x6`. Equivalently, four
positional parity equations over the dumped string:

```
char20 ^ char24 ^ char28 = 0      (nibble bit 3, 0x6 bit 3 = 0)
char21 ^ char25 ^ char29 = 1      (nibble bit 2, 0x6 bit 2 = 1)
char22 ^ char26 ^ char30 = 1      (nibble bit 1, 0x6 bit 1 = 1)
char23 ^ char27 ^ char31 = 0      (nibble bit 0, 0x6 bit 0 = 0)
```

Receive-side validation: `(n5 ^ n6 ^ n7) == 0x6`, where `n5..n7` are the last three
nibbles of the word.

## 3. Transmission behaviour

* Each press sends the same frame **exactly 5 times**. A burst that decodes as fewer than 5 frames means the decoder lost
  some to noise, not that the remote sent fewer.
* **Holding the button down** keeps the burst going: the remote repeats the *original*
  frame indefinitely, with `CNT` unchanged, until the button is released. A hold is
  therefore just a burst longer than 5 frames — there is no separate repeat code and
  no way to distinguish a hold from a long burst other than by its length.
* All repeats, held or not, are bit-identical (same `KEY` *and* same `CNT`).
* No rolling code, no encryption, no per-frame nonce. The only state is the 3-bit
  counter.

## 4. Key table

`CNT=0 frame` is the full 32-bit word for that button with the counter at zero; the
other seven words for the same button follow from `CNT` and the checksum.

| KEY | KEY (bin) | Button | CNT=0 frame |
|---|---|---|---|
| ` 3` / `0x03` | `00011` | bright + | `0xA1D8218F` |
| ` 4` / `0x04` | `00100` | fan forward | `0xA1D82204` |
| ` 5` / `0x05` | `00101` | bright - | `0xA1D8228C` |
| ` 6` / `0x06` | `00110` | all off | `0xA1D82305` |
| ` 7` / `0x07` | `00111` | temp + | `0xA1D8238D` |
| ` 8` / `0x08` | `01000` | light on/off | `0xA1D82402` |
| ` 9` / `0x09` | `01001` | 2H | `0xA1D8248A` |
| `10` / `0x0A` | `01010` | fan 4 | `0xA1D82503` |
| `11` / `0x0B` | `01011` | temp - | `0xA1D8258B` |
| `12` / `0x0C` | `01100` | fan 6 | `0xA1D82600` |
| `13` / `0x0D` | `01101` | cycle full bright | `0xA1D82688` |
| `15` / `0x0F` | `01111` | fan 5 | `0xA1D82789` |
| `16` / `0x10` | `10000` | fan 1 | `0xA1D8280E` |
| `17` / `0x11` | `10001` | fan reverse | `0xA1D82886` |
| `18` / `0x12` | `10010` | fan 2 | `0xA1D8290F` |
| `19` / `0x13` | `10011` | night mode | `0xA1D82987` |
| `21` / `0x15` | `10101` | natural wind | `0xA1D82A84` |
| `22` / `0x16` | `10110` | fan off | `0xA1D82B0D` |
| `25` / `0x19` | `11001` | 4H | `0xA1D82C82` |
| `28` / `0x1C` | `11100` | fan 3 | `0xA1D82E08` |

Unused `KEY` values: `0, 1, 2, 14, 20, 23, 24, 26, 27, 29, 30, 31`.

`KEY` is a plain lookup table, not an encoding. Read the table; do not extrapolate it.
