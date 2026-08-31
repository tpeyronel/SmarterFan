// Novohome NH-VTR500 433 MHz remote -- packet layer.
//
// The frame layer lives in the sibling fan_rf_dump component and is included
// below rather than copied: symbol timings, decode_frame() and walk_frames()
// have exactly one definition in this repo. This header adds what fan_rf_dump
// deliberately does not do -- split a 32-bit word into fields, validate it, name
// the button, and turn a stream of frames into press and repeat events.
//
// The wire and frame formats are documented in PROTOCOL.md. In brief:
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

// Sibling component, same directory level in both trees: this repo's
// firmware/components/ and ESPHome's generated src/esphome/components/.
#include "../fan_rf_dump/fan_rf_dump_protocol.h"

#include <stdint.h>
#include <stddef.h>

namespace smarterfan {
namespace fan_rf {

// The frame layer, borrowed wholesale. Re-exported so callers of this header do
// not have to know which component the decoder came from.
namespace frame = ::smarterfan::fan_rf_dump;
using frame::FrameWalk;
using frame::PACKET_BITS;
using frame::decode_frame;
using frame::is_frame_gap;
using frame::walk_frames;

// Bits 31..12, constant across every frame from this remote. Transmitter
// identity, possibly with a protocol constant folded in -- a single-remote
// capture cannot separate the two.
static const uint32_t PREFIX = 0xA1D82;

// The three payload nibbles XOR to this. See PROTOCOL.md section 2.
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

// --- press / repeat tracking ------------------------------------------------
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
