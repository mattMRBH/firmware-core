/**
 * AirGradient Go -- Melody library (header-only)
 *
 * Musical types, 12-TET frequency table, inline helpers, MelodySelect
 * enum, and all predefined melodies and note patterns.  No service
 * dependencies -- only includes go_buzzer_types.h for Note.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "go_buzzer_types.h"

#include <cstddef>
#include <cstdint>

// ===========================================================================
// Musical types
// ===========================================================================

/// 12-TET equal-temperament pitches, C0..B8 (108 semitones).
/// Indices 0..107 map to C0..B8 for direct frequency table lookup.
/// REST (255) maps to silence (freq_hz = 0).
enum class MusicalNote : uint8_t {
  // clang-format off
  C0 = 0,  Cs0, D0, Ds0, E0, F0, Fs0, G0, Gs0, A0, As0, B0,
  C1 = 12, Cs1, D1, Ds1, E1, F1, Fs1, G1, Gs1, A1, As1, B1,
  C2 = 24, Cs2, D2, Ds2, E2, F2, Fs2, G2, Gs2, A2, As2, B2,
  C3 = 36, Cs3, D3, Ds3, E3, F3, Fs3, G3, Gs3, A3, As3, B3,
  C4 = 48, Cs4, D4, Ds4, E4, F4, Fs4, G4, Gs4, A4, As4, B4,
  C5 = 60, Cs5, D5, Ds5, E5, F5, Fs5, G5, Gs5, A5, As5, B5,
  C6 = 72, Cs6, D6, Ds6, E6, F6, Fs6, G6, Gs6, A6, As6, B6,
  C7 = 84, Cs7, D7, Ds7, E7, F7, Fs7, G7, Gs7, A7, As7, B7,
  C8 = 96, Cs8, D8, Ds8, E8, F8, Fs8, G8, Gs8, A8, As8, B8,
  REST = 255,
  // clang-format on
};

/// Note value in sixteenth-note units.
enum class NoteValue : uint8_t {
  SIXTEENTH = 1,
  EIGHTH = 2,
  DOTTED_EIGHTH = 3,
  QUARTER = 4,
  DOTTED_QUARTER = 6,
  HALF = 8,
  DOTTED_HALF = 12,
  WHOLE = 16,
};

struct MelodyNote {
  MusicalNote pitch;
  NoteValue value;
};

struct Melody {
  uint16_t bpm; ///< Beats per minute (quarter note = 1 beat)
  const MelodyNote *notes;
  size_t count;
};

enum class MelodySelect : uint8_t {
  Off = 0,
  Chime = 1,
  Tetris = 2,
};

inline constexpr uint8_t MELODY_SELECT_COUNT = 3;

/// Maximum melody length. Sized for Tetris (~38 notes) with headroom.
inline constexpr size_t MELODY_MAX_NOTES = 48;

// ===========================================================================
// 12-TET frequency table
// ===========================================================================

/// 108-entry lookup table (C0..B8), A4 = 440 Hz, rounded to nearest Hz.
// clang-format off
inline constexpr uint16_t NOTE_FREQ_HZ[108] = {
    /* C0..B0 */    16,    17,    18,    19,    21,    22,    23,    25,    26,    28,    29,    31,
    /* C1..B1 */    33,    35,    37,    39,    41,    44,    46,    49,    52,    55,    58,    62,
    /* C2..B2 */    65,    69,    73,    78,    82,    87,    92,    98,   104,   110,   117,   123,
    /* C3..B3 */   131,   139,   147,   156,   165,   175,   185,   196,   208,   220,   233,   247,
    /* C4..B4 */   262,   277,   294,   311,   330,   349,   370,   392,   415,   440,   466,   494,
    /* C5..B5 */   523,   554,   587,   622,   659,   698,   740,   784,   831,   880,   932,   988,
    /* C6..B6 */  1047,  1109,  1175,  1245,  1319,  1397,  1480,  1568,  1661,  1760,  1865,  1976,
    /* C7..B7 */  2093,  2217,  2349,  2489,  2637,  2794,  2960,  3136,  3322,  3520,  3729,  3951,
    /* C8..B8 */  4186,  4435,  4699,  4978,  5274,  5588,  5920,  6272,  6645,  7040,  7459,  7902,
};
// clang-format on

// ===========================================================================
// Helper functions
// ===========================================================================

/// Frequency in Hz for a MusicalNote. Returns 0 for REST or out-of-range.
inline constexpr uint32_t note_freq_hz(MusicalNote note) {
  const auto idx = static_cast<uint8_t>(note);
  return (idx < 108) ? NOTE_FREQ_HZ[idx] : 0;
}

/// Duration of one sixteenth note at the given BPM.
inline constexpr uint32_t sixteenth_ms(uint16_t bpm) { return (bpm == 0) ? 0u : (15000u / bpm); }

/// Duration of a NoteValue at the given BPM.
inline constexpr uint32_t note_duration_ms(NoteValue value, uint16_t bpm) {
  return sixteenth_ms(bpm) * static_cast<uint32_t>(value);
}

/// Total wall-clock duration of a melody in milliseconds.
inline constexpr uint32_t melody_total_duration_ms(const Melody &melody) {
  uint32_t total = 0;
  for (size_t i = 0; i < melody.count; ++i) {
    total += note_duration_ms(melody.notes[i].value, melody.bpm);
  }
  return total;
}

// ===========================================================================
// Predefined melodies
// ===========================================================================

/// Chime -- short rising arpeggio, ~0.6 s at 200 BPM.
/// Pitched near the HYG-8503A resonance (C6-E6-G6).
inline constexpr MelodyNote CHIME_NOTES[] = {
    {MusicalNote::C6, NoteValue::EIGHTH},
    {MusicalNote::E6, NoteValue::EIGHTH},
    {MusicalNote::G6, NoteValue::QUARTER},
};

inline constexpr Melody MELODY_CHIME = {200, CHIME_NOTES, 3};

/// Tetris (Korobeiniki) -- first verse, 38 notes at 144 BPM.
/// Transposed one octave up (E6..A6) to sit closer to buzzer resonance.
// clang-format off
inline constexpr MelodyNote TETRIS_NOTES[] = {
    {MusicalNote::E6,  NoteValue::QUARTER},
    {MusicalNote::B5,  NoteValue::EIGHTH},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::D6,  NoteValue::QUARTER},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::B5,  NoteValue::EIGHTH},
    {MusicalNote::A5,  NoteValue::QUARTER},
    {MusicalNote::A5,  NoteValue::EIGHTH},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::E6,  NoteValue::QUARTER},
    {MusicalNote::D6,  NoteValue::EIGHTH},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::B5,  NoteValue::DOTTED_QUARTER},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::D6,  NoteValue::QUARTER},
    {MusicalNote::E6,  NoteValue::QUARTER},
    {MusicalNote::C6,  NoteValue::QUARTER},
    {MusicalNote::A5,  NoteValue::QUARTER},
    {MusicalNote::A5,  NoteValue::QUARTER},
    {MusicalNote::REST, NoteValue::QUARTER},
    {MusicalNote::D6,  NoteValue::DOTTED_QUARTER},
    {MusicalNote::F6,  NoteValue::EIGHTH},
    {MusicalNote::A6,  NoteValue::QUARTER},
    {MusicalNote::G6,  NoteValue::EIGHTH},
    {MusicalNote::F6,  NoteValue::EIGHTH},
    {MusicalNote::E6,  NoteValue::DOTTED_QUARTER},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::E6,  NoteValue::QUARTER},
    {MusicalNote::D6,  NoteValue::EIGHTH},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::B5,  NoteValue::QUARTER},
    {MusicalNote::B5,  NoteValue::EIGHTH},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::D6,  NoteValue::QUARTER},
    {MusicalNote::E6,  NoteValue::QUARTER},
    {MusicalNote::C6,  NoteValue::QUARTER},
    {MusicalNote::A5,  NoteValue::QUARTER},
    {MusicalNote::A5,  NoteValue::QUARTER},
};
// clang-format on

inline constexpr Melody MELODY_TETRIS = {144, TETRIS_NOTES, 38};

// ===========================================================================
// Predefined note patterns (raw Note arrays for system events)
// ===========================================================================

/// Boot: three ascending tones (~340 ms total)
inline constexpr Note PATTERN_BOOT[] = {
    {2400, 80}, {0, 30}, {2700, 80}, {0, 30}, {3200, 120},
};
inline constexpr uint8_t PATTERN_BOOT_COUNT = 5;

/// Charge complete: three ascending tones (~450 ms total)
inline constexpr Note PATTERN_CHARGE_DONE[] = {
    {1500, 100}, {0, 50}, {2000, 100}, {0, 50}, {2500, 150},
};
inline constexpr uint8_t PATTERN_CHARGE_DONE_COUNT = 5;

/// Unplug alert: 3x short beeps + 1x higher long beep (~690 ms total)
inline constexpr Note PATTERN_UNPLUG[] = {
    {2700, 100}, {0, 80}, {2700, 100}, {0, 80}, {2700, 100}, {0, 80}, {3500, 250},
};
inline constexpr uint8_t PATTERN_UNPLUG_COUNT = 7;
