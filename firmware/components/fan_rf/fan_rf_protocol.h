// Novohome NH-VTR500 433 MHz OOK remote — pure protocol decoder.
//
// Deliberately free of any ESPHome (or Arduino, or ESP-IDF) dependency: this
// header is compiled unchanged both into the firmware and into the host
// self-test in tools/fan_rf_selftest.cpp, so the logic that runs on the device
// is literally the logic validated against the captured logs. Anything that
// needs the framework lives in fan_rf.h.
//
// Wire format
// -----------
//   [~350us mark][7.69ms gap]                         preamble, once
//   then 5x [32 data bits][~330us stop mark][8.79ms gap]
//
//   bit 0 = 288us mark + 704us space      bit period ~1009us
//   bit 1 = 800us mark + 224us space
//
// Packet, 32 bits MSB first:
//
//   [ 0..15] 0xA1D8   remote / device ID
//   [16..23] command  0x21 brightness+, 0x23 colour temp+, 0x28 fan speed 1
//   [24..27] high nibble = [domain flag][3-bit press counter]
//   [28..31] low nibble  = high nibble XOR (command & 0x0F) XOR 6
//
// Why this exists at all: ESPHome's rc_switch decoder syncs once and then
// reads bits from offset 0 of the capture. All five repeats carry the same
// payload, but the receiver's AGC corrupts the first one while it settles, so
// a decoder that only ever looks at the first frame fails on the one frame
// that is damaged. This one splits the capture on the inter-frame gap, decodes
// every frame independently, and majority-votes the survivors.

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace smarterfan {
namespace fan_rf {

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
//
// An earlier revision used 288/800/224/704. Those were not measurements: they
// are 9/25/7/22 x 32, the grid imposed by the deleted rc_switch profile's
// `pulse_length: 32`. They sat further from the measured centres (23.5us
// average error) than these do (16.5us).
static const uint32_t SHORT_US = 252;
static const uint32_t LONG_US = 756;

// Width slack, percent. Wide on purpose: with the period check carrying the
// selectivity, this is a sanity rail on each pulse rather than the primary
// discriminator. 50% is where the frame yield plateaus, and it makes the
// filter a strict superset of the old 288/800/224/704 at 40% -- every frame
// that decoded before still decodes, with identical bits.
static const uint32_t WIDTH_TOLERANCE_PCT = 50;

// Bit period, and the real discriminator. Measured mean 1009.1us with a
// standard deviation of 3.3us over 10592 symbols, so +/-5% is about 15 sigma
// and cannot reject a real frame. On noise, where mark and space are
// uncorrelated, the sum wanders freely and this rejects most pairs outright:
// measured against the noise captures it accepts 2.2% of pairs where the width
// windows alone accept 7.3%.
static const uint32_t PERIOD_US = 1009;
static const uint32_t PERIOD_TOLERANCE_PCT = 5;

// A space longer than this ends a frame. The real inter-frame gap is
// 8787us +/- 10us and the preamble gap is 7.69ms; the longest in-frame space
// is a ~700us zero, so 5ms sits far from anything.
static const uint32_t FRAME_GAP_US = 5000;

static const uint32_t PACKET_BITS = 32;
static const uint16_t DEVICE_ID = 0xA1D8;
static const uint8_t CHECK_XOR = 6;

// Frames per burst. Only used to size the vote table; a capture carrying more
// (two presses merged, say) simply votes over the first MAX_FRAMES it decodes.
static const uint8_t MAX_FRAMES = 12;

struct Packet {
  uint32_t raw;       // the 32 bits as received, MSB first
  uint16_t device;    // bits 0..15
  uint8_t command;    // bits 16..23
  uint8_t domain;     // bit 24: 1 = light, 0 = fan
  uint8_t counter;    // bits 25..27, +1 mod 8 per press of any button
  uint8_t check;      // bits 28..31

  bool device_ok;
  bool check_ok;
  // Advisory only. The domain flag is the inverse of command bit 3 for every
  // command captured so far, but only three of the remote's buttons have been
  // seen, so a mismatch is reported rather than treated as a bad packet.
  bool domain_ok;

  // The ID and the check nibble are what validate a packet. An unrecognised
  // command byte is valid-but-unnamed, not an error.
  bool valid() const { return this->device_ok && this->check_ok; }
};

struct DecodeResult {
  bool ok;                 // a valid packet won the vote
  Packet packet;
  uint16_t frames_total;   // chunks between gaps, preamble included
  uint16_t frames_decoded; // chunks that yielded 32 clean bits
  uint16_t votes;          // frames agreeing with the winner
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

// Decode one frame: 32 (mark, space) pairs, MSB first, optionally followed by
// a lone stop mark. Returns false unless every one of the 32 symbols matches,
// which is what makes a corrupt frame drop out instead of voting garbage.
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

// Split a 32-bit codeword into fields and verify them.
inline Packet decode_packet(uint32_t bits) {
  Packet p;
  p.raw = bits;
  p.device = (uint16_t) (bits >> 16);
  p.command = (uint8_t) (bits >> 8);
  const uint8_t high = (uint8_t) ((bits >> 4) & 0x0F);
  const uint8_t low = (uint8_t) (bits & 0x0F);
  p.domain = high >> 3;
  p.counter = high & 0x07;
  p.check = low;
  p.device_ok = p.device == DEVICE_ID;
  p.check_ok = low == (uint8_t) (high ^ (p.command & 0x0F) ^ CHECK_XOR);
  p.domain_ok = p.domain == ((p.command & 0x08) ? 0 : 1);
  return p;
}

struct FrameWalk {
  uint16_t chunks;   // chunks between gaps, preamble included
  uint16_t decoded;  // chunks that yielded 32 clean bits
};

// Split a capture on inter-frame gaps and decode every chunk independently,
// calling on_frame(position, bits) for each one that comes out clean.
//
// Every chunk is tried, including the first: the preamble is a lone mark and
// fails the length check on its own, so there is no need to assume the capture
// begins at a burst boundary. That matters -- captures do start mid-burst.
//
// `position` is 1-based and skips a leading preamble chunk, so "frame 1" means
// the first data frame and a gap in the numbering is a frame that did not
// decode. A frame is all-or-nothing: one symbol that matches neither bit
// rejects the whole thing, which is what keeps a corrupt frame out of the
// caller's hands entirely rather than handing it over half-decoded.
template<typename F> inline FrameWalk walk_frames(const int32_t *data, size_t len, F &&on_frame) {
  FrameWalk walk = {0, 0};
  uint16_t first_data = 0;
  bool first_seen = false;
  size_t start = 0;

  for (size_t i = 0; i <= len; i++) {
    const bool boundary = (i == len) || is_frame_gap(data[i]);
    if (!boundary)
      continue;

    const size_t chunk_len = i - start;
    if (!first_seen) {
      first_seen = true;
      if (chunk_len < PACKET_BITS * 2)
        first_data = 1;  // a lone preamble mark; do not number it as a frame
    }

    uint32_t bits;
    if (decode_frame(&data[start], chunk_len, &bits)) {
      walk.decoded++;
      on_frame((uint16_t) (walk.chunks >= first_data ? walk.chunks - first_data + 1 : 1), bits);
    }
    walk.chunks++;
    start = i + 1;
  }
  return walk;
}

// Decode every frame in a capture and majority-vote the ones that decoded.
inline DecodeResult decode_capture(const int32_t *data, size_t len) {
  DecodeResult result;
  result.ok = false;
  result.votes = 0;

  uint32_t codes[MAX_FRAMES];
  uint16_t counts[MAX_FRAMES];
  uint8_t distinct = 0;

  const FrameWalk walk = walk_frames(data, len, [&](uint16_t, uint32_t bits) {
    uint8_t slot = 0;
    while (slot < distinct && codes[slot] != bits)
      slot++;
    if (slot == distinct && distinct < MAX_FRAMES) {
      codes[distinct] = bits;
      counts[distinct] = 0;
      distinct++;
    }
    if (slot < distinct)
      counts[slot]++;
  });
  result.frames_total = walk.chunks;
  result.frames_decoded = walk.decoded;

  if (distinct == 0)
    return result;

  // Most votes wins. A tie goes to whichever candidate validates -- frames are
  // visited in order, so without that rule a 1-1 tie would hand the burst to
  // frame 1, the one the AGC is known to damage.
  uint8_t best = 0;
  bool best_valid = decode_packet(codes[0]).valid();
  for (uint8_t i = 1; i < distinct; i++) {
    const bool valid = decode_packet(codes[i]).valid();
    if (counts[i] > counts[best] || (counts[i] == counts[best] && valid && !best_valid)) {
      best = i;
      best_valid = valid;
    }
  }
  result.votes = counts[best];
  result.packet = decode_packet(codes[best]);
  result.ok = result.packet.valid();
  return result;
}

// Collapses the repeats of one press into a single event.
//
// One press is five identical frames inside one capture, so the vote already
// handles the normal case. This covers the rest: a burst that arrives as two
// captures (the RMT buffer filling and re-arming mid-burst does exactly that),
// and a held button repeating the same codeword.
//
// The press counter is what actually separates presses -- it advances +1 mod 8
// on every press of any button -- so an identical *codeword* inside the window
// is by definition the same press. Aliasing would need eight presses to land
// inside one window, which no sane window is long enough for.
//
// Time is passed in rather than read, so this is testable against a log's
// timestamps and carries no dependency on the framework.
class PressFilter {
 public:
  void set_window_ms(uint32_t ms) { this->window_ms_ = ms; }
  uint32_t get_window_ms() const { return this->window_ms_; }

  bool accept(uint32_t code, uint32_t now_ms) {
    const bool repeat = this->primed_ && code == this->last_code_ &&
                        (uint32_t) (now_ms - this->last_time_) < this->window_ms_;
    // Refresh the timestamp either way, so holding a button down stays
    // suppressed instead of leaking one event per window.
    this->last_code_ = code;
    this->last_time_ = now_ms;
    this->primed_ = true;
    return !repeat;
  }

 private:
  uint32_t window_ms_{1000};
  uint32_t last_code_{0};
  uint32_t last_time_{0};
  bool primed_{false};
};

}  // namespace fan_rf
}  // namespace smarterfan
