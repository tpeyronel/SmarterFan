#include "fan_rf.h"

namespace smarterfan {
namespace fan_rf {

static const char *const TAG = "fan_rf";

bool FanRfDecoder::on_receive(esphome::remote_base::RemoteReceiveData data) {
  return this->process_raw(data.get_raw_data());
}

bool FanRfDecoder::process_raw(const std::vector<int32_t> &raw) {
  const uint32_t now = esphome::millis();
  bool any = false;

  // walk_frames() splits the capture on the inter-frame gap and decodes each
  // chunk from its own start, so the AGC-damaged first frame of a burst costs
  // nothing as long as a later one is clean. Frames arrive here in order.
  const FrameWalk walk = walk_frames(raw.data(), raw.size(), [&](uint16_t, uint32_t bits) {
    const Packet packet = decode_packet(bits);
    if (!packet.valid()) {
      // 32 pulses at the remote's symbol widths that still fail the prefix or
      // the checksum. Rare, and never allowed to extend a run.
      ESP_LOGV(TAG, "frame 0x%08" PRIX32 " failed validation (prefix %s, check %s)", bits,
               packet.prefix_ok ? "ok" : "BAD", packet.check_ok ? "ok" : "BAD");
      return;
    }
    any = true;

    const PressEvent event = this->tracker_.feed(packet.raw, now);
    if (!event.emit) {
      ESP_LOGV(TAG, "frame %u of the press already reported, swallowed",
               (unsigned) this->tracker_.frames_in_run());
      return;
    }

    if (event.repeat == 0) {
      ESP_LOGD(TAG, "press  key=%2u (%s) counter=%u  0x%08" PRIX32, packet.key,
               key_name(packet.key), packet.counter, packet.raw);
    } else {
      ESP_LOGD(TAG, "repeat key=%2u (%s) counter=%u  0x%08" PRIX32 "  #%" PRIu32, packet.key,
               key_name(packet.key), packet.counter, packet.raw, event.repeat);
    }
    this->publish_(packet, event.repeat);
  });

  if (!any) {
    // Idle noise produces captures constantly. Keep this at VERBOSE: a session
    // runs to hundreds of them, and one line each at DEBUG would bury the
    // presses this component exists to report.
    ESP_LOGV(TAG, "no valid packet (%u symbols, %u chunks, %u frames decoded)",
             (unsigned) raw.size(), (unsigned) walk.chunks, (unsigned) walk.decoded);
  }
  return any;
}

void FanRfDecoder::publish_(const Packet &packet, uint32_t repeat) {
  for (FanRfBinarySensor *sensor : this->sensors_) {
    if (sensor->get_key() != packet.key)
      continue;
    if (repeat != 0 && !sensor->get_repeats())
      continue;
    // Momentary, the same shape as remote_base's own binary sensors: the remote
    // sends a press, never a release. A held button therefore reads as a train
    // of pulses one frame period apart.
    sensor->publish_state(true);
    esphome::yield();
    sensor->publish_state(false);
  }
  this->callbacks_.call(packet.key, packet.counter, packet.raw, repeat);
}

void FanRfDecoder::dump_config() {
  ESP_LOGCONFIG(TAG, "Novohome NH-VTR500 RF decoder:");
  ESP_LOGCONFIG(TAG, "  Prefix: 0x%05" PRIX32 " (20 bits)", PREFIX);
  ESP_LOGCONFIG(TAG, "  Symbols: bit0 %uus/%uus, bit1 %uus/%uus at %u%% width slack",
                (unsigned) frame::SHORT_US, (unsigned) frame::LONG_US, (unsigned) frame::LONG_US,
                (unsigned) frame::SHORT_US, (unsigned) frame::WIDTH_TOLERANCE_PCT);
  ESP_LOGCONFIG(TAG, "  Bit period: %uus +/-%u%%", (unsigned) frame::PERIOD_US,
                (unsigned) frame::PERIOD_TOLERANCE_PCT);
  ESP_LOGCONFIG(TAG, "  Frame split gap: %uus", (unsigned) frame::FRAME_GAP_US);
  ESP_LOGCONFIG(TAG, "  Press: first %u identical frames report once, each frame after repeats",
                (unsigned) this->tracker_.get_press_frames());
  ESP_LOGCONFIG(TAG, "  Run timeout: %" PRIu32 " ms", this->tracker_.get_run_timeout_ms());
  ESP_LOGCONFIG(TAG, "  Buttons: %u", (unsigned) this->sensors_.size());
}

void FanRfBinarySensor::dump_config() {
  LOG_BINARY_SENSOR("", "Fan RF Button", this);
  ESP_LOGCONFIG(TAG, "  Key: %u (%s)", this->key_, key_name(this->key_));
  ESP_LOGCONFIG(TAG, "  Pulses on held repeats: %s", YESNO(this->repeats_));
}

}  // namespace fan_rf
}  // namespace smarterfan
