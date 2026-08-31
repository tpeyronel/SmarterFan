// Frame decoder for the Novohome NH-VTR500 remote -- standalone copy.
//
// Self-contained on purpose. This component depends on nothing but
// `remote_receiver`: no ESPHome headers here, no Arduino, no IDF, and
// deliberately no reach into the sibling `fan_rf` component. Drop this
// directory into any ESPHome project and it works on its own.
//
// The cost of that is real and worth stating: the symbol timings below also
// exist in fan_rf/fan_rf_protocol.h. If the remote is ever re-measured and the
// nominals move, they must move in both places.
//
// The header is free of framework dependencies so it can be compiled straight
// into tools/fan_rf_selftest.cpp, which replays captured logs through it. What
// is validated offline is then literally what runs on the device.
//
// Wire format
// -----------
//   [~350us mark][7.69ms gap]                         preamble, once
//   then 5x [32 data bits][~330us stop mark][8.79ms gap]
//
//   bit 0 = 288us mark + 704us space      bit period ~1009us
//   bit 1 = 800us mark + 224us space

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace smarterfan {
namespace fan_rf_dump {

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

// A space longer than this ends a frame. The inter-frame gap is 8787us +/- 10
// and the preamble gap is 7.69ms; the longest in-frame space is a ~700us zero.
static const uint32_t FRAME_GAP_US = 5000;

static const uint32_t PACKET_BITS = 32;

struct FrameWalk {
  uint16_t chunks;   // chunks between gaps, preamble included
  uint16_t decoded;  // chunks that yielded 32 clean bits
};

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
// calling on_frame(position, bits) for every one that comes out clean.
//
// `position` is 1-based and skips a leading preamble chunk, so "frame 1" means
// the first data frame and a gap in the numbering is a frame that did not
// decode. Every chunk is tried, including the first: the preamble is a lone
// mark and fails the length check on its own, so there is no need to assume the
// capture begins at a burst boundary -- captures do start mid-burst.
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

}  // namespace fan_rf_dump
}  // namespace smarterfan
