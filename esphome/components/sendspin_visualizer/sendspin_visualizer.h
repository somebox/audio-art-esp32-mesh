#pragma once

#include "esphome/core/defines.h"

#ifdef USE_SENDSPIN_VISUALIZER

#include "esphome/components/light/addressable_light.h"
#include "esphome/components/sendspin/sendspin_hub.h"
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
  void set_spectrum(uint8_t bins, uint16_t f_min, uint16_t f_max);
  void set_display_offset_ms(int32_t ms) { this->config_.display_offset_ms = ms; }
  void set_buffer_capacity(size_t bytes) { this->config_.support.buffer_capacity = bytes; }

  /// True between the server's visualizer stream start and its end.
  bool is_streaming() const { return this->streaming_; }
  /// True when the stream carries beats. Music Assistant adds them once its audio analysis has
  /// the track, and leaves them out for tracks it cannot analyse.
  bool has_beats() const { return this->has_beats_; }

  float loudness() const;
  /// Milliseconds since the last beat, or since the last onset while the stream has no beats.
  uint32_t ms_since_beat() const;
  bool last_beat_was_downbeat() const { return this->has_beats_ && this->downbeat_.load(std::memory_order_relaxed); }
  /// Milliseconds since the last onset (a transient, independent of the beat), and its strength.
  uint32_t ms_since_onset() const { return elapsed_ms_(this->onset_ms_.load(std::memory_order_relaxed)); }
  float onset_strength() const { return this->onset_strength_.load(std::memory_order_relaxed) / 255.0f; }
  /// Spectrum bands, low to high frequency, as the server sends them.
  uint8_t spectrum_size() const { return this->spectrum_size_; }
  float spectrum(uint8_t band) const;

  /// One LED per band (stretched or squeezed to the strip), red for the bass to violet for the
  /// treble, each band scaled to its own recent peak.
  void render_spectrum(light::AddressableLight &it);
  /// A level meter from the middle of the strip outwards, green to red, with a falling peak dot.
  void render_level(light::AddressableLight &it);
  /// The whole strip flashes on each beat and steps to the next colour; downbeats flash brighter.
  void render_beat(light::AddressableLight &it);

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
  /// The loudness scaled to its recent peak and smoothed, 0 to 1.
  float update_level_(float dt);
  void log_stats_();

  sendspin::VisualizerRoleConfig config_{};
  bool streaming_{false};
  bool has_beats_{false};
  uint8_t spectrum_size_{0};

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

  // Render state, main loop only.
  uint32_t last_render_ms_{0};
  std::array<float, MAX_SPECTRUM_BINS> band_peak_{};
  std::array<float, MAX_LEDS> led_level_{};
  float level_{0.0f};
  float level_ceiling_{0.0f};
  float level_dot_{0.0f};
  uint32_t level_dot_ms_{0};
  uint32_t last_beat_seen_{0};
  uint16_t beat_hue_{0};
  float beat_flash_{0.0f};
};

}  // namespace esphome::sendspin_visualizer

#endif  // USE_SENDSPIN_VISUALIZER
