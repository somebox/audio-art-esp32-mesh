"""Music data from Music Assistant for LED effects, over Sendspin's visualizer role.

Music Assistant analyses the audio it streams and sends loudness, a spectrum,
onsets and (once its audio analysis has the track) beats, each stamped with its
playback time. ESPHome 2026.10.2 can ask for the visualizer role but never adds
it to the Sendspin client; esphome/esphome#19276 does. Until that is released,
this component adds the role itself, keeps the latest values, and draws effects
on an addressable light.

    sendspin_visualizer:
      id: music
      spectrum_bins: 4       # one band per column
      spectrum_scale: log
      matrix:                # leave out for a strip
        width: 4
        height: 4
        first_pixel: top_right

    light:
      - platform: esp32_rmt_led_strip
        effects:
          - addressable_lambda:
              name: Music Spectrum
              lambda: id(music).render_spectrum(it);
"""

import esphome.codegen as cg
from esphome.components.sendspin import (
    CONF_SENDSPIN_ID,
    SendspinHub,
    request_visualizer_support,
)
import esphome.config_validation as cv
from esphome.const import CONF_HEIGHT, CONF_ID, CONF_WIDTH
from esphome.types import ConfigType

DEPENDENCIES = ["sendspin", "light"]

CONF_BUFFER_CAPACITY = "buffer_capacity"
CONF_DISPLAY_OFFSET = "display_offset"
CONF_F_MAX = "f_max"
CONF_F_MIN = "f_min"
CONF_FIRST_PIXEL = "first_pixel"
CONF_MATRIX = "matrix"
CONF_ORDER = "order"
CONF_RATE_MAX = "rate_max"
CONF_SERPENTINE = "serpentine"
CONF_SPECTRUM_BINS = "spectrum_bins"
CONF_SPECTRUM_SCALE = "spectrum_scale"

# Matches MAX_SPECTRUM_BINS in sendspin_visualizer.h.
MAX_SPECTRUM_BINS = 64

# Matches MAX_LEDS in sendspin_visualizer.h.
MAX_LEDS = 256

VisualizerSpectrumScale = cg.global_ns.namespace("sendspin").enum(
    "VisualizerSpectrumScale", is_class=True
)
SPECTRUM_SCALES = {
    "mel": VisualizerSpectrumScale.MEL,
    "log": VisualizerSpectrumScale.LOG,
    "lin": VisualizerSpectrumScale.LIN,
}

# Corner of the first LED: (counted from the right, counted from the bottom).
FIRST_PIXELS = {
    "top_left": (False, False),
    "top_right": (True, False),
    "bottom_left": (False, True),
    "bottom_right": (True, True),
}

sendspin_visualizer_ns = cg.esphome_ns.namespace("sendspin_visualizer")
SendspinVisualizer = sendspin_visualizer_ns.class_(
    "SendspinVisualizer", cg.Component
)


def _request_role(config: ConfigType) -> ConfigType:
    request_visualizer_support()
    return config


def _check_matrix_size(config: ConfigType) -> ConfigType:
    if config[CONF_WIDTH] * config[CONF_HEIGHT] > MAX_LEDS:
        raise cv.Invalid(f"A matrix has at most {MAX_LEDS} LEDs")
    return config


MATRIX_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_WIDTH): cv.int_range(min=1, max=16),
            cv.Required(CONF_HEIGHT): cv.int_range(min=1, max=16),
            # The corner the first LED (the one the data wire goes to) is in.
            cv.Optional(CONF_FIRST_PIXEL, default="top_left"): cv.one_of(
                *FIRST_PIXELS, lower=True
            ),
            # rows: the second LED is beside the first; columns: above or below it.
            cv.Optional(CONF_ORDER, default="rows"): cv.one_of(
                "rows", "columns", lower=True
            ),
            # Each row (or column) runs the other way from the one before.
            cv.Optional(CONF_SERPENTINE, default=False): cv.boolean,
        }
    ),
    _check_matrix_size,
)


def _check_range(config: ConfigType) -> ConfigType:
    if config[CONF_F_MIN] >= config[CONF_F_MAX]:
        raise cv.Invalid(f"{CONF_F_MIN} must be below {CONF_F_MAX}")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SendspinVisualizer),
            cv.GenerateID(CONF_SENDSPIN_ID): cv.use_id(SendspinHub),
            # Frames per second for loudness and spectrum. Beats and onsets
            # come as they happen.
            cv.Optional(CONF_RATE_MAX, default=30): cv.int_range(min=1, max=120),
            # Spectrum bands, low to high; 0 asks for no spectrum.
            cv.Optional(CONF_SPECTRUM_BINS, default=16): cv.int_range(
                min=0, max=MAX_SPECTRUM_BINS
            ),
            # How the bands divide the range. log gives a few bands an even
            # spread of bass, mids and treble; mel crowds them into the treble.
            cv.Optional(CONF_SPECTRUM_SCALE, default="mel"): cv.enum(
                SPECTRUM_SCALES, lower=True
            ),
            cv.Optional(CONF_F_MIN, default="40Hz"): cv.All(
                cv.frequency, cv.float_range(min=10, max=20000)
            ),
            cv.Optional(CONF_F_MAX, default="16kHz"): cv.All(
                cv.frequency, cv.float_range(min=10, max=20000)
            ),
            # Hands each value over this much before its playback time, to
            # cover the main loop's wait and the strip's write.
            cv.Optional(
                CONF_DISPLAY_OFFSET, default="10ms"
            ): cv.positive_time_period_milliseconds,
            # Memory for frames that arrive ahead of playback (PSRAM, in the
            # stream's inbound buffer). Music Assistant sends up to about 3 s
            # ahead while it is sending beats.
            cv.Optional(CONF_BUFFER_CAPACITY, default=65536): cv.int_range(
                min=4096, max=1048576
            ),
            # The LEDs are a grid, not a line; the effects then draw in 2-D.
            cv.Optional(CONF_MATRIX): MATRIX_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _check_range,
    _request_role,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_SENDSPIN_ID])

    cg.add(var.set_rate_max(config[CONF_RATE_MAX]))
    if config[CONF_SPECTRUM_BINS]:
        cg.add(
            var.set_spectrum(
                config[CONF_SPECTRUM_BINS],
                int(config[CONF_F_MIN]),
                int(config[CONF_F_MAX]),
                config[CONF_SPECTRUM_SCALE],
            )
        )
    cg.add(var.set_display_offset_ms(config[CONF_DISPLAY_OFFSET].total_milliseconds))
    cg.add(var.set_buffer_capacity(config[CONF_BUFFER_CAPACITY]))
    if (matrix := config.get(CONF_MATRIX)) is not None:
        flip_x, flip_y = FIRST_PIXELS[matrix[CONF_FIRST_PIXEL]]
        cg.add(
            var.set_matrix(
                matrix[CONF_WIDTH],
                matrix[CONF_HEIGHT],
                flip_x,
                flip_y,
                matrix[CONF_ORDER] == "columns",
                matrix[CONF_SERPENTINE],
            )
        )
