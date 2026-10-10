#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#define AUDIO_BANDS     16   // log-spaced 40 Hz - 10 kHz, for scripts
#define AUDIO_SPECTRUM  32   // log-spaced, for the web visualisation

/*
 * Sound analysis from an INMP441 I2S microphone (pins in the config): 22.05 kHz, 1024-point FFT
 * every 512 samples (~43/s). Every value is auto-gained against its own recent peak, so they span
 * 0..1 whatever the room volume; below the noise gate they fall to 0. Onsets are jumps in the
 * spectrum; the tempo is the autocorrelation of the onset curve, and beats are a steady grid
 * whose phase follows the onsets.
 *
 * The demo track (kick, hi-hat, pad at the config's fallback tempo) replaces the microphone when
 * asked to, and by itself whenever the microphone hears nothing (or there is none): the fallback.
 * With sound but no beat, the beat grid runs at that same tempo.
 */
typedef struct {
    bool     mic;                    // microphone configured and its driver running
    bool     demo;                   // the demo track is playing (asked for, or as the fallback)
    bool     demo_forced;            // ... because the Demo button asked for it
    bool     signal;                 // input above the noise gate
    float    db;                     // input level, dBFS
    float    level, bass, mid, high; // 0..1
    float    bands[AUDIO_BANDS];     // 0..1
    uint8_t  spectrum[AUDIO_SPECTRUM];   // 0..255
    uint32_t beats;                  // beats detected so far
    float    bpm;                    // the beat grid's tempo (music or fallback), 0 = none
    bool     locked;                 // the grid follows the music (else: the fallback tempo)
    float    phase;                  // 0..1 through the beat, at t_us
    float    period_s;               // beat period (0 = no tempo)
    int64_t  t_us;                   // time of the last analysis
    int64_t  last_beat_us;
} audio_state_t;

esp_err_t audio_start(void);         // after config_init(); runs without a microphone too (demo)
void      audio_get(audio_state_t *out);
float     audio_phase_at(const audio_state_t *a, int64_t t_us);   // phase extrapolated to t_us
void      audio_set_demo(bool on);   // force the demo track (also while there is music)
cJSON    *audio_json(void);          // GET /api/audio
