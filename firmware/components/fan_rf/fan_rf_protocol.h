// Novohome NH-VTR500 433 MHz OOK remote -- the whole decoder.
//
// Three stages, in order, each one strictly above the last:
//
//   1. raw capture -> frames    walk_frames(), decode_frame()
//        Split a capture on the inter-frame gap and turn each chunk into a
//        32-bit codeword. Knows about pulse widths and nothing else.
//   2. frame -> packet          decode_packet()
//        Split a codeword into prefix, key, press counter and checksum, and
//        say whether it is real. Knows about fields and nothing else.
//   3. packets -> presses       PressTracker
//        Count identical codewords to tell a tap from a held button.
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

}  // namespace fan_rf
}  // namespace smarterfan
