#include "sendspin_visualizer.h"

#ifdef USE_SENDSPIN_VISUALIZER

#include "esphome/core/color.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <memory>

namespace esphome::sendspin_visualizer {

static const char *const TAG = "sendspin_visualizer";

namespace {

// ESPHome 2026.10.2's hub keeps its SendspinClient protected and never adds the visualizer role.
// An explicit template instantiation may name a protected member, so this reaches the hub's
// client_ without changing the hub. Replace it with SendspinHub::set_visualizer() once
// esphome/esphome#19276 is in a release.
using ClientMember = std::unique_ptr<sendspin::SendspinClient> sendspin_::SendspinHub::*;
ClientMember hub_client_member();
template<ClientMember M> struct HubClientAccess {
  friend ClientMember hub_client_member() { return M; }
};
template struct HubClientAccess<&sendspin_::SendspinHub::client_>;

constexpr float U16_SCALE = 1.0f / 65535.0f;
// One dB on the 0-1 scale (60 dB across it).
constexpr float DB = 1.0f / 60.0f;

const char *type_name(sendspin::VisualizerDataType type) {
  switch (type) {
    case sendspin::VisualizerDataType::BEAT:
      return "beat";
    case sendspin::VisualizerDataType::LOUDNESS:
      return "loudness";
    case sendspin::VisualizerDataType::F_PEAK:
      return "f_peak";
    case sendspin::VisualizerDataType::SPECTRUM:
      return "spectrum";
    case sendspin::VisualizerDataType::PEAK:
      return "peak";
  }
  return "?";
}

Color hsv(int hue, float value) {
  float r, g, b;
  hsv_to_rgb(hue % 360, 1.0f, clamp(value, 0.0f, 1.0f), r, g, b);
  return Color(static_cast<uint8_t>(r * 255.0f), static_cast<uint8_t>(g * 255.0f), static_cast<uint8_t>(b * 255.0f));
}

uint32_t stamp_now() { return std::max<uint32_t>(millis(), 1); }

}  // namespace

// --- Setup ----------------------------------------------------------------------------------

void SendspinVisualizer::set_spectrum(uint8_t bins, uint16_t f_min, uint16_t f_max) {
  this->config_.stream.spectrum = sendspin::VisualizerSpectrumConfig{
      .n_disp_bins = bins,
      .scale = sendspin::VisualizerSpectrumScale::MEL,
      .f_min = f_min,
      .f_max = f_max,
  };
}

// THREAD CONTEXT: Main loop, after the hub's setup() made the client and before its loop() starts it
void SendspinVisualizer::setup() {
  auto &client = this->parent_->*hub_client_member();
  if (client == nullptr || client->is_started()) {
    ESP_LOGE(TAG, "The Sendspin client is missing or already started; the visualizer role must be added first");
    this->mark_failed();
    return;
  }

  auto &types = this->config_.stream.types;
  types = {sendspin::VisualizerDataType::LOUDNESS, sendspin::VisualizerDataType::BEAT,
           sendspin::VisualizerDataType::PEAK};
  if (this->config_.stream.spectrum.has_value()) {
    types.push_back(sendspin::VisualizerDataType::SPECTRUM);
  }

  client->add_visualizer(this->config_).set_listener(this);
  this->set_interval("stats", 10000, [this]() { this->log_stats_(); });
}

void SendspinVisualizer::dump_config() {
  const auto &spectrum = this->config_.stream.spectrum;
  ESP_LOGCONFIG(TAG,
                "Sendspin Visualizer:\n"
                "  Rate: %u frames/s\n"
                "  Spectrum: %u bands, %u-%u Hz\n"
                "  Display offset: %d ms\n"
                "  Buffer: %u bytes",
                this->config_.stream.rate_max, spectrum.has_value() ? spectrum->n_disp_bins : 0,
                spectrum.has_value() ? spectrum->f_min : 0, spectrum.has_value() ? spectrum->f_max : 0,
                static_cast<int>(this->config_.display_offset_ms),
                static_cast<unsigned>(this->config_.support.buffer_capacity));
}

// --- Data from the library --------------------------------------------------------------------

// THREAD CONTEXT: drain thread
void SendspinVisualizer::on_loudness(int64_t /*client_timestamp*/, uint16_t loudness) {
  this->loudness_.store(loudness, std::memory_order_relaxed);
  this->loudness_count_.fetch_add(1, std::memory_order_relaxed);
}

// THREAD CONTEXT: drain thread
void SendspinVisualizer::on_beat(int64_t /*client_timestamp*/, bool downbeat) {
  this->downbeat_.store(downbeat, std::memory_order_relaxed);
  this->beat_ms_.store(stamp_now(), std::memory_order_relaxed);
  this->beat_count_.fetch_add(1, std::memory_order_relaxed);
}

// THREAD CONTEXT: drain thread
void SendspinVisualizer::on_spectrum(int64_t /*client_timestamp*/, const std::vector<uint16_t> &bins) {
  const size_t n = std::min<size_t>(bins.size(), MAX_SPECTRUM_BINS);
  for (size_t i = 0; i < n; i++) {
    this->spectrum_[i].store(bins[i], std::memory_order_relaxed);
  }
  this->spectrum_count_.fetch_add(1, std::memory_order_relaxed);
}

// THREAD CONTEXT: drain thread
void SendspinVisualizer::on_peak(int64_t /*client_timestamp*/, uint8_t strength) {
  this->onset_strength_.store(strength, std::memory_order_relaxed);
  this->onset_ms_.store(stamp_now(), std::memory_order_relaxed);
  this->onset_count_.fetch_add(1, std::memory_order_relaxed);
}

// THREAD CONTEXT: main loop
void SendspinVisualizer::on_visualizer_stream_start(const sendspin::ServerVisualizerStreamObject &stream) {
  std::string types;
  this->has_beats_ = false;
  for (auto type : stream.types) {
    if (!types.empty()) {
      types += ", ";
    }
    types += type_name(type);
    if (type == sendspin::VisualizerDataType::BEAT) {
      this->has_beats_ = true;
    }
  }
  this->spectrum_size_ =
      stream.spectrum.has_value() ? std::min<uint8_t>(stream.spectrum->n_disp_bins, MAX_SPECTRUM_BINS) : 0;
  this->streaming_ = true;
  ESP_LOGI(TAG, "Stream started: %s; %u frames/s, %u spectrum bands, downbeats %s", types.c_str(), stream.rate_max,
           this->spectrum_size_, YESNO(stream.tracks_downbeats));
}

// THREAD CONTEXT: main loop
void SendspinVisualizer::on_visualizer_stream_end() {
  ESP_LOGI(TAG, "Stream ended");
  this->on_visualizer_stream_clear();
  this->streaming_ = false;
}

// THREAD CONTEXT: main loop
void SendspinVisualizer::on_visualizer_stream_clear() {
  this->loudness_.store(0, std::memory_order_relaxed);
  for (auto &bin : this->spectrum_) {
    bin.store(0, std::memory_order_relaxed);
  }
}

void SendspinVisualizer::log_stats_() {
  const uint32_t loudness = this->loudness_count_.exchange(0, std::memory_order_relaxed);
  const uint32_t spectrum = this->spectrum_count_.exchange(0, std::memory_order_relaxed);
  const uint32_t beats = this->beat_count_.exchange(0, std::memory_order_relaxed);
  const uint32_t onsets = this->onset_count_.exchange(0, std::memory_order_relaxed);
  if (!this->streaming_ && loudness == 0 && spectrum == 0) {
    return;
  }
  ESP_LOGD(TAG,
           "Last 10 s: %" PRIu32 " loudness, %" PRIu32 " spectrum, %" PRIu32 " beats, %" PRIu32
           " onsets; loudness now %.0f dB",
           loudness, spectrum, beats, onsets, this->loudness() * 60.0f - 60.0f);
}

// --- Getters --------------------------------------------------------------------------------

uint32_t SendspinVisualizer::elapsed_ms_(uint32_t stamp) { return stamp == 0 ? UINT32_MAX : millis() - stamp; }

float SendspinVisualizer::loudness() const { return this->loudness_.load(std::memory_order_relaxed) * U16_SCALE; }

uint32_t SendspinVisualizer::ms_since_beat() const {
  return elapsed_ms_((this->has_beats_ ? this->beat_ms_ : this->onset_ms_).load(std::memory_order_relaxed));
}

float SendspinVisualizer::spectrum(uint8_t band) const {
  if (band >= this->spectrum_size_) {
    return 0.0f;
  }
  return this->spectrum_[band].load(std::memory_order_relaxed) * U16_SCALE;
}

// --- Effects --------------------------------------------------------------------------------

float SendspinVisualizer::frame_seconds_() {
  const uint32_t now = millis();
  const float dt = this->last_render_ms_ == 0 ? 0.016f : (now - this->last_render_ms_) / 1000.0f;
  this->last_render_ms_ = now;
  return clamp(dt, 0.0f, 0.1f);
}

float SendspinVisualizer::update_level_(float dt) {
  // The meter shows the top 30 dB below a ceiling that follows the music's recent peak, falling
  // 2 dB a second and never below -33 dB, so a quiet passage does not fill the meter with noise.
  constexpr float RANGE = 30.0f * DB;
  constexpr float MIN_CEILING = 27.0f * DB;
  const float v = this->streaming_ ? this->loudness() : 0.0f;
  this->level_ceiling_ = std::max({v, this->level_ceiling_ - 2.0f * DB * dt, MIN_CEILING});
  const float target = clamp((v - (this->level_ceiling_ - RANGE)) / RANGE, 0.0f, 1.0f);
  // Rises at once, falls with a 150 ms time constant.
  this->level_ = std::max(target, this->level_ * std::exp(-dt / 0.15f));
  return this->level_;
}

void SendspinVisualizer::render_spectrum(light::AddressableLight &it) {
  // Each band shows its top 24 dB below its own recent peak, which falls 3 dB a second and never
  // below -39 dB. The A-weighting leaves the bass bands far below the mids; scaling each band to
  // itself shows them anyway.
  constexpr float RANGE = 24.0f * DB;
  constexpr float MIN_PEAK = 21.0f * DB;
  const float dt = this->frame_seconds_();
  const uint8_t bands = this->streaming_ ? this->spectrum_size_ : 0;
  for (uint8_t b = 0; b < bands; b++) {
    this->band_peak_[b] = std::max({this->spectrum(b), this->band_peak_[b] - 3.0f * DB * dt, MIN_PEAK});
  }

  const int32_t size = it.size();
  const int32_t leds = std::min<int32_t>(size, MAX_LEDS);
  const float release = std::exp(-dt / 0.12f);
  for (int32_t i = 0; i < leds; i++) {
    float target = 0.0f;
    if (bands > 0) {
      const uint8_t b = static_cast<uint8_t>(i * bands / leds);
      target = clamp((this->spectrum(b) - (this->band_peak_[b] - RANGE)) / RANGE, 0.0f, 1.0f);
    }
    float &level = this->led_level_[i];
    level = std::max(target, level * release);
    // Red for the bass to violet for the treble.
    it[i] = hsv(270 * i / std::max<int32_t>(leds - 1, 1), level);
  }
  for (int32_t i = leds; i < size; i++) {
    it[i] = Color::BLACK;
  }
}

void SendspinVisualizer::render_level(light::AddressableLight &it) {
  const float dt = this->frame_seconds_();
  const float level = this->update_level_(dt);
  const uint32_t now = millis();

  // The peak dot holds for 400 ms, then falls the meter's height in a second.
  if (level >= this->level_dot_) {
    this->level_dot_ = level;
    this->level_dot_ms_ = now;
  } else if (now - this->level_dot_ms_ > 400) {
    this->level_dot_ = std::max(0.0f, this->level_dot_ - dt);
  }

  // Both halves grow from the middle; an odd strip shares its middle LED.
  const int32_t size = it.size();
  const int32_t half = (size + 1) / 2;
  const float lit = level * half;
  const int32_t dot = this->level_dot_ > 0.02f ? std::min<int32_t>(this->level_dot_ * half, half - 1) : -1;
  for (int32_t k = 0; k < half; k++) {
    const float position = static_cast<float>(k) / half;
    const int hue = position < 0.6f ? 120 : position < 0.85f ? 60 : 0;
    Color color = hsv(hue, clamp(lit - k, 0.0f, 1.0f));
    if (k == dot && lit < k + 1) {
      color = Color(160, 160, 160);
    }
    it[(size - 1) / 2 - k] = color;
    it[size / 2 + k] = color;
  }
}

void SendspinVisualizer::render_beat(light::AddressableLight &it) {
  const float dt = this->frame_seconds_();
  const float level = this->update_level_(dt);

  // Beats once Music Assistant has them; until then onsets, only the stronger ones and at most
  // one every 150 ms, so hi-hats don't make it flicker.
  const uint32_t stamp = (this->has_beats_ ? this->beat_ms_ : this->onset_ms_).load(std::memory_order_relaxed);
  if (this->streaming_ && stamp != 0 && stamp != this->last_beat_seen_) {
    const bool strong_enough = this->has_beats_ || this->onset_strength() >= 0.35f;
    const bool spaced = this->last_beat_seen_ == 0 || stamp - this->last_beat_seen_ >= 150;
    if (strong_enough && spaced) {
      const bool downbeat = this->last_beat_was_downbeat();
      this->beat_hue_ = (this->beat_hue_ + (downbeat ? 90 : 35)) % 360;
      this->beat_flash_ = downbeat ? 1.0f : 0.7f;
      this->last_beat_seen_ = stamp;
    }
  }
  this->beat_flash_ *= std::exp(-dt / 0.18f);

  // A glow that follows the loudness, under the flash.
  const Color color = hsv(this->beat_hue_, 0.15f * level + this->beat_flash_);
  for (int32_t i = 0; i < it.size(); i++) {
    it[i] = color;
  }
}

}  // namespace esphome::sendspin_visualizer

#endif  // USE_SENDSPIN_VISUALIZER
