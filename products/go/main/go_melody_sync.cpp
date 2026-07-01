/**
 * AirGradient Go -- Synced melody playback implementation
 *
 * Blocking per-note loop that coordinates buzzer audio and back-LED
 * color visualization via the semitone color palette.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_melody_sync.h"

#include "go_buzzer.h"
#include "go_led.h"
#include "rtos.h"

// ===========================================================================
// Semitone color palette (12 entries indexed by semitone % 12)
// ===========================================================================

static constexpr Rgb SEMITONE_COLORS[12] = {
    {255, 0, 0},   // C   - red
    {255, 80, 0},  // C#  - red-orange
    {255, 160, 0}, // D   - orange
    {255, 220, 0}, // D#  - amber
    {255, 255, 0}, // E   - yellow
    {160, 255, 0}, // F   - yellow-green
    {0, 255, 0},   // F#  - green
    {0, 255, 160}, // G   - cyan-green
    {0, 255, 255}, // G#  - cyan
    {0, 80, 255},  // A   - blue
    {120, 0, 255}, // A#  - purple
    {255, 0, 200}, // B   - magenta
};

// ===========================================================================
// Melody resolution
// ===========================================================================

static const Melody *resolve_melody(MelodySelect select) {
  switch (select) {
  case MelodySelect::Chime:
    return &MELODY_CHIME;
  case MelodySelect::Tetris:
    return &MELODY_TETRIS;
  case MelodySelect::Off:
  default:
    return nullptr;
  }
}

// ===========================================================================
// play_synced
// ===========================================================================

uint32_t play_synced(BuzzerService &buzzer, LedService &led, MelodySelect melody) {
  const Melody *m = resolve_melody(melody);
  if (m == nullptr || !buzzer.enabled()) {
    return 0;
  }

  // Cancel any in-progress non-synced pattern
  buzzer.stop();

  // Force full brightness for the melody visualization
  led.back_set_brightness(LedBrightness::Bright);

  for (size_t i = 0; i < m->count; ++i) {
    const MelodyNote &mn = m->notes[i];
    uint32_t freq = note_freq_hz(mn.pitch);
    uint32_t duration = note_duration_ms(mn.value, m->bpm);

    // Map pitch to semitone color
    if (mn.pitch != MusicalNote::REST) {
      uint8_t semitone = static_cast<uint8_t>(mn.pitch) % 12;
      led.back_solid(SEMITONE_COLORS[semitone]);
    } else {
      led.back_off();
    }

    // Enqueue one note
    Note note{freq, duration};
    buzzer.play(&note, 1);

    // Block the caller for the note duration
    RTOS::delay_ms(duration);
  }

  // Cleanup
  led.back_off();

  return melody_total_duration_ms(*m);
}
