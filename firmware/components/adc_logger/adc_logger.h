// Continuous raw ADC streamer.
//
// Samples one ADC pin and streams every sample to the log, unconditionally,
// for as long as the firmware runs. No trigger, no ring buffer, no cleverness
// about what is or is not a transmission -- earlier revisions of this file had
// all three, and each one turned out to be deciding what we got to look at.
// The point here is to see the pin exactly as an oscilloscope would.
//
// Wiring (2:1 divider keeps 4.48 V inside the ADC's ~3.1 V range):
//     pad --[10k]-- GPIO3 --[10k]-- GND
//
// Throughput is the real constraint. Every sample costs 3 hex characters, and
// the logger sustains roughly 20-30 kchar/s over USB-CDC, so ~10 kSa/s streams
// comfortably. Higher rates drop chunks -- which is reported rather than
// hidden, because a silent gap would corrupt the time axis of the plot.

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esp_adc/adc_continuous.h"
#include "esp_heap_caps.h"

namespace smarterfan {

static const char *const ADCLOG_TAG = "adclog";

static const size_t SAMPLES_PER_LINE = 64;
static const size_t ADCLOG_FRAME = 1024;      // bytes per adc_continuous_read
static const uint8_t LINES_PER_LOOP = 8;      // emitted per loop() iteration

class AdcLogger : public esphome::Component {
 public:
  void set_channel(uint8_t ch) { this->channel_ = ch; }
  void set_sample_hz(uint32_t hz) { this->sample_hz_ = hz; }
  void set_queue_lines(size_t n) { this->queue_lines_ = n; }

  void setup() override {
    this->queue_ = (uint16_t *) heap_caps_malloc(
        this->queue_lines_ * SAMPLES_PER_LINE * sizeof(uint16_t), MALLOC_CAP_8BIT);
    this->queue_start_ = (uint32_t *) heap_caps_malloc(
        this->queue_lines_ * sizeof(uint32_t), MALLOC_CAP_8BIT);
    this->frame_ = (uint8_t *) malloc(ADCLOG_FRAME);
    if (!this->queue_ || !this->queue_start_ || !this->frame_) {
      ESP_LOGE(ADCLOG_TAG, "allocation failed (%u KB heap free)",
               (unsigned) (esp_get_free_heap_size() / 1024));
      this->mark_failed();
      return;
    }

    adc_continuous_handle_cfg_t handle_cfg = {};
    handle_cfg.max_store_buf_size = ADCLOG_FRAME * 8;
    handle_cfg.conv_frame_size = ADCLOG_FRAME;
    if (adc_continuous_new_handle(&handle_cfg, &this->handle_) != ESP_OK) {
      ESP_LOGE(ADCLOG_TAG, "adc_continuous_new_handle failed");
      this->mark_failed();
      return;
    }

    adc_digi_pattern_config_t pattern = {};
    pattern.atten = ADC_ATTEN_DB_12;          // ~0-3.1 V full scale
    pattern.channel = this->channel_;
    pattern.unit = ADC_UNIT_1;                // ADC2 is unusable with Wi-Fi
    pattern.bit_width = ADC_BITWIDTH_12;

    adc_continuous_config_t cfg = {};
    cfg.pattern_num = 1;
    cfg.adc_pattern = &pattern;
    cfg.sample_freq_hz = this->sample_hz_;
    cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
    if (adc_continuous_config(this->handle_, &cfg) != ESP_OK ||
        adc_continuous_start(this->handle_) != ESP_OK) {
      ESP_LOGE(ADCLOG_TAG, "adc_continuous config/start failed");
      this->mark_failed();
      return;
    }
    ESP_LOGI(ADCLOG_TAG, "STREAM rate=%u channel=%u atten=12dB divider=2.0",
             (unsigned) this->sample_hz_, this->channel_);
  }

  float get_setup_priority() const override {
    return esphome::setup_priority::HARDWARE;
  }

  void loop() override {
    if (this->handle_ == nullptr) return;
    this->collect_();
    this->emit_();
  }

 protected:
  // Drain the ADC driver into whole lines. A line is only queued if there is
  // room; otherwise it is dropped and counted, so the gap shows up in the log
  // instead of silently shortening the time axis.
  void collect_() {
    uint32_t got = 0;
    while (adc_continuous_read(this->handle_, this->frame_, ADCLOG_FRAME, &got, 0)
           == ESP_OK && got > 0) {
      for (uint32_t i = 0; i < got; i += SOC_ADC_DIGI_RESULT_BYTES) {
        auto *p = (adc_digi_output_data_t *) &this->frame_[i];
        if (this->fill_ == 0) this->pending_start_ = this->total_;
        this->pending_[this->fill_++] = p->type2.data & 0x0FFF;
        this->total_++;
        if (this->fill_ == SAMPLES_PER_LINE) {
          this->push_line_();
          this->fill_ = 0;
        }
      }
    }
  }

  void push_line_() {
    if (this->queued_ >= this->queue_lines_) {
      this->dropped_++;      // logger is behind; losing this line is honest
      return;
    }
    size_t slot = (this->head_ + this->queued_) % this->queue_lines_;
    memcpy(&this->queue_[slot * SAMPLES_PER_LINE], this->pending_,
           SAMPLES_PER_LINE * sizeof(uint16_t));
    this->queue_start_[slot] = this->pending_start_;
    this->queued_++;
  }

  void emit_() {
    char line[SAMPLES_PER_LINE * 3 + 8];
    for (uint8_t n = 0; n < LINES_PER_LOOP && this->queued_ > 0; n++) {
      size_t slot = this->head_;
      size_t pos = 0;
      for (size_t i = 0; i < SAMPLES_PER_LINE; i++) {
        pos += snprintf(line + pos, sizeof(line) - pos, "%03X",
                        this->queue_[slot * SAMPLES_PER_LINE + i]);
      }
      ESP_LOGI(ADCLOG_TAG, "S %u %s", (unsigned) this->queue_start_[slot], line);
      this->head_ = (this->head_ + 1) % this->queue_lines_;
      this->queued_--;
    }
    // Re-announce the stream parameters periodically. Printing them only from
    // setup() means any log started after boot -- which is the normal case --
    // has no header at all, and the decoder cannot know the sample rate.
    const uint32_t now = esphome::millis();
    if (now - this->last_header_ > 5000) {
      ESP_LOGI(ADCLOG_TAG, "STREAM rate=%u channel=%u atten=12dB divider=2.0",
               (unsigned) this->sample_hz_, this->channel_);
      this->last_header_ = now;
    }
    if (this->dropped_ != this->reported_ &&
        esphome::millis() - this->last_report_ > 1000) {
      ESP_LOGW(ADCLOG_TAG, "DROP %u lines total (logger cannot keep up; "
               "lower sample_rate)", (unsigned) this->dropped_);
      this->reported_ = this->dropped_;
      this->last_report_ = esphome::millis();
    }
  }

  adc_continuous_handle_t handle_{nullptr};
  uint8_t *frame_{nullptr};
  uint16_t *queue_{nullptr};
  uint32_t *queue_start_{nullptr};
  uint16_t pending_[SAMPLES_PER_LINE]{};
  uint32_t pending_start_{0};
  size_t fill_{0}, head_{0}, queued_{0}, queue_lines_{32};
  uint32_t total_{0}, dropped_{0}, reported_{0}, last_report_{0}, last_header_{0};
  uint32_t sample_hz_{10000};
  uint8_t channel_{3};
};

}  // namespace smarterfan
