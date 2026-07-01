/**
 * AirGradient Go -- Synced melody playback
 *
 * Blocking free function that coordinates buzzer audio and back-LED
 * visuals per note.  Depends on BuzzerService, LedService, go_melody.h,
 * and RTOS.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "go_melody.h"

#include <cstdint>

class BuzzerService;
class LedService;

/// Blocks the caller for the melody duration. Forces back LED
/// brightness to Bright, then drives back LEDs per-note using the
/// semitone color palette. Fires notes one-at-a-time through
/// buzzer.play(). On completion, calls back_off(). Returns total
/// duration in ms (0 if Off or buzzer disabled).
///
/// The caller (orchestrator) is responsible for restoring both
/// back_set_brightness() and back-LED state (e.g., back_update_aqi())
/// after this function returns.
uint32_t play_synced(BuzzerService &buzzer, LedService &led, MelodySelect melody);
