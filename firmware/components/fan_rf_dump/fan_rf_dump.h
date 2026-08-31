// Diagnostic: print every frame that decodes, as a 32-bit binary word.
//
// This is the plain view of what the receiver actually delivered. It does no
// interpretation at all -- no device ID check, no checksum, no majority vote,
// no dedupe, no field split. The only test applied is the one that has to be
// applied: do these pulses look like the remote's symbols?
//
// That test is all-or-nothing per frame. A frame is 32 (mark, space) pairs at
// the remote's two pulse widths; one symbol that matches neither bit throws the
// whole frame away. So a corrupted frame prints nothing rather than printing a
// half-guessed word -- and because frames are numbered with the preamble
// skipped, a gap in the numbering IS the report that a frame was corrupt:
//
//     166 symbols, 6 chunks, 5 frames decoded
//       f1 10100001110110000010000111101001
//       f2 10100001110110000010000111101001
//       ...
//
//     190 symbols, 6 chunks, 4 frames decoded
//       f2 10100001110110000010000111101001    <- f1 was corrupt
//       f3 10100001110110000010000111101001
//
// Silent when a capture yields nothing, which is most of them: idle noise and
// cold-press chatter both produce captures, and a session can run to hundreds.
//
// Standalone: it needs `remote_receiver` and nothing else. The frame decoder is
// its own, in fan_rf_dump_protocol.h. It can run alongside `fan_rf` -- every
// listener sees every capture, so the decoded press events and this raw view
// arrive together -- but it does not need it, and will build in a config where
// `fan_rf` is not present at all.

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/remote_base/remote_base.h"

#include "fan_rf_dump_protocol.h"

#include <vector>

namespace smarterfan {
namespace fan_rf_dump {

static const char *const TAG = "fan_rf_dump";

// One capture can hold at most a few tens of frames: `receive_symbols: 1024`
// caps the raw vector, and a frame costs 64 entries of it. Collecting them
// before printing keeps the header line accurate and bounds the work per call.
static const uint8_t MAX_REPORTED = 32;

class FanRfDump : public esphome::Component, public esphome::remote_base::RemoteReceiverListener {
 public:
  bool on_receive(esphome::remote_base::RemoteReceiveData data) override {
    const std::vector<int32_t> &raw = data.get_raw_data();

    uint32_t codes[MAX_REPORTED];
    uint16_t positions[MAX_REPORTED];
    uint8_t found = 0;

    const FrameWalk walk =
        walk_frames(raw.data(), raw.size(), [&](uint16_t position, uint32_t bits) {
          if (found < MAX_REPORTED) {
            codes[found] = bits;
            positions[found] = position;
            found++;
          }
        });

    if (found == 0)
      return false;

    ESP_LOGI(TAG, "%u symbols, %u chunks, %u frames decoded", (unsigned) raw.size(),
             (unsigned) walk.chunks, (unsigned) walk.decoded);
    char bits[PACKET_BITS + 1];
    for (uint8_t i = 0; i < found; i++) {
      format_bits_(codes[i], bits);
      ESP_LOGI(TAG, "  f%u %s", (unsigned) positions[i], bits);
    }
    return true;
  }

  void dump_config() override {
    ESP_LOGCONFIG(TAG, "Fan RF frame dump (diagnostic):");
    ESP_LOGCONFIG(TAG, "  Prints every frame that decodes as 32 clean bits, as a binary word.");
    ESP_LOGCONFIG(TAG, "  No ID check, no checksum, no vote, no dedupe. Silent when nothing decodes.");
  }

 protected:
  static void format_bits_(uint32_t value, char *out) {
    for (uint8_t i = 0; i < PACKET_BITS; i++)
      out[i] = (value & (1UL << (PACKET_BITS - 1 - i))) ? '1' : '0';
    out[PACKET_BITS] = '\0';
  }
};

}  // namespace fan_rf_dump
}  // namespace smarterfan
