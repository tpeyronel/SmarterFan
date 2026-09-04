// Novohome NH-VTR500 433 MHz OOK remote -- the whole decoder, and the relay
// that puts it back on the wire.
//
// Five stages, in order, each one strictly above the last:
//
//   1. raw capture -> frames    walk_frames(), decode_frame()
//        Split a capture on the inter-frame gap and turn each chunk into a
//        32-bit codeword. Knows about pulse widths and nothing else.
//   2. frame -> packet          decode_packet()
//        Split a codeword into prefix, key, press counter and checksum, and
//        say whether it is real. Knows about fields and nothing else.
//   3. packets -> presses       PressTracker
//        Count identical codewords to tell a tap from a held button.
//   4. packet -> waveform       build_preamble(), build_frame()
//        Stage 1 backwards: a key and a counter out as signed durations.
//   5. presses -> injection     RelayGate
//        Decide tap versus hold, then say what to put on the wire and when.
//
// Nothing lower reaches up: the frame decoder has no idea what a press is.
//
// The wire and frame formats are documented in PROTOCOL.md. In brief:
//
//   [~350us mark][7.69ms gap]                         preamble, once
//   then 5x [32 data bits][~330us stop mark][8.79ms gap]
//
//   31                                  11        6      3   0
//   +----------------------+-----------+---------+------------+
//   |        prefix        |    KEY    |   CNT   |     CS     |
//   |    20b, 0xA1D82      |    5b     |   3b    |     4b     |
//   +----------------------+-----------+---------+------------+
//
//   CS = (word >> 8 & 0xF) ^ (word >> 4 & 0xF) ^ 0x6
//
// Free of any ESPHome, Arduino or IDF dependency, so it compiles unchanged into
// the firmware and into tools/fan_rf_selftest.cpp. Anything that needs the
// framework lives in fan_rf.h.

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace smarterfan {
namespace fan_rf {

// --- stage 1: capture -> frames ---------------------------------------------

// Symbol timings in microseconds.
//
// The protocol is a single tick, 1:3 PWM -- the standard OOK remote encoding:
//
//     tick  = 252us
//     bit 0 = 1 tick mark + 3 tick space
//     bit 1 = 3 tick mark + 1 tick space
//     period = 4 ticks = 1008us, measured at 1009us
//
// Measured pulses do NOT sit on those values: the receiver stretches marks and
// eats spaces by the same amount (+17us / -16us on average, +89us in the first
// frame of a burst while the AGC settles). That bias is common-mode, so it
// cancels in mark+space and leaves the period untouched -- which is why the
// period is checked separately below and is by far the strongest test here.
static const uint32_t SHORT_US = 252;
static const uint32_t LONG_US = 756;

// Width slack, percent. Wide on purpose: with the period check carrying the
// selectivity, this is a sanity rail on each pulse rather than the primary
// discriminator. 50% is where the frame yield plateaus.
static const uint32_t WIDTH_TOLERANCE_PCT = 50;

// Bit period, and the real discriminator. Measured mean 1009.1us with a
// standard deviation of 3.3us over 10592 symbols, so +/-5% is about 15 sigma
// and cannot reject a real frame. On noise, where mark and space are
// uncorrelated, the sum wanders freely and this rejects most pairs outright:
// measured against the noise captures it accepts 2.2% of pairs where the width
// windows alone accept 7.3%.
static const uint32_t PERIOD_US = 1009;
static const uint32_t PERIOD_TOLERANCE_PCT = 5;

// A space longer than this ends a frame. The inter-frame gap is 8787us +/- 10
// and the preamble gap is 7.69ms; the longest in-frame space is a ~700us zero.
static const uint32_t FRAME_GAP_US = 5000;

static const uint32_t PACKET_BITS = 32;

struct FrameWalk {
  uint16_t chunks;   // chunks between gaps, preamble included
  uint16_t decoded;  // chunks that yielded 32 clean bits
};

// Percentage-tolerance match, same convention as remote_base.
inline bool timing_matches(int32_t value, uint32_t nominal) {
  if (value < 0)
    value = -value;
  const uint32_t v = (uint32_t) value;
  return v >= (100 - WIDTH_TOLERANCE_PCT) * nominal / 100 &&
         v <= (100 + WIDTH_TOLERANCE_PCT) * nominal / 100;
}

// mark + space, the quantity the receiver's duty-cycle skew cancels out of.
inline bool period_matches(int32_t mark, int32_t space) {
  const uint32_t total = (uint32_t) (mark - space);  // space is negative
  return total >= (100 - PERIOD_TOLERANCE_PCT) * PERIOD_US / 100 &&
         total <= (100 + PERIOD_TOLERANCE_PCT) * PERIOD_US / 100;
}

inline bool is_frame_gap(int32_t value) { return value < -(int32_t) FRAME_GAP_US; }

// Decode one frame: 32 (mark, space) pairs, MSB first, optionally followed by a
// lone stop mark. All-or-nothing -- one symbol matching neither bit rejects the
// whole frame, which is what keeps a corrupt frame from being reported as a
// half-guessed word.
inline bool decode_frame(const int32_t *frame, size_t len, uint32_t *out) {
  if (len < PACKET_BITS * 2)
    return false;
  uint32_t bits = 0;
  for (size_t i = 0; i < PACKET_BITS * 2; i += 2) {
    const int32_t mark = frame[i];
    const int32_t space = frame[i + 1];
    if (mark <= 0 || space >= 0)
      return false;  // marks are positive, spaces negative -- never both
    if (!period_matches(mark, space))
      return false;
    if (timing_matches(mark, LONG_US) && timing_matches(space, SHORT_US)) {
      bits = (bits << 1) | 1;
    } else if (timing_matches(mark, SHORT_US) && timing_matches(space, LONG_US)) {
      bits = bits << 1;
    } else {
      return false;
    }
  }
  *out = bits;
  return true;
}

// Split a capture on inter-frame gaps and decode each chunk from its own start,
// calling on_frame(bits) for every one that comes out clean.
//
// Every chunk is tried, including the first: the preamble is a lone mark and
// fails the length check on its own, so there is no need to assume the capture
// begins at a burst boundary. That matters -- captures do start mid-burst.
template<typename F> inline FrameWalk walk_frames(const int32_t *data, size_t len, F &&on_frame) {
  FrameWalk walk = {0, 0};
  size_t start = 0;

  for (size_t i = 0; i <= len; i++) {
    if (i != len && !is_frame_gap(data[i]))
      continue;

    uint32_t bits;
    if (decode_frame(&data[start], i - start, &bits)) {
      walk.decoded++;
      on_frame(bits);
    }
    walk.chunks++;
    start = i + 1;
  }
  return walk;
}

// --- stage 2: frame -> packet -----------------------------------------------

// Bits 31..12, constant across every frame from this remote. Transmitter
// identity, possibly with a protocol constant folded in -- a single-remote
// capture cannot separate the two.
static const uint32_t PREFIX = 0xA1D82;

// The three payload nibbles XOR to this. See PROTOCOL.md section 3.
static const uint8_t CHECK_XOR = 0x6;

// KEY is a lookup table, not an encoding: the values are not contiguous and
// nothing about a code predicts its button. Do not extrapolate this.
// 0, 1, 2, 14, 20, 23, 24, 26, 27 and 29..31 are unused by this remote.
static const uint8_t KEY_BRIGHT_UP = 3;
static const uint8_t KEY_FAN_FORWARD = 4;
static const uint8_t KEY_BRIGHT_DOWN = 5;
static const uint8_t KEY_ALL_OFF = 6;
static const uint8_t KEY_TEMP_UP = 7;
static const uint8_t KEY_LIGHT_TOGGLE = 8;
static const uint8_t KEY_TIMER_2H = 9;
static const uint8_t KEY_FAN_4 = 10;
static const uint8_t KEY_TEMP_DOWN = 11;
static const uint8_t KEY_FAN_6 = 12;
static const uint8_t KEY_CYCLE_FULL_BRIGHT = 13;
static const uint8_t KEY_FAN_5 = 15;
static const uint8_t KEY_FAN_1 = 16;
static const uint8_t KEY_FAN_REVERSE = 17;
static const uint8_t KEY_FAN_2 = 18;
static const uint8_t KEY_NIGHT_MODE = 19;
static const uint8_t KEY_NATURAL_WIND = 21;
static const uint8_t KEY_FAN_OFF = 22;
static const uint8_t KEY_TIMER_4H = 25;
static const uint8_t KEY_FAN_3 = 28;

inline const char *key_name(uint8_t key) {
  switch (key) {
    case KEY_BRIGHT_UP: return "bright+";
    case KEY_FAN_FORWARD: return "fan forward";
    case KEY_BRIGHT_DOWN: return "bright-";
    case KEY_ALL_OFF: return "all off";
    case KEY_TEMP_UP: return "temp+";
    case KEY_LIGHT_TOGGLE: return "light on/off";
    case KEY_TIMER_2H: return "2H";
    case KEY_FAN_4: return "fan 4";
    case KEY_TEMP_DOWN: return "temp-";
    case KEY_FAN_6: return "fan 6";
    case KEY_CYCLE_FULL_BRIGHT: return "cycle full bright";
    case KEY_FAN_5: return "fan 5";
    case KEY_FAN_1: return "fan 1";
    case KEY_FAN_REVERSE: return "fan reverse";
    case KEY_FAN_2: return "fan 2";
    case KEY_NIGHT_MODE: return "night mode";
    case KEY_NATURAL_WIND: return "natural wind";
    case KEY_FAN_OFF: return "fan off";
    case KEY_TIMER_4H: return "4H";
    case KEY_FAN_3: return "fan 3";
    default: return "unknown";
  }
}

// --- speed <-> key ----------------------------------------------------------
//
// The six OEM speeds are six separate buttons, so anything holding a speed --
// the Home Assistant fan entity, an automation -- has to name the key that
// selects it. Read off the table above like everything else here, never
// computed: the codes are not contiguous and do not run in speed order.

static const uint8_t FAN_SPEED_COUNT = 6;

// Speed 1..FAN_SPEED_COUNT, or 0 for off. Anything else is off too, so a
// caller that clamps badly stops the fan rather than picking a speed at random.
// Off is `fan off`, not `all off` -- `all off` kills the light as well.
inline uint8_t key_for_speed(uint8_t speed) {
  switch (speed) {
    case 1: return KEY_FAN_1;
    case 2: return KEY_FAN_2;
    case 3: return KEY_FAN_3;
    case 4: return KEY_FAN_4;
    case 5: return KEY_FAN_5;
    case 6: return KEY_FAN_6;
    default: return KEY_FAN_OFF;
  }
}

// The inverse, for mirroring a press of the physical remote into whatever holds
// the speed. -1 for a key that does not select one -- the light keys, the
// direction keys, and every code this remote has no button for.
//
// `all off` reports 0: it stops the fan as well as the light, so a fan that
// tracked only `fan off` would sit there claiming to run.
inline int speed_for_key(uint8_t key) {
  switch (key) {
    case KEY_FAN_1: return 1;
    case KEY_FAN_2: return 2;
    case KEY_FAN_3: return 3;
    case KEY_FAN_4: return 4;
    case KEY_FAN_5: return 5;
    case KEY_FAN_6: return 6;
    case KEY_FAN_OFF:
    case KEY_ALL_OFF: return 0;
    default: return -1;
  }
}

struct Packet {
  uint32_t raw;      // the 32 bits as received, MSB first
  uint32_t prefix;   // bits 31..12
  uint8_t key;       // bits 11..7 -- which button
  uint8_t counter;   // bits 6..4  -- +1 mod 8 per press of any button
  uint8_t check;     // bits 3..0

  bool prefix_ok;
  bool check_ok;

  // An unlisted KEY is valid-but-unnamed, never an error: the prefix and the
  // checksum are what say a packet is real. Twelve KEY values are unused by
  // this remote, but a second remote or an unpressed button could use them.
  bool valid() const { return this->prefix_ok && this->check_ok; }
};

// Split a 32-bit codeword into fields and verify them.
inline Packet decode_packet(uint32_t bits) {
  Packet p;
  p.raw = bits;
  p.prefix = bits >> 12;
  p.key = (uint8_t) ((bits >> 7) & 0x1F);
  p.counter = (uint8_t) ((bits >> 4) & 0x07);
  p.check = (uint8_t) (bits & 0x0F);

  p.prefix_ok = p.prefix == PREFIX;

  // The three payload nibbles XOR to CHECK_XOR. Equivalent to recomputing CS
  // from KEY and CNT, but symmetric and one expression.
  const uint8_t n5 = (uint8_t) ((bits >> 8) & 0x0F);
  const uint8_t n6 = (uint8_t) ((bits >> 4) & 0x0F);
  const uint8_t n7 = (uint8_t) (bits & 0x0F);
  p.check_ok = (uint8_t) (n5 ^ n6 ^ n7) == CHECK_XOR;

  return p;
}

// Build the codeword for a button at a given press counter. The inverse of
// decode_packet, for the transmit path and for tests.
inline uint32_t encode_packet(uint8_t key, uint8_t counter) {
  const uint8_t k = key & 0x1F;
  const uint8_t c = counter & 0x07;
  const uint8_t n5 = (uint8_t) (k >> 1);
  const uint8_t n6 = (uint8_t) (((k & 1) << 3) | c);
  const uint8_t n7 = (uint8_t) (n5 ^ n6 ^ CHECK_XOR);
  return (PREFIX << 12) | ((uint32_t) k << 7) | ((uint32_t) c << 4) | n7;
}

// Write `bits` as 32 '0'/'1' characters plus a terminator. `out` needs 33
// bytes. Used by the frame dump and by the host tests.
inline void format_bits(uint32_t bits, char *out) {
  for (uint32_t i = 0; i < PACKET_BITS; i++)
    out[i] = (bits & (1UL << (PACKET_BITS - 1 - i))) ? '1' : '0';
  out[PACKET_BITS] = '\0';
}

// --- stage 3: packets -> presses --------------------------------------------
//
// The remote gives no explicit repeat code: a held button just keeps sending
// the same frame, counter unchanged, at one frame every 41.1 ms. So the only
// way to tell "pressed" from "still holding" is to count frames.
//
//   frame:  1   2   3   4   5   6   7   8   ...
//   event:  P   .   .   .   .   R1  R2  R3
//
// The first frame of a new codeword is the press, reported immediately -- about
// 37 ms after the remote started transmitting, rather than 218 ms if it waited
// for the burst to finish. The next PRESS_FRAMES-1 frames are the rest of that
// same press and are swallowed, so a plain tap fires exactly once. Anything
// beyond that is the button being held, and each such frame is its own repeat.
//
// A run ends when the codeword changes or when RUN_TIMEOUT_MS passes with no
// frame. The timeout is what stops the ninth press of one button -- the counter
// wraps at 8, so its codeword repeats -- from looking like a continuing hold.
// It has to sit above the 41.1 ms frame period with room for dropped frames,
// and below the interval between two deliberate presses.
//
// Frames that fail validation never reach here, so noise cannot extend a run.
//
// Time is passed in rather than read, so this is testable and carries no
// dependency on the framework.

static const uint8_t DEFAULT_PRESS_FRAMES = 5;
static const uint32_t DEFAULT_RUN_TIMEOUT_MS = 250;

struct PressEvent {
  bool emit;        // false = swallowed as part of the press already reported
  uint32_t repeat;  // 0 = the press itself, 1.. = index of a held repeat
};

class PressTracker {
 public:
  void set_press_frames(uint8_t frames) { this->press_frames_ = frames < 1 ? 1 : frames; }
  uint8_t get_press_frames() const { return this->press_frames_; }
  void set_run_timeout_ms(uint32_t ms) { this->run_timeout_ms_ = ms; }
  uint32_t get_run_timeout_ms() const { return this->run_timeout_ms_; }

  PressEvent feed(uint32_t code, uint32_t now_ms) {
    const bool continuing = this->primed_ && code == this->code_ &&
                            (uint32_t) (now_ms - this->last_ms_) < this->run_timeout_ms_;
    this->last_ms_ = now_ms;

    if (!continuing) {
      this->primed_ = true;
      this->code_ = code;
      this->frames_ = 1;
      return PressEvent{true, 0};
    }

    if (this->frames_ < 0xFFFF)
      this->frames_++;
    if (this->frames_ <= this->press_frames_)
      return PressEvent{false, 0};
    return PressEvent{true, (uint32_t) (this->frames_ - this->press_frames_)};
  }

  // Frames counted into the run so far, held codeword included. Diagnostic.
  uint16_t frames_in_run() const { return this->frames_; }

  void reset() {
    this->primed_ = false;
    this->frames_ = 0;
  }

 private:
  uint32_t code_{0};
  uint32_t last_ms_{0};
  uint16_t frames_{0};
  uint32_t run_timeout_ms_{DEFAULT_RUN_TIMEOUT_MS};
  uint8_t press_frames_{DEFAULT_PRESS_FRAMES};
  bool primed_{false};
};

// --- stage 4: packet -> waveform --------------------------------------------
//
// Stage 1 run backwards, for the relay: turn a codeword into the signed
// durations remote_transmitter wants. Same convention decode_frame() reads --
// marks positive, spaces negative -- so the receive path, which is already
// validated against real captures, doubles as the oracle for this one.
//
// Nominal timings only. The AGC skew in PROTOCOL.md section 1 is what the OEM
// receiver does to a signal on the way in; putting it back on the way out
// would be copying a measurement artefact into the transmitter.

static const uint32_t PREAMBLE_MARK_US = 335;
static const uint32_t PREAMBLE_GAP_US = 7690;
static const uint32_t STOP_MARK_US = 330;
static const uint32_t GAP_US = 8787;

// Entries each builder writes. A frame is 32 (mark, space) pairs, then the stop
// mark and the gap behind it.
static const size_t PREAMBLE_ENTRIES = 2;
static const size_t FRAME_ENTRIES = PACKET_BITS * 2 + 2;

// Airtime of one frame including its trailing gap. This is the cadence a held
// button runs at, and therefore the cadence an injected stream has to keep: one
// frame in per received repeat, each occupying the interval until the next.
static const uint32_t FRAME_PERIOD_US = PACKET_BITS * (SHORT_US + LONG_US) + STOP_MARK_US + GAP_US;

// The lone mark that opens a burst, emitted once ahead of the first frame.
// Returns entries written, or 0 if `cap` is too small.
inline size_t build_preamble(int32_t *out, size_t cap) {
  if (cap < PREAMBLE_ENTRIES)
    return 0;
  out[0] = (int32_t) PREAMBLE_MARK_US;
  out[1] = -(int32_t) PREAMBLE_GAP_US;
  return PREAMBLE_ENTRIES;
}

// One frame for a button at a given press counter: 32 bits MSB first, the stop
// mark, and the inter-frame gap.
inline size_t build_frame(uint8_t key, uint8_t counter, int32_t *out, size_t cap) {
  if (cap < FRAME_ENTRIES)
    return 0;
  const uint32_t word = encode_packet(key, counter);
  size_t n = 0;
  for (uint32_t i = 0; i < PACKET_BITS; i++) {
    const bool one = (word >> (PACKET_BITS - 1 - i)) & 1u;
    out[n++] = (int32_t) (one ? LONG_US : SHORT_US);
    out[n++] = -(int32_t) (one ? SHORT_US : LONG_US);
  }
  out[n++] = (int32_t) STOP_MARK_US;
  out[n++] = -(int32_t) GAP_US;
  return n;
}

// --- which half of the fan a button drives ----------------------------------
//
// Read off the button names in PROTOCOL.md section 4 and confirmed by pressing
// them, never computed: KEY is a lookup table and nothing in a code predicts
// what it does.
//
// The relay runs in `fan` mode: the ESP32 drives the LEDs itself, so the light
// keys stop there and only the fan half is forwarded. `all` forwards
// everything, which is what an unmodified LED path needs.
enum KeyDomain : uint8_t {
  DOMAIN_FAN,
  DOMAIN_LIGHT,
  // Drives both halves: `all off` stops the fan and kills the light, so it is
  // forwarded to the MCU *and* acted on here.
  DOMAIN_BOTH,
  // Reserved for the owner's own automations, never injected. These are real
  // buttons with real OEM behaviour -- see key_domain() for which and why --
  // that this build deliberately takes over as spare inputs.
  DOMAIN_USER,
  // Not a button on this remote. Forwarded in `fan` mode on the same grounds
  // the classification is conservative everywhere: a code nobody has seen is
  // not one to start swallowing.
  DOMAIN_UNKNOWN,
};

inline KeyDomain key_domain(uint8_t key) {
  switch (key) {
    case KEY_BRIGHT_UP:
    case KEY_BRIGHT_DOWN:
    case KEY_TEMP_UP:
    case KEY_TEMP_DOWN:
    case KEY_LIGHT_TOGGLE:
    case KEY_CYCLE_FULL_BRIGHT:
    // Observed: night mode acts on the light alone, so it stops here like the
    // other six and the ESP32 implements it.
    case KEY_NIGHT_MODE:
      return DOMAIN_LIGHT;
    case KEY_FAN_1:
    case KEY_FAN_2:
    case KEY_FAN_3:
    case KEY_FAN_4:
    case KEY_FAN_5:
    case KEY_FAN_6:
    case KEY_FAN_OFF:
    case KEY_FAN_FORWARD:
    case KEY_FAN_REVERSE:
      return DOMAIN_FAN;
    case KEY_ALL_OFF:
      return DOMAIN_BOTH;
    // Repurposed by choice, not by ignorance. `natural wind` is a fan mode and
    // the MCU would act on it; 2H and 4H are believed to be fan-off timers,
    // never used either. The owner does not use any of the three, so they are
    // free inputs for automations here and are not injected. Moving one back
    // to DOMAIN_FAN restores its OEM behaviour and costs nothing else.
    case KEY_NATURAL_WIND:
    case KEY_TIMER_2H:
    case KEY_TIMER_4H:
      return DOMAIN_USER;
    default:
      return DOMAIN_UNKNOWN;
  }
}

enum RelayMode : uint8_t {
  RELAY_NONE,  // decode only, inject nothing -- bring-up, and send_key still works
  RELAY_FAN,
  RELAY_ALL,
};

inline bool relay_key(RelayMode mode, uint8_t key) {
  switch (mode) {
    // Literal pass-through, including the DOMAIN_USER keys: `all` is the mode
    // for a board whose R37/R38 are still fitted, where the ESP32 drives
    // nothing and swallowing any key would just break a working remote.
    case RELAY_ALL:
      return true;
    // Fan mode drops what the ESP32 owns: the light keys, and the three the
    // owner has claimed for automations. DOMAIN_BOTH is forwarded -- `all off`
    // has to reach the MCU to stop the fan, and the light half is handled here
    // in parallel.
    case RELAY_FAN: {
      const KeyDomain d = key_domain(key);
      return d != DOMAIN_LIGHT && d != DOMAIN_USER;
    }
    default:
      return false;
  }
}

// --- stage 5: presses -> injection ------------------------------------------
//
// Nothing goes on the wire on the press event itself, and the waiting is the
// point. PressTracker reports a press on the first frame; only the arrival, or
// not, of a repeat says whether the button was tapped or is being held.
//
//   repeat first    -> a hold. Inject immediately, then one more frame for
//                      every further repeat, so the injected stream tracks the
//                      hold in real time and stops when the button is released.
//   deadline first  -> a tap. Inject TAP_FRAMES frames, once.
//
// The preamble goes out once, at the start of either.
//
// One received repeat every frame period against one injected frame occupying
// a frame period is what keeps the stream in step: nothing queues, nothing
// overlaps.
//
// The counter is ours, not the remote's. Once the 102 is removed the MCU hears
// only the ESP32, so there is a single sequence reaching it and nothing to
// collide with. It advances once per injected press and holds for every frame
// of that press: a counter that moved per frame would make a hold read as a
// stream of separate presses, which is the one thing the counter exists to
// prevent.
//
// ASSUMPTION: that the MCU dedupes on the counter at all. PROTOCOL.md section 2
// describes what the *remote* does with it; nothing here has measured what the
// receiver makes of it. If the MCU ignores the counter entirely, this policy
// costs nothing -- it just makes every frame of a press identical, which is
// what the remote sends anyway.
//
// Time is passed in, the same way PressTracker takes it, so the decision is
// host-testable with no clock and no framework.

// Past where the sixth frame would arrive -- the tracker swallows PRESS_FRAMES
// frames, so the first repeat lands PRESS_FRAMES frame periods after the press
// event. See the note on relay_decision in firmware/relay.yaml for the choice.
static const uint32_t DEFAULT_DECISION_DELAY_MS = 230;

// Frames a tap injects. The remote sends five, but that redundancy buys margin
// on a noisy RF link and injection is over a wire.
//
// One is enough: the fan keys tap through to the MCU at this setting, on
// hardware. Raise this if a tap is ever dropped.
static const uint8_t DEFAULT_TAP_FRAMES = 1;

struct InjectPlan {
  uint8_t frames;   // frames to put on the wire now; 0 = nothing to do
  bool preamble;    // prefix them with the preamble -- the start of an injection
  uint8_t key;
  uint8_t counter;  // ours, and the same for every frame of this press

  bool any() const { return this->frames != 0; }
};

class RelayGate {
 public:
  void set_decision_delay_ms(uint32_t ms) { this->decision_delay_ms_ = ms; }
  uint32_t get_decision_delay_ms() const { return this->decision_delay_ms_; }
  void set_tap_frames(uint8_t frames) { this->tap_frames_ = frames < 1 ? 1 : frames; }
  uint8_t get_tap_frames() const { return this->tap_frames_; }
  uint8_t get_counter() const { return this->counter_; }

  // A press event. Arms the deadline and injects nothing at all. If a press is
  // already pending -- two buttons inside one decision window -- that one
  // resolves here as the tap it turned out to be, since a repeat of it can no
  // longer arrive.
  InjectPlan press(uint8_t key, uint32_t now_ms) {
    const InjectPlan plan = this->resolve_pending_();
    this->streaming_ = false;
    this->pending_ = true;
    this->pending_key_ = key;
    this->pending_ms_ = now_ms;
    return plan;
  }

  // A repeat event. The first one settles the pending press as a hold and opens
  // the stream; each one after adds a frame to it. Carries no timestamp: a
  // repeat is a fact about the button, and the only deadline here is the one
  // poll() watches.
  InjectPlan repeat(uint8_t key) {
    if (this->pending_ && key == this->pending_key_) {
      this->pending_ = false;
      return this->open_stream_(key, 1);
    }
    // Also the recovery path when the sixth frame was lost and the deadline
    // fired first: the tap that went out already carries this run's counter, so
    // extending it here is indistinguishable on the wire from having called it
    // a hold from the start.
    if (this->streaming_ && key == this->stream_key_)
      return InjectPlan{1, false, key, this->stream_counter_};
    // A repeat with nothing behind it -- the press was not relayed, or a newer
    // press has superseded this run.
    return InjectPlan{0, false, key, 0};
  }

  // Called on every pass of the main loop. Fires the tap once the deadline
  // passes with no repeat behind it.
  InjectPlan poll(uint32_t now_ms) {
    if (!this->pending_ || (uint32_t) (now_ms - this->pending_ms_) < this->decision_delay_ms_)
      return InjectPlan{0, false, 0, 0};
    this->pending_ = false;
    return this->resolve_pending_tap_();
  }

  // A command originated on the ESP32 rather than relayed from the remote: one
  // press, its own counter, no hold. It leaves any run in progress alone --
  // interrupting a physical hold to squeeze this in would be worse than letting
  // the two interleave.
  InjectPlan originate(uint8_t key) {
    return InjectPlan{this->tap_frames_, true, key, this->advance_counter_()};
  }

  void reset() {
    this->pending_ = false;
    this->streaming_ = false;
  }

 protected:
  // Resolve a pending press as a tap, if there is one. Injecting leaves the
  // stream open on the same counter so a late repeat can extend it.
  InjectPlan resolve_pending_() {
    if (!this->pending_)
      return InjectPlan{0, false, 0, 0};
    this->pending_ = false;
    return this->resolve_pending_tap_();
  }

  InjectPlan resolve_pending_tap_() { return this->open_stream_(this->pending_key_, this->tap_frames_); }

  InjectPlan open_stream_(uint8_t key, uint8_t frames) {
    this->streaming_ = true;
    this->stream_key_ = key;
    this->stream_counter_ = this->advance_counter_();
    return InjectPlan{frames, true, key, this->stream_counter_};
  }

  uint8_t advance_counter_() {
    this->counter_ = (uint8_t) ((this->counter_ + 1) & 0x07);
    return this->counter_;
  }

  uint32_t pending_ms_{0};
  uint32_t decision_delay_ms_{DEFAULT_DECISION_DELAY_MS};
  uint8_t tap_frames_{DEFAULT_TAP_FRAMES};
  // Not restored across a reboot, deliberately. The ESP32 and the OEM MCU share
  // the fan's secondary rail, so mains power cycles both and the MCU has no
  // remembered counter either; the two can only disagree after an ESP32-only
  // restart (OTA or a watchdog reset), and then at worst one injected command
  // in eight is ignored and the button is pressed again. Persisting it would
  // mean an NVS write per press to guard an inference.
  uint8_t counter_{0};
  uint8_t pending_key_{0};
  uint8_t stream_key_{0};
  uint8_t stream_counter_{0};
  bool pending_{false};
  bool streaming_{false};
};

}  // namespace fan_rf
}  // namespace smarterfan
