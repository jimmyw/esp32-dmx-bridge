#include "audio.h"

#include <math.h>
#include <string.h>
#include "config.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define RATE        22050
#define N           1024                 // FFT size
#define HOP         512                  // new samples per analysis
#define BIN_HZ      ((float)RATE / N)
#define HOP_US      (1000000LL * HOP / RATE)
#define RELEASE     0.75f                // per analysis: outputs fall to 75% unless refreshed
#define AGC_ATTACK  0.3f                 // per analysis: the peak moves 30% towards a louder value
#define AGC_DECAY   0.995f               // per analysis: a peak halves in ~3 s
#define SPEC_RANGE  45.0f                // visualisation spectrum: dB below its peak shown
#define FRAME_HZ    ((float)RATE / HOP)  // analyses per second (~43)
#define ONSET_BINS  12                   // onsets: bins up to ~260 Hz (kick, bass) at full weight,
#define ONSET_HI    186                  //   up to ~4 kHz (snare, claps) at ONSET_HI_W each
#define ONSET_HI_W  0.06f
#ifndef COMB_POW
#define COMB_POW    2                    // the beats' place: hits weighted by their square
#endif
#define UPPER_TOP   372                  // the upper curve (snare, claps, hats, notes): ~260 Hz - 8 kHz
#ifndef UPPER_CONF
#define UPPER_CONF  0.55f                // ... gives a tempo without a kick when this regular
#endif
#define SMOOTH_PASSES 1                  // 1-2-1 smoothing of the onset curve before the tempo
#define SWITCH_MARGIN 1.25f              // a rival tempo must fit this much better than the current
#define CAND_N 20                        // tempo updates (~5/s) a different tempo must hold to win
#ifndef FOLLOW_WIN
#define FOLLOW_WIN  0.06f                // estimates this close to the tempo pull it along...
#endif
#ifndef LOCAL_MIN
#define LOCAL_MIN   0.5f                 // our own peak keeps being followed while this strong
#endif
#ifndef FOLLOW_RATE
#define FOLLOW_RATE 0.8f                 // ... this much per update (~5/s)
#endif
#define KICK_LO     2                    // the kick: bins ~43-150 Hz
#define KICK_HI     7
#ifndef KICK_SHARE
#define KICK_SHARE  0.5f                 // ... and has this share of the frame's rise in energy
#endif
#ifndef KICK_FIT
#define KICK_FIT    0.4f                 // the kicks' self-match at the tempo (or every other beat)
#endif
#define KICK_HOLD_US     4000000         // keep a tempo this long without a steady kick
#ifndef KICK_WIN_US
#define KICK_WIN_US      3000000         // a steady kick: 4 in this long
#endif
#define SILENCE_DROP_US  5000000         // ... but drop it after this much silence
#define FLUX_REL    2.0f                 // an onset also needs flux > FLUX_REL * the mean
#define PRIOR_OCT   0.9f                 // tempo prior: spread around 120 BPM, in octaves
#define PHASE_STEP  0.04f                // largest phase correction per tempo update (~5/s)
#define ONSET_MIN_US 200000              // onsets at least 200 ms apart
#define ONSET_N     256                  // onset curve kept for the tempo: ~6 s
#define ONSET_MIN   2.0f                 // flux an onset needs at least (sum of log rises)
#define BPM_MIN     70
#define BPM_MAX     180
#define FOLLOW_MIN  60                   // a tempo is followed a bit past the range, then folds
#define FOLLOW_MAX  200

typedef struct { int lo, hi; } range_t;  // FFT bins [lo, hi)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static audio_state_t s_state;            // published (guarded by s_mux)
static i2s_chan_handle_t s_rx;

// Analysis (audio task only)
static float *s_ring;                    // last N samples
static float *s_re, *s_im, *s_win, *s_cos, *s_sin, *s_mag;
static range_t s_band_bins[AUDIO_BANDS], s_spec_bins[AUDIO_SPECTRUM];
static range_t s_bass_bins, s_mid_bins, s_high_bins;
static float s_peak[4 + AUDIO_BANDS];    // AGC peaks: level, bass, mid, high, bands...
static float s_spec_peak;
static float s_prev_log[UPPER_TOP + 1];
static float s_prev_kick[KICK_HI + 1];
static int64_t s_onset_times[4];         // the last 4 onsets: a tempo needs them recent
static float s_flux_mean, s_flux_var, s_flux_prev;
static int64_t s_last_onset_us, s_signal_seen_us;
static float s_onsets[ONSET_N];          // onset curve (ring, newest at s_onset_pos - 1): tempo
static float s_onsets_low[ONSET_N];      // the same from the low end only: where the beats fall
static float s_onsets_up[ONSET_N];       // the same from above the bass: snares, hats, notes
static bool  s_upper;                    // the tempo comes from the upper curve (no kick)
static int   s_onset_pos, s_tempo_tick;
static float s_kicks[ONSET_N];           // the same from the kick range, frames that are mostly bass
static int64_t s_kick_times[4];          // the last 4 kicks
static int64_t s_last_kick_us, s_kick_seen_us;
static float s_kick_mean, s_kick_prev, s_prev_e_kick, s_prev_e_all;
static float s_cand;                     // tempo candidate that differs from the current one
static int   s_cand_n;
static int64_t s_last_mic_activity_us;
static bool    s_gate_open;              // the last analysis was above the gate
static float   s_open_ratio;             // share of recent analyses above the gate (~1.5 s)
#define OPEN_MIN    0.9f                 // a tempo needs sound this steadily: not noise at the gate
#define GATE_HYST   0.708f               // the gate closes 3 dB below where it opens
static volatile audio_tap_t s_tap;
static double  s_clock_us;               // when the newest sample of a block was heard, from the sample count
static int64_t s_diag_start_us;          // diagnostics, per DIAG_US window: the longest analysis
static int32_t s_proc_max_us, s_late_max_us, s_proc_last_us, s_late_last_us;   //   and the task's lag
static int64_t s_proc_sum_us;
static int32_t s_proc_n, s_proc_avg_us;
#define DIAG_US 10000000
static int s_channel = -1;               // the slot the mic talks on: 0 left, 1 right, -1 not yet
static int32_t s_raw_min[2], s_raw_max[2];   // last block, per slot (diagnostics)

static void *alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

static int bin_of(float hz)
{
    int b = (int)lroundf(hz / BIN_HZ);
    return b < 1 ? 1 : b > N / 2 ? N / 2 : b;
}

static range_t bins(float lo_hz, float hi_hz)
{
    range_t r = { bin_of(lo_hz), bin_of(hi_hz) };
    if (r.hi <= r.lo) r.hi = r.lo + 1;
    return r;
}

// n log-spaced ranges 40 Hz - 10 kHz, each at least one bin wide
static void log_bins(range_t *out, int n)
{
    int prev = bin_of(40);
    for (int i = 0; i < n; i++) {
        int hi = bin_of(40 * powf(10000.0f / 40, (float)(i + 1) / n));
        if (hi <= prev) hi = prev + 1;
        out[i] = (range_t){ prev, hi };
        prev = hi;
    }
}

// The hot path (each block, each tempo update) runs from IRAM: no stalls on flash cache misses.
static IRAM_ATTR void fft(float *re, float *im)
{
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        int half = len / 2, step = N / len;
        for (int i = 0; i < N; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = s_cos[k * step], wi = -s_sin[k * step];
                float *ar = &re[i + k], *ai = &im[i + k], *br = &re[i + k + half], *bi = &im[i + k + half];
                float xr = *br * wr - *bi * wi, xi = *br * wi + *bi * wr;
                *br = *ar - xr; *bi = *ai - xi;
                *ar += xr; *ai += xi;
            }
        }
    }
}

static IRAM_ATTR float rms_bins(range_t r)
{
    float sum = 0;
    for (int b = r.lo; b < r.hi; b++) sum += s_mag[b] * s_mag[b];
    return sqrtf(sum / (r.hi - r.lo));
}

// Auto gain: v against its own peak, 0..1. The peak follows louder values partway (one loud
// click or tap doesn't flatten everything after it), decays slowly and never drops below floor.
static float agc(int i, float v, float floor)
{
    float p = s_peak[i] * AGC_DECAY;
    if (v > p) p += (v - p) * AGC_ATTACK;
    s_peak[i] = fmaxf(p, floor);
    float n = v / s_peak[i];
    return n > 1 ? 1 : n;
}

// Output smoothing: rises at once, falls by RELEASE per analysis.
static float release(float prev, float v)
{
    return fmaxf(v, prev * RELEASE);
}

/* ---------- tempo ---------- */

static float s_music_bpm;                // tempo found in the music, 0 = none
static float s_last_est;                 // the last tempo estimate (overall best peak)

// The grid's tempo: the music's when there is one; else none, and the beats are
// the onsets as they come.
static void set_tempo(audio_state_t *a)
{
    // A tempo followed past the range (a build-up speeding up) folds by an octave, the grid
    // staying on the beats: past BPM_MAX every other beat is kept, below BPM_MIN each is split.
    if (s_music_bpm > BPM_MAX) {
        s_music_bpm /= 2;
        a->phase = (a->phase + (a->beats & 1)) / 2;
    } else if (s_music_bpm > 0 && s_music_bpm < BPM_MIN) {
        s_music_bpm *= 2;
        a->phase *= 2;
        if (a->phase >= 1) { a->phase -= 1; a->beats++; }
    }
    a->locked = s_music_bpm > 0;
    a->bpm = s_music_bpm;
    a->period_s = a->bpm > 0 ? 60 / a->bpm : 0;
}

static void grid_advance(audio_state_t *a, float dt, int64_t now)
{
    float p = a->phase + dt / a->period_s;
    if (p >= 1) {
        a->beats++;
        a->last_beat_us = now;
    }
    a->phase = p - floorf(p);
}

// No microphone: everything at rest.
static void idle_tick(int64_t now)
{
    portENTER_CRITICAL(&s_mux);
    audio_state_t a = s_state;
    portEXIT_CRITICAL(&s_mux);
    float dt = a.t_us ? (now - a.t_us) / 1e6f : 0;
    a.t_us = now;
    a.signal = false;
    s_music_bpm = 0;
    set_tempo(&a);
    a.level = a.bass = a.mid = a.high = 0;
    memset(a.bands, 0, sizeof(a.bands));
    memset(a.spectrum, 0, sizeof(a.spectrum));
    if (a.period_s > 0) grid_advance(&a, dt, now);
    portENTER_CRITICAL(&s_mux);
    s_state = a;
    portEXIT_CRITICAL(&s_mux);
}

// Autocorrelation of the onset curve over BPM_MIN..BPM_MAX, weighted by a log-normal prior
// around 120 BPM. Returns BPM (parabolic interpolation between lags) and *conf = r(lag) / r(0).
static float s_r[64], s_r_lo, s_r_hi;    // the last autocorrelation (for tempo_score)

// Prior-weighted autocorrelation at a given tempo (interpolated between lags), from the last
// estimate_tempo(): how well that tempo fits the recent onsets.
static float tempo_score(float bpm)
{
    float lag = FRAME_HZ * 60 / bpm;
    if (lag < s_r_lo || lag > s_r_hi) return 0;
    int i = (int)lag;
    float f = lag - i, v = s_r[i] * (1 - f) + s_r[i + 1] * f;
    float oct = log2f(bpm / 120);
    return v * expf(-0.5f * oct * oct / (PRIOR_OCT * PRIOR_OCT));
}

// The autocorrelation peak nearest a tempo (within FOLLOW_WIN), interpolated: the tempo the
// music has moved to if it changes gradually. Returns 0 if there's no peak there; *height gets
// its (unweighted) height. Uses the last estimate_tempo().
static float local_peak(float bpm, float *height)
{
    float centre = FRAME_HZ * 60 / bpm;
    int lo = (int)floorf(centre * (1 - FOLLOW_WIN)), hi = (int)ceilf(centre * (1 + FOLLOW_WIN));
    if (lo < s_r_lo) lo = s_r_lo;
    if (hi > s_r_hi) hi = s_r_hi;
    float best_at = 0, best_h = 0;
    for (int lag = lo; lag <= hi; lag++) {
        float y0 = s_r[lag - 1], y1 = s_r[lag], y2 = s_r[lag + 1];
        if (y1 < y0 || y1 < y2) continue;
        float den = y0 - 2 * y1 + y2;
        float at = lag + (den < 0 ? 0.5f * (y0 - y2) / den : 0);
        float h = den < 0 ? y1 - 0.125f * (y0 - y2) * (y0 - y2) / den : y1;
        if (best_at == 0 || fabsf(at - centre) < fabsf(best_at - centre)) { best_at = at; best_h = h; }
    }
    *height = best_h;
    return best_at > 0 ? FRAME_HZ * 60 / best_at : 0;
}

static IRAM_ATTR float estimate_tempo(const float *curve, float *conf)
{
    float o[ONSET_N], mean = 0;
    for (int i = 0; i < ONSET_N; i++) {
        o[i] = curve[(s_onset_pos + i) % ONSET_N];   // oldest first
        mean += o[i];
    }
    mean /= ONSET_N;
    for (int i = 0; i < ONSET_N; i++) o[i] -= mean;
    // Smooth it (1-2-1): a period between two analysis steps then peaks as clearly as one that
    // lands on a step (16th notes are ~5.1 steps apart at 126 BPM).
    for (int pass = 0; pass < SMOOTH_PASSES; pass++) {
        float prev = o[0];
        for (int i = 1; i < ONSET_N - 1; i++) {
            float cur = o[i];
            o[i] = 0.25f * prev + 0.5f * cur + 0.25f * o[i + 1];
            prev = cur;
        }
    }
    int lo = (int)floorf(FRAME_HZ * 60 / BPM_MAX), hi = (int)ceilf(FRAME_HZ * 60 / BPM_MIN);
    int flo = (int)floorf(FRAME_HZ * 60 / FOLLOW_MAX), fhi = (int)ceilf(FRAME_HZ * 60 / FOLLOW_MIN);
    float *r = s_r, r0 = 0;
    for (int i = 0; i < ONSET_N; i++) r0 += o[i] * o[i];
    for (int lag = flo - 1; lag <= fhi + 1; lag++) {
        float sum = 0;
        for (int i = lag; i < ONSET_N; i++) sum += o[i] * o[i - lag];
        r[lag] = sum / (ONSET_N - lag) * ONSET_N;   // unbiased
    }
    // Each peak is scored by its interpolated height at its interpolated lag: a period that falls
    // between two steps would otherwise lose to one that lands on a step.
    float best_lag = 0, best_score = 0, best_r = 0;
    for (int lag = lo; lag <= hi; lag++) {
        float y0 = r[lag - 1], y1 = r[lag], y2 = r[lag + 1];
        if (y1 < y0 || y1 < y2) continue;   // not a peak
        float den = y0 - 2 * y1 + y2;
        float d = den < 0 ? 0.5f * (y0 - y2) / den : 0;
        float height = den < 0 ? y1 - 0.125f * (y0 - y2) * (y0 - y2) / den : y1;
        float at = lag + d;
        float oct = log2f(FRAME_HZ * 60 / at / 120);
        float score = height * expf(-0.5f * oct * oct / (PRIOR_OCT * PRIOR_OCT));
        if (best_lag == 0 || score > best_score) { best_lag = at; best_score = score; best_r = height; }
    }
    s_r_lo = flo;   // the following looks a bit further (FOLLOW_MIN..FOLLOW_MAX)
    s_r_hi = fhi;
    *conf = r0 > 0 && best_lag > 0 ? best_r / r0 : 0;
    if (best_lag <= 0 || r0 <= 0) return 0;
    return FRAME_HZ * 60 / best_lag;
}

// How well the kick-range curve matches itself one beat, or two beats, back (kicks on every beat,
// or on 1 and 3), relative to itself: whether there's a steady kick at this tempo.
static IRAM_ATTR float kick_fit(float bpm)
{
    if (bpm <= 0) return 0;
    float o[ONSET_N], mean = 0, r0 = 0, best = 0;
    for (int i = 0; i < ONSET_N; i++) {
        o[i] = s_kicks[(s_onset_pos + i) % ONSET_N];
        mean += o[i];
    }
    mean /= ONSET_N;
    for (int i = 0; i < ONSET_N; i++) {
        o[i] -= mean;
        r0 += o[i] * o[i];
    }
    if (r0 <= 0) return 0;
    for (int mult = 1; mult <= 2; mult++) {
        int centre = (int)lroundf(mult * FRAME_HZ * 60 / bpm);
        for (int lag = centre - 1; lag <= centre + 1; lag++) {
            if (lag < 1 || lag > ONSET_N / 2) continue;
            float sum = 0;
            for (int i = lag; i < ONSET_N; i++) sum += o[i] * o[i - lag];
            best = fmaxf(best, sum / (ONSET_N - lag) * ONSET_N / r0);
        }
    }
    return best;
}

// Where the beats fall: the offset (0..1 of a period, before now) whose grid of beats over an
// onset history collects the most energy: the low end's (kicks and bass, not off-beat hi-hats),
// or without a kick the upper curve the tempo came from.
// Returns the phase it implies now.
static IRAM_ATTR float comb_phase(float lag, const float *curve)
{
    const int STEPS = 16;
    float best_score = -1, best = 0;
    for (int s = 0; s < STEPS; s++) {
        float off = (float)s / STEPS, score = 0;
        for (float back = off * lag; back < ONSET_N - 2; back += lag) {
            int i = (int)back;   // analyses ago; take the stronger of the two neighbours
            float a = curve[(s_onset_pos - 1 - i + 2 * ONSET_N) % ONSET_N];
            float b = curve[(s_onset_pos - 2 - i + 2 * ONSET_N) % ONSET_N];
            float v = fmaxf(a, b);
            score += COMB_POW == 2 ? v * v : v;   // squared: the strongest hit per beat counts most
        }
        if (score > best_score) { best_score = score; best = off; }
    }
    return best;   // a beat was `best` periods ago: that is the phase now
}

// Follow the autocorrelation peak next to the current tempo, if it's a solid one (at least
// LOCAL_MIN of the peak at the best estimate). Returns whether it did.
static bool follow_local(void)
{
    float h, best_h;
    float near = local_peak(s_music_bpm, &h);
    local_peak(s_last_est, &best_h);
    if (near <= 0 || h < LOCAL_MIN * best_h) return false;
    s_music_bpm += FOLLOW_RATE * (near - s_music_bpm);
    return true;
}

/* ---------- analysis ---------- */

static void analyse(const float *hop, int64_t now)
{
    memmove(s_ring, s_ring + HOP, (N - HOP) * sizeof(float));
    memcpy(s_ring + N - HOP, hop, HOP * sizeof(float));

    float sq = 0;
    for (int i = 0; i < HOP; i++) sq += hop[i] * hop[i];
    float rms = sqrtf(sq / HOP);
    float db = 20 * log10f(rms + 1e-9f);
    // The gate opens at its level and closes GATE_HYST_DB below it: room noise right at the
    // gate doesn't make the sound come and go.
    float gate_amp = powf(10, -(float)g_config.mic_gate / 20);
    bool signal = rms > (s_gate_open ? gate_amp * GATE_HYST : gate_amp);
    s_gate_open = signal;
    s_open_ratio = 0.985f * s_open_ratio + (signal ? 0.015f : 0);

    for (int i = 0; i < N; i++) {
        s_re[i] = s_ring[i] * s_win[i];
        s_im[i] = 0;
    }
    fft(s_re, s_im);
    for (int b = 0; b <= N / 2; b++) {
        s_mag[b] = sqrtf(s_re[b] * s_re[b] + s_im[b] * s_im[b]) * (4.0f / N);   // Hann: x2 gain back
    }

    audio_state_t a;
    portENTER_CRITICAL(&s_mux);
    a = s_state;
    portEXIT_CRITICAL(&s_mux);
    float dt = a.t_us ? (now - a.t_us) / 1e6f : (float)HOP / RATE;
    a.t_us = now;
    a.db = db;
    a.signal = signal;

    // Levels: auto-gained, 0 below the gate. A band's gain is capped by the loudest band (its
    // floor is a fraction of that band's peak), so faint hiss doesn't show up as full scale.
    float floor = gate_amp * 0.5f;
    float loud = 0;
    for (int i = 1; i < 4 + AUDIO_BANDS; i++) loud = fmaxf(loud, s_peak[i]);
    float bfloor = fmaxf(floor, loud * 0.12f);
    float lvl = signal ? agc(0, rms, gate_amp * 2) : 0;
    float bass = signal ? agc(1, rms_bins(s_bass_bins), bfloor) : 0;
    float mid = signal ? agc(2, rms_bins(s_mid_bins), bfloor) : 0;
    float high = signal ? agc(3, rms_bins(s_high_bins), bfloor) : 0;
    a.level = release(a.level, lvl);
    a.bass = release(a.bass, bass);
    a.mid = release(a.mid, mid);
    a.high = release(a.high, high);
    for (int i = 0; i < AUDIO_BANDS; i++) {
        float v = signal ? agc(4 + i, rms_bins(s_band_bins[i]), bfloor) : 0;
        a.bands[i] = release(a.bands[i], v);
    }

    // Visualisation: dB against one shared, slowly decaying peak, so the shape stays natural.
    float spec_db[AUDIO_SPECTRUM], top = -200;
    for (int i = 0; i < AUDIO_SPECTRUM; i++) {
        spec_db[i] = 20 * log10f(rms_bins(s_spec_bins[i]) + 1e-9f);
        top = fmaxf(top, spec_db[i]);
    }
    s_spec_peak = fmaxf(top, fmaxf(s_spec_peak - 0.05f, 20 * log10f(floor)));
    for (int i = 0; i < AUDIO_SPECTRUM; i++) {
        float v = signal ? (spec_db[i] - (s_spec_peak - SPEC_RANGE)) / SPEC_RANGE : 0;
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        float prev = a.spectrum[i] / 255.0f;
        a.spectrum[i] = (uint8_t)(release(prev, v) * 255 + 0.5f);
    }

    // Onsets: log spectral flux (how much the spectrum rose since the last analysis), mostly
    // from the low end, against an adaptive threshold of mean + k * spread.
    float eps = gate_amp * 0.1f, flux = 0, flux_low = 0, flux_up = 0;
    for (int b = 1; b <= UPPER_TOP; b++) {
        float lm = logf(s_mag[b] + eps);
        float rise = signal ? fmaxf(0, lm - s_prev_log[b]) : 0;
        if (b <= ONSET_BINS) flux_low += rise;
        else flux_up += rise;
        if (b <= ONSET_HI) flux += rise * (b <= ONSET_BINS ? 1 : ONSET_HI_W);
        s_prev_log[b] = lm;
    }
    // Kicks: a rise in the kick range that is most of the frame's rise in energy (a note's
    // attack clicks down there too, but rises mostly higher up).
    float e_kick = 0, e_all = 0, kflux = 0;
    for (int b = 1; b <= ONSET_HI; b++) {
        float e = s_mag[b] * s_mag[b];
        e_all += e;
        if (b >= KICK_LO && b <= KICK_HI) e_kick += e;
    }
    float de_kick = e_kick - s_prev_e_kick, de_all = e_all - s_prev_e_all;
    bool bassy = de_kick > 0 && de_kick >= KICK_SHARE * de_all;
    s_prev_e_kick = e_kick;
    s_prev_e_all = e_all;
    for (int b = KICK_LO; b <= KICK_HI && signal; b++) {
        kflux += fmaxf(0, logf(s_mag[b] + eps) - s_prev_kick[b]);
    }
    for (int b = KICK_LO; b <= KICK_HI; b++) s_prev_kick[b] = logf(s_mag[b] + eps);
    if (!bassy) kflux = 0;
    bool kick = kflux > ONSET_MIN && kflux > 2 * s_kick_mean && kflux > s_kick_prev &&
                now - s_last_kick_us > ONSET_MIN_US;
    s_kick_mean += 0.05f * (kflux - s_kick_mean);
    s_kick_prev = kflux;
    if (kick) {
        s_last_kick_us = now;
        memmove(s_kick_times, s_kick_times + 1, 3 * sizeof(int64_t));
        s_kick_times[3] = now;
    }
    float sd = sqrtf(s_flux_var);
    float k = g_config.beat_sens / 10.0f;
    bool onset = signal && flux > s_flux_mean + k * sd && flux > FLUX_REL * s_flux_mean &&
                 flux > ONSET_MIN && flux > s_flux_prev &&
                 now - s_last_onset_us > ONSET_MIN_US;
    float dev = flux - s_flux_mean;
    s_flux_mean += 0.05f * dev;
    s_flux_var = 0.95f * (s_flux_var + 0.05f * dev * dev);
    s_flux_prev = flux;
    if (onset) {
        s_last_onset_us = now;
        memmove(s_onset_times, s_onset_times + 1, 3 * sizeof(int64_t));
        s_onset_times[3] = now;
    }
    s_onsets[s_onset_pos] = flux;
    s_onsets_low[s_onset_pos] = flux_low;
    s_onsets_up[s_onset_pos] = flux_up;
    s_kicks[s_onset_pos] = kflux;
    s_onset_pos = (s_onset_pos + 1) % ONSET_N;

    // Tempo, a few times a second: the beat period is the lag at which the onset curve best
    // matches itself, weighted towards ~120 BPM against half/double-time picks.
    bool update = ++s_tempo_tick % 8 == 0;
    if (update) {
        // Two readings: the upper curve's first, then (with a steady kick) the main curve's, so
        // the following below works on the autocorrelation of the one in use.
        float conf_up, conf;
        estimate_tempo(s_onsets_up, &conf_up);
        // A tempo counts with a few clear hits behind it (a steady tone or hiss has none).
        bool active = s_onset_times[0] && now - s_onset_times[0] < 3000000;   // 4 onsets in 3 s
        if (signal) s_signal_seen_us = now;
        // ... and a beat needs a steady kick: without one (an arpeggio, a breakdown, a build-up)
        // there's no telling which notes are the beats, and the beats are the onsets instead.
        // Without a kick, a very regular upper curve (a snare, claps, an arpeggio) gives the
        // tempo instead, and the beats are placed by it.
        bool kicking = s_kick_times[0] && now - s_kick_times[0] < KICK_WIN_US;   // 4 kicks in 3 s
        float bpm = 0;
        bool kick_ok = false;
        if (kicking) {
            bpm = estimate_tempo(s_onsets, &conf);
            kick_ok = conf >= 0.12f && kick_fit(s_music_bpm > 0 ? s_music_bpm : bpm) >= KICK_FIT;
        }
        bool upper_ok = !kick_ok && conf_up >= UPPER_CONF;
        if (upper_ok) {
            bpm = estimate_tempo(s_onsets_up, &conf);   // its autocorrelation for the following
        }
        s_last_est = bpm;
        if (kick_ok || upper_ok) s_upper = upper_ok;
        if (s_music_bpm > 0 && (kick_ok || upper_ok)) s_kick_seen_us = now;
        bool ok = signal && s_open_ratio >= OPEN_MIN && active && (kick_ok || upper_ok);
        if (s_music_bpm > 0 && (now - s_kick_seen_us > KICK_HOLD_US || now - s_signal_seen_us > SILENCE_DROP_US)) {
            s_music_bpm = 0;   // lost: a few bars without a beat, or the sound stopped
            s_cand_n = 0;
        }
        if (ok) {
            if (s_music_bpm <= 0) {
                // First lock: two estimates in a row that agree.
                if (s_cand_n > 0 && fabsf(bpm - s_cand) < s_cand * 0.06f) {
                    s_music_bpm = bpm;
                    s_kick_seen_us = now;
                    s_cand_n = 0;
                } else {
                    s_cand = bpm;
                    s_cand_n = 1;
                }
            } else if (fabsf(bpm - s_music_bpm) < s_music_bpm * FOLLOW_WIN) {
                // Follow it: gradual tempo changes (an accelerando) are tracked here.
                s_music_bpm += FOLLOW_RATE * (bpm - s_music_bpm);
                s_cand_n = 0;
            } else {
                // The best estimate is somewhere else: often another reading of the same music
                // (twice as fast, 3/4...). Keep following our own peak, and let the other one
                // take over only if it clearly fits better, with hits behind it, for ~4 s. An
                // even stream of 16ths fits 4, 5 or 6 notes per beat about equally well, and must
                // not make the beat wander between those.
                follow_local();
                bool better = active && tempo_score(bpm) >= SWITCH_MARGIN * tempo_score(s_music_bpm);
                if (!better) {
                    s_cand_n = 0;
                } else if (s_cand_n > 0 && fabsf(bpm - s_cand) < s_cand * 0.06f) {
                    if (++s_cand_n >= CAND_N) {   // held ~4 s: the music changed
                        s_music_bpm = bpm;
                        s_cand_n = 0;
                    }
                } else {
                    s_cand = bpm;
                    s_cand_n = 1;
                }
            }
        }
    }
    bool was_locked = a.locked;
    set_tempo(&a);
    if (update && a.locked) {
        // Steer the phase towards where the onsets say the beats are: a third of the way,
        // at most PHASE_STEP per update, so an odd pick can't make the beat jump.
        // beat_offset_ms > 0 leads the music, to make up for the lights' own delay.
        float target = comb_phase(FRAME_HZ * 60 / a.bpm, s_upper ? s_onsets_up : s_onsets_low) +
                       g_config.beat_offset_ms / 1000.0f / a.period_s;
        float err = target - a.phase;
        err -= floorf(err + 0.5f);   // -0.5..0.5
        float step = err / 3;
        step = step > PHASE_STEP ? PHASE_STEP : step < -PHASE_STEP ? -PHASE_STEP : step;
        if (!was_locked) step = err;   // a new lock: straight onto the beats
        float p = a.phase + step;
        if (p >= 1) {                // moved forward across a beat: that's the beat
            a.beats++;
            a.last_beat_us = now;
        }
        a.phase = p - floorf(p);
    }

    // Beats: with a tempo, a steady grid; without, the onsets.
    if (a.period_s > 0) {
        grid_advance(&a, dt, now);
    } else if (onset) {
        a.beats++;
        a.last_beat_us = now;
        a.phase = 0;
    }

    portENTER_CRITICAL(&s_mux);
    s_state = a;
    portEXIT_CRITICAL(&s_mux);
}

static esp_err_t mic_init(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan.dma_frame_num = HOP;
    esp_err_t err = i2s_new_channel(&chan, NULL, &s_rx);
    if (err != ESP_OK) return err;
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        // INMP441: 24-bit samples, MSB first in a 32-bit slot; left slot with L/R at GND, right
        // with L/R at VDD. Both are read and the one with data is used.
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = g_config.mic_sck,
            .ws = g_config.mic_ws,
            .dout = I2S_GPIO_UNUSED,
            .din = g_config.mic_sd,
        },
    };
    std.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    err = i2s_channel_init_std_mode(s_rx, &std);
    if (err == ESP_OK) err = i2s_channel_enable(s_rx);
    // The mic leaves SD floating in the slot it doesn't use; the pull-up makes that slot read a
    // steady -1 instead of noise (and an unconnected SD shows up as all 1s).
    if (err == ESP_OK) gpio_pullup_en(g_config.mic_sd);
    if (err != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    return err;
}

static void audio_task(void *arg)
{
    static int32_t raw[2 * HOP];   // interleaved left, right
    static float hop[HOP];
    while (1) {
        bool have = false;
        if (s_rx) {   // the microphone paces the analysis
            size_t got = 0;
            if (i2s_channel_read(s_rx, raw, sizeof(raw), &got, pdMS_TO_TICKS(1000)) == ESP_OK &&
                got == sizeof(raw)) {
                bool alive[2];
                for (int c = 0; c < 2; c++) {
                    int32_t lo = INT32_MAX, hi = INT32_MIN;
                    for (int i = 0; i < HOP; i++) {
                        int32_t v = raw[2 * i + c];
                        lo = v < lo ? v : lo;
                        hi = v > hi ? v : hi;
                    }
                    s_raw_min[c] = lo;
                    s_raw_max[c] = hi;
                    alive[c] = hi != lo;
                }
                // Stick with a slot while it carries data; switch only when the other one does.
                if (s_channel < 0 || (!alive[s_channel] && alive[!s_channel])) {
                    if (alive[0] || alive[1]) {
                        s_channel = alive[0] ? 0 : 1;
                        ESP_LOGI(TAG, "microphone on the %s channel", s_channel ? "right" : "left");
                    }
                }
                int c = s_channel < 0 ? 0 : s_channel;
                for (int i = 0; i < HOP; i++) {
                    hop[i] = (raw[2 * i + c] >> 8) / 8388608.0f;   // 24-bit sample in the top bits
                }
                if (alive[c]) s_last_mic_activity_us = esp_timer_get_time();
                have = true;
                audio_tap_t tap = s_tap;
                if (tap) tap(hop, HOP);
            }
        }
        if (!s_rx) {
            vTaskDelay(pdMS_TO_TICKS(20));
            idle_tick(esp_timer_get_time());
        }
        if (have) {
            // The block's time comes from the sample count, not from when this task got to it:
            // the microphone's DMA buffers ~140 ms, and a task that runs late and then reads
            // several blocks at once would squeeze their timestamps and move the beat. A block
            // that arrives earlier than the count says puts the clock back on time; one that
            // arrives later moves it only slowly (the I2S clock isn't exactly RATE).
            int64_t got = esp_timer_get_time();
            double predicted = s_clock_us + 1e6 * HOP / RATE;
            if (s_clock_us == 0 || got < predicted || got - predicted > 500000) {
                s_clock_us = got;   // first block, on time, or a gap (no data)
            } else {
                s_clock_us = predicted + 0.002 * (got - predicted);
            }
            int32_t late = (int32_t)(got - (int64_t)s_clock_us);
            analyse(hop, (int64_t)s_clock_us);
            int32_t proc = (int32_t)(esp_timer_get_time() - got);
            if (got - s_diag_start_us > DIAG_US) {
                s_proc_last_us = s_proc_max_us;
                s_late_last_us = s_late_max_us;
                s_proc_avg_us = s_proc_n ? (int32_t)(s_proc_sum_us / s_proc_n) : 0;
                s_proc_max_us = s_late_max_us = 0;
                s_proc_sum_us = s_proc_n = 0;
                s_diag_start_us = got;
            }
            if (proc > s_proc_max_us) s_proc_max_us = proc;
            s_proc_sum_us += proc;
            s_proc_n++;
            if (late > s_late_max_us) s_late_max_us = late;
        }
    }
}

void audio_get(audio_state_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_state;
    portEXIT_CRITICAL(&s_mux);
}

float audio_phase_at(const audio_state_t *a, int64_t t_us)
{
    if (a->period_s <= 0) return 0;
    float p = a->phase + (t_us - a->t_us) / 1e6f / a->period_s;
    return p - floorf(p);
}

void audio_set_tap(audio_tap_t tap)
{
    s_tap = tap;
}

cJSON *audio_json(void)
{
    audio_state_t a;
    audio_get(&a);
    int64_t now = esp_timer_get_time();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "mic", a.mic);
    cJSON_AddBoolToObject(r, "mic_data", a.mic && now - s_last_mic_activity_us < 2000000);
    cJSON_AddStringToObject(r, "mic_channel", s_channel < 0 ? "none" : s_channel ? "right" : "left");
    cJSON *raw = cJSON_AddObjectToObject(r, "raw");   // last block's sample range per slot
    cJSON_AddNumberToObject(raw, "left_min", s_raw_min[0]);
    cJSON_AddNumberToObject(raw, "left_max", s_raw_max[0]);
    cJSON_AddNumberToObject(raw, "right_min", s_raw_min[1]);
    cJSON_AddNumberToObject(raw, "right_max", s_raw_max[1]);
    // The last 10 s: the longest analysis of one block (of the ~23 ms between blocks), and the
    // most the audio task ran behind the microphone.
    cJSON_AddNumberToObject(r, "proc_ms_max", (s_proc_last_us > s_proc_max_us ? s_proc_last_us : s_proc_max_us) / 1000.0);
    cJSON_AddNumberToObject(r, "proc_ms_avg", s_proc_avg_us / 1000.0);
    cJSON_AddNumberToObject(r, "late_ms_max", (s_late_last_us > s_late_max_us ? s_late_last_us : s_late_max_us) / 1000.0);
    cJSON_AddBoolToObject(r, "signal", a.signal);
    cJSON_AddNumberToObject(r, "db", roundf(a.db * 10) / 10);
    cJSON_AddNumberToObject(r, "level", a.level);
    cJSON_AddNumberToObject(r, "bass", a.bass);
    cJSON_AddNumberToObject(r, "mid", a.mid);
    cJSON_AddNumberToObject(r, "high", a.high);
    cJSON_AddNumberToObject(r, "beats", a.beats);
    cJSON_AddNumberToObject(r, "bpm", roundf(a.bpm * 10) / 10);
    cJSON_AddBoolToObject(r, "locked", a.locked);
    cJSON_AddNumberToObject(r, "phase", audio_phase_at(&a, now));
    cJSON *b = cJSON_AddArrayToObject(r, "bands");
    for (int i = 0; i < AUDIO_BANDS; i++) cJSON_AddItemToArray(b, cJSON_CreateNumber(roundf(a.bands[i] * 1000) / 1000));
    cJSON *s = cJSON_AddArrayToObject(r, "spectrum");
    for (int i = 0; i < AUDIO_SPECTRUM; i++) cJSON_AddItemToArray(s, cJSON_CreateNumber(a.spectrum[i]));
    return r;
}

esp_err_t audio_start(void)
{
    s_ring = alloc(N * sizeof(float));
    s_re = alloc(N * sizeof(float));
    s_im = alloc(N * sizeof(float));
    s_win = alloc(N * sizeof(float));
    s_cos = alloc(N / 2 * sizeof(float));
    s_sin = alloc(N / 2 * sizeof(float));
    s_mag = alloc((N / 2 + 1) * sizeof(float));
    if (!s_ring || !s_re || !s_im || !s_win || !s_cos || !s_sin || !s_mag) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_ring, 0, N * sizeof(float));
    for (int i = 0; i < N; i++) s_win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (N - 1));
    for (int k = 0; k < N / 2; k++) {
        s_cos[k] = cosf(2 * (float)M_PI * k / N);
        s_sin[k] = sinf(2 * (float)M_PI * k / N);
    }
    s_bass_bins = bins(40, 150);
    s_mid_bins = bins(150, 2000);
    s_high_bins = bins(2000, 8000);
    log_bins(s_band_bins, AUDIO_BANDS);
    log_bins(s_spec_bins, AUDIO_SPECTRUM);

    if (g_config.mic_sck >= 0 && g_config.mic_ws >= 0 && g_config.mic_sd >= 0) {
        esp_err_t err = mic_init();
        if (err == ESP_OK) {
            s_state.mic = true;
            ESP_LOGI(TAG, "INMP441 on SCK %d, WS %d, SD %d", g_config.mic_sck, g_config.mic_ws, g_config.mic_sd);
        } else {
            ESP_LOGW(TAG, "microphone init failed: %s", esp_err_to_name(err));
        }
    }
    // Core 1 with the DMX output (which needs little CPU), below its priority.
    return xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 5, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
