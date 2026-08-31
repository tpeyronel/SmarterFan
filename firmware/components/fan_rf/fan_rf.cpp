#include "fan_rf.h"

#include <cstdio>
#include <cstring>

namespace smarterfan {
namespace fan_rf {

static const char *const TAG = "fan_rf";

bool FanRfDecoder::on_receive(esphome::remote_base::RemoteReceiveData data) {
  return this->process_raw(data.get_raw_data());
}

bool FanRfDecoder::process_raw(const std::vector<int32_t> &raw) {
  const uint32_t now = esphome::millis();
  bool any = false;

  // One line per capture, built as the frames come out. Comma-separated
  // codewords, because a capture normally carries exactly one.
  char words[MAX_DUMPED_FRAMES * (PACKET_BITS + 2) + 1];
  size_t used = 0;
  uint16_t dumped = 0;
  words[0] = '\0';

  // Stage 1 -> 2 -> 3. walk_frames() splits the capture on the inter-frame gap
  // and decodes each chunk from its own start, so the AGC-damaged first frame
  // of a burst costs nothing as long as a later one is clean.
  const FrameWalk walk = walk_frames(raw.data(), raw.size(), [&](uint32_t bits) {
    if (this->dump_frames_ && dumped < MAX_DUMPED_FRAMES) {
      if (used != 0) {
        words[used++] = ',';
        words[used++] = ' ';
      }
      format_bits(bits, &words[used]);
      used += PACKET_BITS;
      dumped++;
    }

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

    if (this->dump_commands_) {
      if (event.repeat == 0) {
        ESP_LOGD(TAG, "press  key=%2u (%s) counter=%u  0x%08" PRIX32, packet.key,
                 key_name(packet.key), packet.counter, packet.raw);
      } else {
        ESP_LOGD(TAG, "repeat key=%2u (%s) counter=%u  0x%08" PRIX32 "  #%" PRIu32, packet.key,
                 key_name(packet.key), packet.counter, packet.raw, event.repeat);
      }
    }
    this->publish_(packet, event.repeat);
    this->relay_(packet, event.repeat, now);
  });

  // Silent on captures that decode nothing, which is nearly all of them: idle
  // noise produces captures constantly and a session runs to hundreds.
  if (this->dump_frames_ && walk.decoded > 0) {
    ESP_LOGD(TAG, "%u symbols, %u chunk%s, %u frame%s: %s%s", (unsigned) raw.size(),
             (unsigned) walk.chunks, walk.chunks == 1 ? "" : "s", (unsigned) walk.decoded,
             walk.decoded == 1 ? "" : "s", words, walk.decoded > dumped ? ", ..." : "");
  }

  if (!any) {
    ESP_LOGV(TAG, "no valid packet (%u symbols, %u chunks, %u frames decoded)",
             (unsigned) raw.size(), (unsigned) walk.chunks, (unsigned) walk.decoded);
  }
  return any;
}

void FanRfDecoder::loop() {
  // The deadline that says a press was a tap. Nothing else here is periodic.
  this->inject_(this->gate_.poll(esphome::millis()));
}

void FanRfDecoder::send_key(uint8_t key) { this->inject_(this->gate_.originate(key)); }

void FanRfDecoder::relay_(const Packet &packet, uint32_t repeat, uint32_t now) {
  if (!relay_key(this->relay_mode_, packet.key))
    return;
  // A press arms the deadline and injects nothing; the first repeat behind it
  // is what says the button is held. See RelayGate in fan_rf_protocol.h.
  this->inject_(repeat == 0 ? this->gate_.press(packet.key, now)
                            : this->gate_.repeat(packet.key));
}

void FanRfDecoder::inject_(const InjectPlan &plan) {
  if (!plan.any())
    return;
  if (this->transmitter_ == nullptr) {
    ESP_LOGW(TAG, "no transmitter, key %u (%s) not injected", plan.key, key_name(plan.key));
    return;
  }

  auto call = this->transmitter_->transmit();
  auto *data = call.get_data();
  data->reserve((plan.preamble ? PREAMBLE_ENTRIES : 0) + plan.frames * FRAME_ENTRIES);
  // Baseband OOK: the levels are the signal. A carrier would chop every mark
  // into a burst, which is not what comes off the OEM receiver's output pad.
  data->set_carrier_frequency(0);

  // One frame at a time through a 66-entry buffer rather than a whole burst on
  // the stack; the transmitter accumulates the durations anyway.
  int32_t buffer[FRAME_ENTRIES];
  const auto append = [&](size_t entries) {
    for (size_t i = 0; i < entries; i++) {
      if (buffer[i] >= 0) {
        data->mark((uint32_t) buffer[i]);
      } else {
        data->space((uint32_t) -buffer[i]);
      }
    }
  };

  if (plan.preamble)
    append(build_preamble(buffer, FRAME_ENTRIES));
  // Every frame of one press carries the same counter, tap or hold.
  for (uint8_t frame = 0; frame < plan.frames; frame++)
    append(build_frame(plan.key, plan.counter, buffer, FRAME_ENTRIES));

  call.perform();

  if (this->dump_commands_) {
    ESP_LOGD(TAG, "inject key=%2u (%s) counter=%u  0x%08" PRIX32 "  %ux%s", plan.key,
             key_name(plan.key), plan.counter, encode_packet(plan.key, plan.counter),
             (unsigned) plan.frames, plan.preamble ? " +preamble" : "");
  }
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
                (unsigned) SHORT_US, (unsigned) LONG_US, (unsigned) LONG_US, (unsigned) SHORT_US,
                (unsigned) WIDTH_TOLERANCE_PCT);
  ESP_LOGCONFIG(TAG, "  Bit period: %uus +/-%u%%", (unsigned) PERIOD_US,
                (unsigned) PERIOD_TOLERANCE_PCT);
  ESP_LOGCONFIG(TAG, "  Frame split gap: %uus", (unsigned) FRAME_GAP_US);
  ESP_LOGCONFIG(TAG, "  Press: first %u identical frames report once, each frame after repeats",
                (unsigned) this->tracker_.get_press_frames());
  ESP_LOGCONFIG(TAG, "  Run timeout: %" PRIu32 " ms", this->tracker_.get_run_timeout_ms());
  ESP_LOGCONFIG(TAG, "  Dump raw frames: %s", YESNO(this->dump_frames_));
  ESP_LOGCONFIG(TAG, "  Dump commands: %s", YESNO(this->dump_commands_));
  ESP_LOGCONFIG(TAG, "  Buttons: %u", (unsigned) this->sensors_.size());
  ESP_LOGCONFIG(TAG, "  Relay: %s", this->transmitter_ == nullptr ? "no transmitter"
                                    : this->relay_mode_ == RELAY_ALL ? "all keys"
                                    : this->relay_mode_ == RELAY_FAN ? "fan keys"
                                                                     : "off");
  ESP_LOGCONFIG(TAG, "  Tap/hold decision: %" PRIu32 " ms after the press",
                this->gate_.get_decision_delay_ms());
  ESP_LOGCONFIG(TAG, "  Frames injected per tap: %u", (unsigned) this->gate_.get_tap_frames());
}

void FanRfBinarySensor::dump_config() {
  LOG_BINARY_SENSOR("", "Fan RF Button", this);
  ESP_LOGCONFIG(TAG, "  Key: %u (%s)", this->key_, key_name(this->key_));
  ESP_LOGCONFIG(TAG, "  Pulses on held repeats: %s", YESNO(this->repeats_));
}

}  // namespace fan_rf
}  // namespace smarterfan
