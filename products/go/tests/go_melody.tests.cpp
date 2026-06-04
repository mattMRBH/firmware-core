/**
 * AirGradient Go -- Melody library unit tests
 *
 * Pure-function tests for the header-only melody library. No service
 * stubs needed — only includes go_melody.h.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include "go_melody.h"

// ============================================================================
// note_freq_hz
// ============================================================================

TEST_CASE("note_freq_hz: well-known pitches", "[melody][freq]") {
  CHECK(note_freq_hz(MusicalNote::A4) == 440);
  CHECK(note_freq_hz(MusicalNote::C4) == 262);
  CHECK(note_freq_hz(MusicalNote::C0) == 16);
  CHECK(note_freq_hz(MusicalNote::B8) == 7902);
}

TEST_CASE("note_freq_hz: REST returns 0", "[melody][freq]") {
  CHECK(note_freq_hz(MusicalNote::REST) == 0);
}

TEST_CASE("note_freq_hz: octave doubling within rounding tolerance", "[melody][freq]") {
  // Each octave should roughly double the frequency.
  // Allow rounding tolerance of +/-1 Hz.
  for (int semitone = 0; semitone < 12; ++semitone) {
    for (int octave = 0; octave < 8; ++octave) {
      auto low = static_cast<MusicalNote>(octave * 12 + semitone);
      auto high = static_cast<MusicalNote>((octave + 1) * 12 + semitone);
      uint32_t f_low = note_freq_hz(low);
      uint32_t f_high = note_freq_hz(high);
      // f_high should be approximately 2 * f_low
      int32_t diff = static_cast<int32_t>(f_high) - static_cast<int32_t>(2 * f_low);
      CHECK(diff >= -1);
      CHECK(diff <= 1);
    }
  }
}

// ============================================================================
// sixteenth_ms
// ============================================================================

TEST_CASE("sixteenth_ms: standard BPM values", "[melody][timing]") {
  CHECK(sixteenth_ms(60) == 250);  // 15000/60
  CHECK(sixteenth_ms(120) == 125); // 15000/120
  CHECK(sixteenth_ms(144) == 104); // 15000/144 = 104.16 -> 104
  CHECK(sixteenth_ms(0) == 0);     // Guard against div-by-zero
}

// ============================================================================
// note_duration_ms
// ============================================================================

TEST_CASE("note_duration_ms: linear scaling for all NoteValue variants", "[melody][timing]") {
  constexpr uint16_t BPM = 120;
  constexpr uint32_t S16 = sixteenth_ms(BPM); // 125 ms

  CHECK(note_duration_ms(NoteValue::SIXTEENTH, BPM) == S16 * 1);
  CHECK(note_duration_ms(NoteValue::EIGHTH, BPM) == S16 * 2);
  CHECK(note_duration_ms(NoteValue::DOTTED_EIGHTH, BPM) == S16 * 3);
  CHECK(note_duration_ms(NoteValue::QUARTER, BPM) == S16 * 4);
  CHECK(note_duration_ms(NoteValue::DOTTED_QUARTER, BPM) == S16 * 6);
  CHECK(note_duration_ms(NoteValue::HALF, BPM) == S16 * 8);
  CHECK(note_duration_ms(NoteValue::DOTTED_HALF, BPM) == S16 * 12);
  CHECK(note_duration_ms(NoteValue::WHOLE, BPM) == S16 * 16);
}

// ============================================================================
// melody_total_duration_ms
// ============================================================================

TEST_CASE("melody_total_duration_ms: Chime under 1 second", "[melody][duration]") {
  uint32_t duration = melody_total_duration_ms(MELODY_CHIME);
  CHECK(duration > 0);
  CHECK(duration < 1000);
}

TEST_CASE("melody_total_duration_ms: Tetris between 4 and 15 seconds", "[melody][duration]") {
  uint32_t duration = melody_total_duration_ms(MELODY_TETRIS);
  CHECK(duration >= 4000);
  CHECK(duration <= 15000);
}

TEST_CASE("melody_total_duration_ms: matches manual calculation for Chime", "[melody][duration]") {
  // Chime: 200 BPM, EIGHTH + EIGHTH + QUARTER = 2 + 2 + 4 = 8 sixteenths
  // sixteenth_ms(200) = 15000/200 = 75
  // Total = 8 * 75 = 600 ms
  CHECK(melody_total_duration_ms(MELODY_CHIME) == 600);
}

// ============================================================================
// Predefined melodies validation
// ============================================================================

TEST_CASE("Predefined melodies: non-empty and fit in MELODY_MAX_NOTES", "[melody][data]") {
  CHECK(MELODY_CHIME.count > 0);
  CHECK(MELODY_CHIME.count <= MELODY_MAX_NOTES);

  CHECK(MELODY_TETRIS.count > 0);
  CHECK(MELODY_TETRIS.count <= MELODY_MAX_NOTES);
}

TEST_CASE("Predefined melodies: all pitches within audible buzzer range", "[melody][data]") {
  // All non-REST pitches should be between 100-6000 Hz (audible on magnetic buzzer)
  auto check_melody = [](const Melody &m) {
    for (size_t i = 0; i < m.count; ++i) {
      if (m.notes[i].pitch == MusicalNote::REST) {
        continue;
      }
      uint32_t freq = note_freq_hz(m.notes[i].pitch);
      CHECK(freq >= 100);
      CHECK(freq <= 6000);
    }
  };

  check_melody(MELODY_CHIME);
  check_melody(MELODY_TETRIS);
}

// ============================================================================
// Predefined note patterns validation
// ============================================================================

TEST_CASE("Predefined note patterns: non-empty and fit in MAX_PATTERN_NOTES", "[melody][data]") {
  // BuzzerService::MAX_PATTERN_NOTES = 8 (from go_buzzer.h).
  // Duplicated here to keep melody tests free of service dependencies.
  constexpr uint8_t MAX_PATTERN_NOTES = 8;

  CHECK(PATTERN_BOOT_COUNT > 0);
  CHECK(PATTERN_BOOT_COUNT <= MAX_PATTERN_NOTES);

  CHECK(PATTERN_CHARGE_DONE_COUNT > 0);
  CHECK(PATTERN_CHARGE_DONE_COUNT <= MAX_PATTERN_NOTES);

  CHECK(PATTERN_UNPLUG_COUNT > 0);
  CHECK(PATTERN_UNPLUG_COUNT <= MAX_PATTERN_NOTES);
}

// ============================================================================
// MelodySelect count
// ============================================================================

TEST_CASE("MELODY_SELECT_COUNT matches enum values", "[melody][data]") {
  CHECK(MELODY_SELECT_COUNT == 3);
  CHECK(static_cast<uint8_t>(MelodySelect::Off) == 0);
  CHECK(static_cast<uint8_t>(MelodySelect::Chime) == 1);
  CHECK(static_cast<uint8_t>(MelodySelect::Tetris) == 2);
}
