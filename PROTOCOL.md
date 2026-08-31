# SmarterFan RF Remote Protocol

## 1. Frame layout

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

One value per physical button, from the fixed table in §3.

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

## 2. Transmission behaviour

* Each press sends the same frame **exactly 5 times**. A burst that decodes as fewer than 5 frames means the decoder lost
  some to noise, not that the remote sent fewer.
* **Holding the button down** keeps the burst going: the remote repeats the *original*
  frame indefinitely, with `CNT` unchanged, until the button is released. A hold is
  therefore just a burst longer than 5 frames — there is no separate repeat code and
  no way to distinguish a hold from a long burst other than by its length.
* All repeats, held or not, are bit-identical (same `KEY` *and* same `CNT`).
* No rolling code, no encryption, no per-frame nonce. The only state is the 3-bit
  counter.

## 3. Key table

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
