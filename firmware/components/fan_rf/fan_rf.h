// ESPHome binding for the Novohome NH-VTR500 remote decoder.
//
// Registers as a listener on `remote_receiver` and runs each raw capture down
// the three stages in fan_rf_protocol.h: capture -> frames -> packets ->
// presses. This file is only plumbing: the two console dumps, and dispatch to
// the trigger and the per-key binary sensors.
//
// Frames are processed one at a time, in the order they arrive, and the press
// tracker is fed per frame rather than per capture. That makes the component
// independent of `remote_receiver`'s `idle` setting: at 4 ms each of a burst's
// five repeats is its own capture, at 12 ms all five sit in one, and either way
// a tap produces one event and a hold produces one event plus a repeat per
// extra frame.

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/remote_base/remote_base.h"

#include "fan_rf_protocol.h"

#include <vector>

namespace smarterfan {
namespace fan_rf {

// Frames listed on one `dump_frames` line before it gives up and says how many
// more there were. A capture normally holds exactly one frame -- `idle` sits
// below the inter-frame gap -- and at most five when it does not, so this only
// bites on a held button captured whole.
static const uint8_t MAX_DUMPED_FRAMES = 8;

class FanRfBinarySensor;

class FanRfDecoder : public esphome::Component, public esphome::remote_base::RemoteReceiverListener {
 public:
  void set_press_frames(uint8_t frames) { this->tracker_.set_press_frames(frames); }
  void set_run_timeout(uint32_t ms) { this->tracker_.set_run_timeout_ms(ms); }
  void set_dump_frames(bool dump) { this->dump_frames_ = dump; }
  void set_dump_commands(bool dump) { this->dump_commands_ = dump; }
  void register_binary_sensor(FanRfBinarySensor *sensor) { this->sensors_.push_back(sensor); }
  void add_on_code_callback(std::function<void(uint8_t, uint8_t, uint32_t, uint32_t)> &&callback) {
    this->callbacks_.add(std::move(callback));
  }

  void dump_config() override;
  bool on_receive(esphome::remote_base::RemoteReceiveData data) override;

  // Exposed so a `remote_receiver: on_raw:` lambda can drive the decoder too,
  // and so the same entry point is reachable from a test. Registering as a
  // listener is the normal path; using both would feed every frame twice, which
  // would be counted as a held button.
  bool process_raw(const std::vector<int32_t> &raw);

 protected:
  void publish_(const Packet &packet, uint32_t repeat);

  std::vector<FanRfBinarySensor *> sensors_;
  esphome::CallbackManager<void(uint8_t, uint8_t, uint32_t, uint32_t)> callbacks_;
  PressTracker tracker_;
  bool dump_frames_{false};
  bool dump_commands_{true};
};

class FanRfBinarySensor : public esphome::binary_sensor::BinarySensorInitiallyOff,
                          public esphome::Component {
 public:
  void set_key(uint8_t key) { this->key_ = key; }
  uint8_t get_key() const { return this->key_; }
  void set_repeats(bool repeats) { this->repeats_ = repeats; }
  bool get_repeats() const { return this->repeats_; }
  void dump_config() override;

 protected:
  uint8_t key_{0};
  bool repeats_{true};
};

// on_code: fires with (key, counter, code, repeat). `repeat` is 0 for the press
// itself and 1, 2, 3... for each frame of a hold beyond it.
class FanRfCodeTrigger : public esphome::Trigger<uint8_t, uint8_t, uint32_t, uint32_t> {
 public:
  explicit FanRfCodeTrigger(FanRfDecoder *parent) {
    parent->add_on_code_callback([this](uint8_t key, uint8_t counter, uint32_t code,
                                        uint32_t repeat) { this->trigger(key, counter, code, repeat); });
  }
};

}  // namespace fan_rf
}  // namespace smarterfan
