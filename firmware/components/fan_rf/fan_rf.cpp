#include "fan_rf.h"

namespace smarterfan {
namespace fan_rf {

static const char *const TAG = "fan_rf";

static const char *command_name(uint8_t command) {
  switch (command) {
    case 0x21:
      return "brightness+";
    case 0x23:
      return "colour temp+";
    case 0x28:
      return "fan speed 1";
    default:
      return "unknown";
  }
}

bool FanRfDecoder::on_receive(esphome::remote_base::RemoteReceiveData data) {
  return this->process_raw(data.get_raw_data());
}

bool FanRfDecoder::process_raw(const std::vector<int32_t> &raw) {
  const DecodeResult result = decode_capture(raw.data(), raw.size());

  if (!result.ok) {
    // Idle noise and cold-press chatter both produce captures. Keep this at
    // VERBOSE: a noisy session runs to hundreds of captures, and one line each
    // at DEBUG would bury the presses.
    ESP_LOGV(TAG, "no valid packet (%u symbols, %u frames, %u decoded)", (unsigned) raw.size(),
             (unsigned) result.frames_total, (unsigned) result.frames_decoded);
    return false;
  }

  const Packet &packet = result.packet;

  // Collapse the repeats of one press into one event. See PressFilter.
  if (!this->filter_.accept(packet.raw, esphome::millis())) {
    ESP_LOGV(TAG, "repeat of 0x%08" PRIX32 " inside the dedup window, ignored", packet.raw);
    return true;
  }

  ESP_LOGD(TAG, "0x%08" PRIX32 " cmd=0x%02X (%s) counter=%u domain=%u  [%u/%u frames]", packet.raw,
           packet.command, command_name(packet.command), packet.counter, packet.domain,
           (unsigned) result.votes, (unsigned) result.frames_decoded);
  if (!packet.domain_ok) {
    // Not fatal: only three of the remote's buttons have ever been captured, so
    // an unfamiliar command may well split the domain space differently.
    ESP_LOGD(TAG, "  domain flag %u is unexpected for command 0x%02X", packet.domain, packet.command);
  }

  this->publish_(packet);
  return true;
}

void FanRfDecoder::publish_(const Packet &packet) {
  for (FanRfBinarySensor *sensor : this->sensors_) {
    if (sensor->get_command() != packet.command)
      continue;
    // Momentary, the same shape as remote_base's own binary sensors: the remote
    // sends a press, never a release.
    sensor->publish_state(true);
    esphome::yield();
    sensor->publish_state(false);
  }
  this->callbacks_.call(packet.command, packet.counter, packet.raw);
}

void FanRfDecoder::dump_config() {
  ESP_LOGCONFIG(TAG, "Novohome NH-VTR500 RF decoder:");
  ESP_LOGCONFIG(TAG, "  Device ID: 0x%04X", DEVICE_ID);
  ESP_LOGCONFIG(TAG, "  Symbols: bit0 %uus/%uus, bit1 %uus/%uus at %u%% width slack",
                (unsigned) SHORT_US, (unsigned) LONG_US, (unsigned) LONG_US, (unsigned) SHORT_US,
                (unsigned) WIDTH_TOLERANCE_PCT);
  ESP_LOGCONFIG(TAG, "  Bit period: %uus +/-%u%%", (unsigned) PERIOD_US,
                (unsigned) PERIOD_TOLERANCE_PCT);
  ESP_LOGCONFIG(TAG, "  Frame split gap: %uus", (unsigned) FRAME_GAP_US);
  ESP_LOGCONFIG(TAG, "  Dedup window: %" PRIu32 " ms", this->filter_.get_window_ms());
  ESP_LOGCONFIG(TAG, "  Buttons: %u", (unsigned) this->sensors_.size());
}

void FanRfBinarySensor::dump_config() {
  LOG_BINARY_SENSOR("", "Fan RF Button", this);
  ESP_LOGCONFIG(TAG, "  Command: 0x%02X (%s)", this->command_, command_name(this->command_));
}

}  // namespace fan_rf
}  // namespace smarterfan
