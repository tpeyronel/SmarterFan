// ESPHome binding for the Novohome NH-VTR500 remote decoder.
//
// Registers as a listener on `remote_receiver` and hands each raw capture to
// the framework-free decoder in fan_rf_protocol.h, which is where the protocol
// actually lives. This file is only plumbing: dedupe, trigger dispatch, and
// per-command binary sensors.
//
// This replaces the three `rc_switch_raw` binary sensors that used to be in
// sniffer.yaml. Those could not work: RCSwitchBase::decode() syncs once and
// reads bits from offset 0, so it only ever sees frame 1 of the five repeats --
// the one the receiver's AGC corrupts while it settles. Measured on a cold
// press, frame 1 decodes 40% of the time and frames 3-5 decode 100% of the
// time. Scanning past the damaged frame is the entire point of this component.

#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/remote_base/remote_base.h"

#include "fan_rf_protocol.h"

#include <vector>

namespace smarterfan {
namespace fan_rf {

class FanRfBinarySensor;

class FanRfDecoder : public esphome::Component, public esphome::remote_base::RemoteReceiverListener {
 public:
  void set_dedup_window(uint32_t ms) { this->filter_.set_window_ms(ms); }
  void register_binary_sensor(FanRfBinarySensor *sensor) { this->sensors_.push_back(sensor); }
  void add_on_code_callback(std::function<void(uint8_t, uint8_t, uint32_t)> &&callback) {
    this->callbacks_.add(std::move(callback));
  }

  void dump_config() override;
  bool on_receive(esphome::remote_base::RemoteReceiveData data) override;

  // Exposed so a `remote_receiver: on_raw:` lambda can drive the decoder too,
  // and so the same entry point is reachable from a test. Registering as a
  // listener is the normal path; using both would just decode twice, and the
  // press-counter dedupe would collapse the second one anyway.
  bool process_raw(const std::vector<int32_t> &raw);

 protected:
  void publish_(const Packet &packet);

  std::vector<FanRfBinarySensor *> sensors_;
  esphome::CallbackManager<void(uint8_t, uint8_t, uint32_t)> callbacks_;
  PressFilter filter_;
};

class FanRfBinarySensor : public esphome::binary_sensor::BinarySensorInitiallyOff,
                          public esphome::Component {
 public:
  void set_command(uint8_t command) { this->command_ = command; }
  uint8_t get_command() const { return this->command_; }
  void dump_config() override;

 protected:
  uint8_t command_{0};
};

// on_code: fires once per press with (command, counter, code).
class FanRfCodeTrigger : public esphome::Trigger<uint8_t, uint8_t, uint32_t> {
 public:
  explicit FanRfCodeTrigger(FanRfDecoder *parent) {
    parent->add_on_code_callback(
        [this](uint8_t command, uint8_t counter, uint32_t code) { this->trigger(command, counter, code); });
  }
};

}  // namespace fan_rf
}  // namespace smarterfan
