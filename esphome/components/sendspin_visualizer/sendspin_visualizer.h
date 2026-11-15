#pragma once

#include "esphome/core/defines.h"

#ifdef USE_SENDSPIN_VISUALIZER

#include "esphome/components/light/addressable_light.h"
#include "esphome/components/sendspin/sendspin_hub.h"
#include "esphome/core/color.h"
#include "esphome/core/component.h"

#include <sendspin/visualizer_role.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace esphome::sendspin_visualizer {

/// Most spectrum bands kept; matches MAX_SPECTRUM_BINS in __init__.py.
static constexpr uint8_t MAX_SPECTRUM_BINS = 64;
/// Most LEDs the effects track per-LED state for; LEDs past this stay dark.
static constexpr uint16_t MAX_LEDS = 256;

/// Receives Sendspin visualizer data and draws it on an addressable light.
///
/// THREAD CONTEXT: the library calls the data callbacks (on_loudness, on_beat, on_spectrum,
/// on_peak) on its drain thread, display_offset ahead of each value's playback time; they only
/// store atomics. Stream start/end/clear, the getters and the render functions run on the main
/// loop.
///
/// All levels are 0 to 1, linear in dB from -60 dB (or silence) to full scale, A-weighted, as the
/// server measures them before the node's own volume.
class SendspinVisualizer : public sendspin_::SendspinChild, public sendspin::VisualizerRoleListener {
 public:
  void setup() override;
  void dump_config() override;

  void set_rate_max(uint16_t rate_max) { this->config_.stream.rate_max = rate_max; }
  void set_spectrum(uint8_t bins, uint16_t f_min, uint16_t f_max, sendspin::VisualizerSpectrumScale scale);
  /// Lays the effects out on a grid of width x height LEDs instead of along a line. flip_x and flip_y:
  /// the first LED is at the right, or at the bottom. columns: the second LED is above or below the
  /// first, not beside it. serpentine: each row (or column) runs the other way from the one before.
  void set_matrix(uint8_t width, uint8_t height, bool flip_x, bool flip_y, bool columns, bool serpentine);
  void set_display_offset_ms(int32_t ms) { this->config_.display_offset_ms = ms; }
  void set_buffer_capacity(size_t bytes) { this->config_.support.buffer_capacity = bytes; }

  /// True between the server's visualizer stream start and its end.
  bool is_streaming() const { return this->streaming_; }
  /// True when the stream carries beats. Music Assistant adds them once its audio analysis has
  /// the track, and leaves them out for tracks it cannot analyse.
  bool has_beats() const { return this->has_beats_; }

  float loudness() const;
  /// Milliseconds since the last beat. While the stream has no beats: since the last kick (the
  /// lowest spectrum band jumping), or without a spectrum, since the last onset.
  uint32_t ms_since_beat() const;
  bool last_beat_was_downbeat() const { return this->has_beats_ && this->downbeat_.load(std::memory_order_relaxed); }
  /// Milliseconds since the last onset (a transient, independent of the beat), and its strength.
  uint32_t ms_since_onset() const { return elapsed_ms_(this->onset_ms_.load(std::memory_order_relaxed)); }
  float onset_strength() const { return this->onset_strength_.load(std::memory_order_relaxed) / 255.0f; }
  /// Spectrum bands, low to high frequency, as the server sends them.
  uint8_t spectrum_size() const { return this->spectrum_size_; }
  float spectrum(uint8_t band) const;

  /// Each band scaled to its own recent peak, red for the bass to violet for the treble. On a strip,
  /// one LED per band (stretched or squeezed to the strip); on a matrix, one bar per column, bass on
  /// the left.
  void render_spectrum(light::AddressableLight &it);
  /// A level meter with a falling peak marker, green to red: from the middle of a strip outwards, or
  /// in rings from the centre of a matrix.
  void render_level(light::AddressableLight &it);
  /// The LEDs flash on each beat (see ms_since_beat()) and step to the next colour; downbeats flash
  /// brighter. On a matrix the flash spreads from the centre to the corners.
  void render_beat(light::AddressableLight &it);
  /// Red, green, blue and white, one per row from the top (on a strip, one per quarter from the first
  /// LED), with a dark dot that moves a quarter of a second per LED. On a matrix the dot reads along
  /// each row, top row first. Shows whether the channel order and the matrix settings match the LEDs.
  void render_test(light::AddressableLight &it);

  // VisualizerRoleListener, drain thread
  void on_loudness(int64_t client_timestamp, uint16_t loudness) override;
  void on_beat(int64_t client_timestamp, bool downbeat) override;
  void on_spectrum(int64_t client_timestamp, const std::vector<uint16_t> &bins) override;
  void on_peak(int64_t client_timestamp, uint8_t strength) override;
  // VisualizerRoleListener, main loop
  void on_visualizer_stream_start(const sendspin::ServerVisualizerStreamObject &stream) override;
  void on_visualizer_stream_end() override;
  void on_visualizer_stream_clear() override;

 protected:
  /// Milliseconds since a stored millis() stamp; UINT32_MAX when nothing has been stored.
  static uint32_t elapsed_ms_(uint32_t stamp);
  /// Seconds since the previous render, capped so a stall does not jump the decays.
  float frame_seconds_();
  /// The loudness scaled to its recent peak and smoothed, 0 to 1; also moves the peak marker.
  float update_level_(float dt);
  /// Follows each band's recent peak; returns the number of bands to draw.
  uint8_t update_bands_(float dt);
  /// A band scaled to its recent peak, 0 to 1.
  float band_level_(uint8_t band) const;
  /// Starts a flash on a new beat, kick or strong onset (see ms_since_beat()).
  void update_beat_();
  /// The millis() stamp of the last beat, or of what stands in for beats (see ms_since_beat()).
  uint32_t beat_stamp_() const;
  /// Finds kicks in the lowest spectrum band, for when the stream has no beats.
  void detect_kick_(int64_t client_timestamp, float bass);
  /// The flash colour delay_ms after the beat, over the glow.
  Color beat_color_(uint32_t now, float delay_ms, float glow) const;
  /// LED index of grid cell (x, y), x counted from the left and y from the top.
  int32_t matrix_index_(uint8_t x, uint8_t y) const;
  /// A cell's distance from the grid's centre, 0 to 1 in the corners.
  float matrix_radius_(uint8_t x, uint8_t y) const;
  void log_stats_();
  /// Notes the brightest byte and the pixels with all three colours lit in a frame just drawn, for
  /// the stats log line.
  void note_frame_(light::AddressableLight &it);

  sendspin::VisualizerRoleConfig config_{};
  bool streaming_{false};
  bool has_beats_{false};
  uint8_t spectrum_size_{0};
  // Grid layout; a width of 0 means a strip.
  uint8_t matrix_width_{0};
  uint8_t matrix_height_{0};
  bool flip_x_{false};
  bool flip_y_{false};
  bool columns_{false};
  bool serpentine_{false};

  // Written on the drain thread, read on the main loop.
  std::atomic<uint16_t> loudness_{0};
  std::atomic<uint32_t> beat_ms_{0};
  std::atomic<bool> downbeat_{false};
  std::atomic<uint32_t> onset_ms_{0};
  std::atomic<uint8_t> onset_strength_{0};
  std::array<std::atomic<uint16_t>, MAX_SPECTRUM_BINS> spectrum_{};
  std::atomic<uint32_t> loudness_count_{0};
  std::atomic<uint32_t> spectrum_count_{0};
  std::atomic<uint32_t> beat_count_{0};
  std::atomic<uint32_t> onset_count_{0};
  std::atomic<uint32_t> kick_ms_{0};
  std::atomic<uint32_t> kick_count_{0};

  // Kick detection, drain thread only.
  int64_t last_spectrum_us_{0};
  int64_t last_kick_us_{0};
  float bass_average_{0.0f};

  // Frame stats for the log, main loop only.
  uint32_t frames_{0};
  uint8_t brightest_byte_{0};
  uint32_t white_pixels_{0};
  uint32_t white_frames_{0};

  // Render state, main loop only.
  uint32_t last_render_ms_{0};
  std::array<float, MAX_SPECTRUM_BINS> band_peak_{};
  std::array<float, MAX_LEDS> led_level_{};
  float level_{0.0f};
  float level_ceiling_{0.0f};
  float level_dot_{0.0f};
  uint32_t level_dot_ms_{0};
  uint32_t last_beat_seen_{0};
  /// A beat's flash: when it started, how bright, and in what colour.
  struct Flash {
    uint32_t start_ms{0};
    float peak{0.0f};
    uint16_t hue{0};
    /// Brightness delay_ms after the start, fading with a 180 ms time constant.
    float at(uint32_t now, float delay_ms) const;
  };
  Flash flash_{};
  Flash previous_flash_{};
};

}  // namespace esphome::sendspin_visualizer

#endif  // USE_SENDSPIN_VISUALIZER
