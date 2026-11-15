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

void SendspinVisualizer::set_spectrum(uint8_t bins, uint16_t f_min, uint16_t f_max,
                                      sendspin::VisualizerSpectrumScale scale) {
  this->config_.stream.spectrum = sendspin::VisualizerSpectrumConfig{
      .n_disp_bins = bins,
      .scale = scale,
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
                "  Buffer: %u bytes\n"
                "  Layout: %s",
                this->config_.stream.rate_max, spectrum.has_value() ? spectrum->n_disp_bins : 0,
                spectrum.has_value() ? spectrum->f_min : 0, spectrum.has_value() ? spectrum->f_max : 0,
                static_cast<int>(this->config_.display_offset_ms),
                static_cast<unsigned>(this->config_.support.buffer_capacity),
                this->matrix_width_ > 0 ? "matrix" : "strip");
  if (this->matrix_width_ > 0) {
    ESP_LOGCONFIG(TAG, "  Matrix: %ux%u, first LED %s %s, along the %s%s", this->matrix_width_, this->matrix_height_,
                  this->flip_y_ ? "bottom" : "top", this->flip_x_ ? "right" : "left",
                  this->columns_ ? "columns" : "rows", this->serpentine_ ? ", serpentine" : "");
  }
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
void SendspinVisualizer::on_spectrum(int64_t client_timestamp, const std::vector<uint16_t> &bins) {
  const size_t n = std::min<size_t>(bins.size(), MAX_SPECTRUM_BINS);
  for (size_t i = 0; i < n; i++) {
    this->spectrum_[i].store(bins[i], std::memory_order_relaxed);
  }
  this->spectrum_count_.fetch_add(1, std::memory_order_relaxed);
  if (n > 0) {
    this->detect_kick_(client_timestamp, bins[0] * U16_SCALE);
  }
}

// THREAD CONTEXT: drain thread (from on_spectrum)
void SendspinVisualizer::detect_kick_(int64_t client_timestamp, float bass) {
  // A kick is the lowest band jumping 4 dB above its average of the last 0.4 s, at least 250 ms
  // after the last one (up to 240 a minute), and above -50 dB so silence doesn't trigger it. The
  // spectrum is A-weighted, which puts a kick drum's 50-100 Hz 20-30 dB down, hence the low floor.
  constexpr float RISE = 4.0f * DB;
  constexpr float FLOOR = 10.0f * DB;
  constexpr int64_t MIN_GAP_US = 250000;
  const float dt = this->last_spectrum_us_ == 0 ? 0.033f : (client_timestamp - this->last_spectrum_us_) / 1e6f;
  this->last_spectrum_us_ = client_timestamp;
  if (bass > FLOOR && bass - this->bass_average_ > RISE && client_timestamp - this->last_kick_us_ >= MIN_GAP_US) {
    this->last_kick_us_ = client_timestamp;
    this->kick_ms_.store(stamp_now(), std::memory_order_relaxed);
    this->kick_count_.fetch_add(1, std::memory_order_relaxed);
  }
  this->bass_average_ += (1.0f - std::exp(-clamp(dt, 0.0f, 0.5f) / 0.4f)) * (bass - this->bass_average_);
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
  const uint32_t kicks = this->kick_count_.exchange(0, std::memory_order_relaxed);
  if (!this->streaming_ && loudness == 0 && spectrum == 0) {
    return;
  }
  if (this->frames_ > 0) {
    ESP_LOGD(TAG,
             "Last 10 s drawn: %" PRIu32 " frames, brightest byte %u, %" PRIu32 " frames with a pixel in all three "
             "colours (%" PRIu32 " pixels)",
             this->frames_, this->brightest_byte_, this->white_frames_, this->white_pixels_);
    this->frames_ = 0;
    this->brightest_byte_ = 0;
    this->white_pixels_ = 0;
    this->white_frames_ = 0;
  }
  ESP_LOGD(TAG,
           "Last 10 s: %" PRIu32 " loudness, %" PRIu32 " spectrum, %" PRIu32 " beats, %" PRIu32 " onsets, %" PRIu32
           " kicks; loudness now %.0f dB",
           loudness, spectrum, beats, onsets, kicks, this->loudness() * 60.0f - 60.0f);
}

void SendspinVisualizer::note_frame_(light::AddressableLight &it) {
  uint32_t white = 0;
  for (int32_t i = 0; i < it.size(); i++) {
    const auto led = it[i];
    const uint8_t r = led.get_red_raw();
    const uint8_t g = led.get_green_raw();
    const uint8_t b = led.get_blue_raw();
    this->brightest_byte_ = std::max({this->brightest_byte_, r, g, b});
    if (r > 0 && g > 0 && b > 0) {
      white++;
    }
  }
  this->frames_++;
  this->white_pixels_ += white;
  if (white > 0) {
    this->white_frames_++;
  }
}

// --- Getters --------------------------------------------------------------------------------

uint32_t SendspinVisualizer::elapsed_ms_(uint32_t stamp) { return stamp == 0 ? UINT32_MAX : millis() - stamp; }

float SendspinVisualizer::loudness() const { return this->loudness_.load(std::memory_order_relaxed) * U16_SCALE; }

uint32_t SendspinVisualizer::ms_since_beat() const { return elapsed_ms_(this->beat_stamp_()); }

uint32_t SendspinVisualizer::beat_stamp_() const {
  if (this->has_beats_) {
    return this->beat_ms_.load(std::memory_order_relaxed);
  }
  if (this->spectrum_size_ > 0) {
    return this->kick_ms_.load(std::memory_order_relaxed);
  }
  return this->onset_ms_.load(std::memory_order_relaxed);
}

float SendspinVisualizer::spectrum(uint8_t band) const {
  if (band >= this->spectrum_size_) {
    return 0.0f;
  }
  return this->spectrum_[band].load(std::memory_order_relaxed) * U16_SCALE;
}

// --- Layout -------------------------------------------------------------------------------

void SendspinVisualizer::set_matrix(uint8_t width, uint8_t height, bool flip_x, bool flip_y, bool columns,
                                    bool serpentine) {
  this->matrix_width_ = width;
  this->matrix_height_ = height;
  this->flip_x_ = flip_x;
  this->flip_y_ = flip_y;
  this->columns_ = columns;
  this->serpentine_ = serpentine;
}

int32_t SendspinVisualizer::matrix_index_(uint8_t x, uint8_t y) const {
  const uint8_t w = this->matrix_width_;
  const uint8_t h = this->matrix_height_;
  // Count from the corner the first LED is in.
  const uint8_t across = this->flip_x_ ? w - 1 - x : x;
  const uint8_t down = this->flip_y_ ? h - 1 - y : y;
  if (this->columns_) {
    const uint8_t along = this->serpentine_ && (across % 2) ? h - 1 - down : down;
    return across * h + along;
  }
  const uint8_t along = this->serpentine_ && (down % 2) ? w - 1 - across : across;
  return down * w + along;
}

float SendspinVisualizer::matrix_radius_(uint8_t x, uint8_t y) const {
  const float cx = (this->matrix_width_ - 1) / 2.0f;
  const float cy = (this->matrix_height_ - 1) / 2.0f;
  const float corner = std::hypot(cx, cy);
  return corner > 0.0f ? std::hypot(x - cx, y - cy) / corner : 0.0f;
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

  // The peak marker holds for 400 ms, then falls the meter's height in a second.
  const uint32_t now = millis();
  if (this->level_ >= this->level_dot_) {
    this->level_dot_ = this->level_;
    this->level_dot_ms_ = now;
  } else if (now - this->level_dot_ms_ > 400) {
    this->level_dot_ = std::max(0.0f, this->level_dot_ - dt);
  }
  return this->level_;
}

uint8_t SendspinVisualizer::update_bands_(float dt) {
  // Each band shows its top 24 dB below its own recent peak, which falls 3 dB a second and never
  // below -39 dB. The A-weighting leaves the bass bands far below the mids; scaling each band to
  // itself shows them anyway.
  constexpr float MIN_PEAK = 21.0f * DB;
  const uint8_t bands = this->streaming_ ? this->spectrum_size_ : 0;
  for (uint8_t b = 0; b < bands; b++) {
    this->band_peak_[b] = std::max({this->spectrum(b), this->band_peak_[b] - 3.0f * DB * dt, MIN_PEAK});
  }
  return bands;
}

float SendspinVisualizer::band_level_(uint8_t band) const {
  constexpr float RANGE = 24.0f * DB;
  return clamp((this->spectrum(band) - (this->band_peak_[band] - RANGE)) / RANGE, 0.0f, 1.0f);
}

void SendspinVisualizer::update_beat_() {
  // Beats when Music Assistant sends them; otherwise kicks in the bass band; without a spectrum,
  // the stronger onsets, at most one every 150 ms, so hi-hats don't make it flicker.
  const uint32_t stamp = this->beat_stamp_();
  if (!this->streaming_ || stamp == 0 || stamp == this->last_beat_seen_) {
    return;
  }
  const bool onsets_only = !this->has_beats_ && this->spectrum_size_ == 0;
  const bool strong_enough = !onsets_only || this->onset_strength() >= 0.35f;
  const bool spaced = this->last_beat_seen_ == 0 || stamp - this->last_beat_seen_ >= 150;
  if (!strong_enough || !spaced) {
    return;
  }
  const bool downbeat = this->last_beat_was_downbeat();
  this->previous_flash_ = this->flash_;
  this->flash_.start_ms = millis();
  this->flash_.peak = downbeat ? 1.0f : 0.7f;
  this->flash_.hue = (this->previous_flash_.hue + (downbeat ? 90 : 35)) % 360;
  this->last_beat_seen_ = stamp;
}

float SendspinVisualizer::Flash::at(uint32_t now, float delay_ms) const {
  if (this->start_ms == 0) {
    return 0.0f;
  }
  const float age_ms = static_cast<int32_t>(now - this->start_ms) - delay_ms;
  return age_ms < 0.0f ? 0.0f : this->peak * std::exp(-age_ms / 180.0f);
}

Color SendspinVisualizer::beat_color_(uint32_t now, float delay_ms, float glow) const {
  const float current = this->flash_.at(now, delay_ms);
  const float previous = this->previous_flash_.at(now, delay_ms);
  // The wave of a new beat replaces the old one's tail as it passes.
  return current >= previous ? hsv(this->flash_.hue, glow + current) : hsv(this->previous_flash_.hue, glow + previous);
}

void SendspinVisualizer::render_spectrum(light::AddressableLight &it) {
  const float dt = this->frame_seconds_();
  const uint8_t bands = this->update_bands_(dt);
  const float release = std::exp(-dt / 0.12f);
  const int32_t size = it.size();
  it.all() = Color::BLACK;

  if (this->matrix_width_ > 0) {
    // One bar per column, bass on the left, rising from the bottom row.
    const uint8_t w = this->matrix_width_;
    const uint8_t h = this->matrix_height_;
    for (uint8_t x = 0; x < w; x++) {
      const float target = bands > 0 ? this->band_level_(x * bands / w) : 0.0f;
      float &level = this->led_level_[x];
      level = std::max(target, level * release);
      const int hue = 270 * x / std::max<int>(w - 1, 1);
      for (uint8_t y = 0; y < h; y++) {
        const int32_t i = this->matrix_index_(x, y);
        if (i < size) {
          it[i] = hsv(hue, clamp(level * h - (h - 1 - y), 0.0f, 1.0f));
        }
      }
    }
    this->note_frame_(it);
    return;
  }

  // One LED per band, stretched or squeezed to the strip.
  const int32_t leds = std::min<int32_t>(size, MAX_LEDS);
  for (int32_t i = 0; i < leds; i++) {
    const float target = bands > 0 ? this->band_level_(i * bands / leds) : 0.0f;
    float &level = this->led_level_[i];
    level = std::max(target, level * release);
    // Red for the bass to violet for the treble.
    it[i] = hsv(270 * i / std::max<int32_t>(leds - 1, 1), level);
  }
  this->note_frame_(it);
}

void SendspinVisualizer::render_level(light::AddressableLight &it) {
  const float level = this->update_level_(this->frame_seconds_());
  const int32_t size = it.size();
  const Color marker(160, 160, 160);

  if (this->matrix_width_ > 0) {
    // Rings from the centre outwards: green in the middle, yellow, red in the corners. The edge of
    // the lit area is soft, so the meter moves smoothly across the few rings a small grid has.
    constexpr float SOFT = 0.35f;
    it.all() = Color::BLACK;
    for (uint8_t y = 0; y < this->matrix_height_; y++) {
      for (uint8_t x = 0; x < this->matrix_width_; x++) {
        const int32_t i = this->matrix_index_(x, y);
        if (i >= size) {
          continue;
        }
        const float r = this->matrix_radius_(x, y);
        const float lit = clamp((level * (1.0f + SOFT) - r) / SOFT, 0.0f, 1.0f);
        const int hue = r < 0.5f ? 120 : r < 0.85f ? 60 : 0;
        const bool on_marker = this->level_dot_ > 0.02f && std::fabs(r - this->level_dot_) < 0.2f;
        it[i] = on_marker && lit < 0.5f ? marker : hsv(hue, lit);
      }
    }
    this->note_frame_(it);
    return;
  }

  // Both halves grow from the middle; an odd strip shares its middle LED.
  const int32_t half = (size + 1) / 2;
  const float lit = level * half;
  const int32_t dot = this->level_dot_ > 0.02f ? std::min<int32_t>(this->level_dot_ * half, half - 1) : -1;
  for (int32_t k = 0; k < half; k++) {
    const float position = static_cast<float>(k) / half;
    const int hue = position < 0.6f ? 120 : position < 0.85f ? 60 : 0;
    const Color color = k == dot && lit < k + 1 ? marker : hsv(hue, clamp(lit - k, 0.0f, 1.0f));
    it[(size - 1) / 2 - k] = color;
    it[size / 2 + k] = color;
  }
  this->note_frame_(it);
}

void SendspinVisualizer::render_beat(light::AddressableLight &it) {
  const float level = this->update_level_(this->frame_seconds_());
  this->update_beat_();
  const uint32_t now = millis();
  // A glow that follows the loudness, under the flash.
  const float glow = 0.15f * level;
  const int32_t size = it.size();

  if (this->matrix_width_ > 0) {
    // Each flash spreads from the centre and reaches the corners 150 ms later.
    constexpr float SPREAD_MS = 150.0f;
    it.all() = Color::BLACK;
    for (uint8_t y = 0; y < this->matrix_height_; y++) {
      for (uint8_t x = 0; x < this->matrix_width_; x++) {
        const int32_t i = this->matrix_index_(x, y);
        if (i < size) {
          it[i] = this->beat_color_(now, this->matrix_radius_(x, y) * SPREAD_MS, glow);
        }
      }
    }
    this->note_frame_(it);
    return;
  }

  it.all() = this->beat_color_(now, 0.0f, glow);
  this->note_frame_(it);
}

void SendspinVisualizer::render_test(light::AddressableLight &it) {
  // Red, green, blue and white, one per row (or per quarter of a strip) from the top; a dark dot
  // moves a quarter of a second per LED.
  static const Color COLORS[] = {Color(255, 0, 0), Color(0, 255, 0), Color(0, 0, 255), Color(255, 255, 255)};
  const uint32_t step = millis() / 250;
  const int32_t size = it.size();
  it.all() = Color::BLACK;

  if (this->matrix_width_ > 0) {
    // The dot reads along each row, top row first.
    const uint8_t w = this->matrix_width_;
    const uint8_t h = this->matrix_height_;
    const uint32_t dot = step % (w * h);
    for (uint8_t y = 0; y < h; y++) {
      for (uint8_t x = 0; x < w; x++) {
        const int32_t i = this->matrix_index_(x, y);
        if (i < size && y * w + x != dot) {
          it[i] = COLORS[y * 4 / h];
        }
      }
    }
    this->note_frame_(it);
    return;
  }

  // The dot runs along the strip from the first LED.
  for (int32_t i = 0; i < size; i++) {
    if (i != static_cast<int32_t>(step % size)) {
      it[i] = COLORS[i * 4 / size];
    }
  }
  this->note_frame_(it);
}

}  // namespace esphome::sendspin_visualizer

#endif  // USE_SENDSPIN_VISUALIZER
