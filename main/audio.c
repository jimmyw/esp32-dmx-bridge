#include "audio.h"

#include <math.h>
#include <string.h>
#include "config.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
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
#define FLUX_REL    2.0f                 // an onset also needs flux > FLUX_REL * the mean
#define PRIOR_OCT   0.9f                 // tempo prior: spread around 120 BPM, in octaves
#define PHASE_STEP  0.04f                // largest phase correction per tempo update (~5/s)
#define ONSET_MIN_US 200000              // onsets at least 200 ms apart
#define ONSET_N     256                  // onset curve kept for the tempo: ~6 s
#define ONSET_MIN   2.0f                 // flux an onset needs at least (sum of log rises)
#define BPM_MIN     70
#define BPM_MAX     180

typedef struct { int lo, hi; } range_t;  // FFT bins [lo, hi)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static audio_state_t s_state;            // published (guarded by s_mux)
static volatile bool s_demo;
static i2s_chan_handle_t s_rx;

// Analysis (audio task only)
static float *s_ring;                    // last N samples
static float *s_re, *s_im, *s_win, *s_cos, *s_sin, *s_mag;
static range_t s_band_bins[AUDIO_BANDS], s_spec_bins[AUDIO_SPECTRUM];
static range_t s_bass_bins, s_mid_bins, s_high_bins;
static float s_peak[4 + AUDIO_BANDS];    // AGC peaks: level, bass, mid, high, bands...
static float s_spec_peak;
static float s_prev_log[ONSET_HI + 1];
static int64_t s_onset_times[4];         // the last 4 onsets: a tempo needs them recent
static float s_flux_mean, s_flux_var, s_flux_prev;
static int64_t s_last_onset_us, s_tempo_seen_us;
static float s_onsets[ONSET_N];          // onset curve (ring, newest at s_onset_pos - 1): tempo
static float s_onsets_low[ONSET_N];      // the same from the low end only: where the beats fall
static int   s_onset_pos, s_tempo_tick;
static float s_cand;                     // tempo candidate that differs from the current one
static int   s_cand_n;
static int64_t s_last_mic_activity_us;
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

static void fft(float *re, float *im)
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

static float rms_bins(range_t r)
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

/* ---------- demo track: 120 BPM kick, off-beat hi-hat, a slow pad ---------- */

static uint32_t s_demo_n;   // sample counter

static void demo_fill(float *out)
{
    const float beat_s = 0.5f;   // 120 BPM
    for (int i = 0; i < HOP; i++, s_demo_n++) {
        float t = (float)s_demo_n / RATE;
        float tb = fmodf(t, beat_s);                       // time since the beat
        float th = fmodf(t + beat_s / 2, beat_s);          // time since the off-beat
        // kick: pitch sweeping 120 -> 50 Hz, ~80 ms decay
        float kick = 0.6f * expf(-tb / 0.08f) * sinf(2 * (float)M_PI * (50 * tb + 70 * 0.03f * (1 - expf(-tb / 0.03f))));
        float noise = (float)(esp_random() & 0xFFFF) / 32768.0f - 1;
        float hat = 0.12f * expf(-th / 0.02f) * noise;
        float swell = 0.5f + 0.5f * sinf(2 * (float)M_PI * t / 8);   // 8 s swell
        float pad = 0.04f * swell * (sinf(2 * (float)M_PI * 220 * t) + sinf(2 * (float)M_PI * 277.2f * t) +
                                     sinf(2 * (float)M_PI * 329.6f * t));
        out[i] = kick + hat + pad;
    }
}

/* ---------- tempo ---------- */

// Autocorrelation of the onset curve over BPM_MIN..BPM_MAX, weighted by a log-normal prior
// around 120 BPM. Returns BPM (parabolic interpolation between lags) and *conf = r(lag) / r(0).
static float estimate_tempo(float *conf)
{
    float o[ONSET_N], mean = 0;
    for (int i = 0; i < ONSET_N; i++) {
        o[i] = s_onsets[(s_onset_pos + i) % ONSET_N];   // oldest first
        mean += o[i];
    }
    mean /= ONSET_N;
    for (int i = 0; i < ONSET_N; i++) o[i] -= mean;
    int lo = (int)floorf(FRAME_HZ * 60 / BPM_MAX), hi = (int)ceilf(FRAME_HZ * 60 / BPM_MIN);
    float r[64], r0 = 0;
    for (int i = 0; i < ONSET_N; i++) r0 += o[i] * o[i];
    for (int lag = lo - 1; lag <= hi + 1; lag++) {
        float sum = 0;
        for (int i = lag; i < ONSET_N; i++) sum += o[i] * o[i - lag];
        r[lag] = sum / (ONSET_N - lag) * ONSET_N;   // unbiased
    }
    int best = -1;
    float best_score = 0;
    for (int lag = lo; lag <= hi; lag++) {
        float oct = log2f(FRAME_HZ * 60 / lag / 120);
        float score = r[lag] * expf(-0.5f * oct * oct / (PRIOR_OCT * PRIOR_OCT));
        if (best < 0 || score > best_score) { best = lag; best_score = score; }
    }
    *conf = r0 > 0 && best > 0 ? r[best] / r0 : 0;
    if (best < 0 || r0 <= 0) return 0;
    float y0 = r[best - 1], y1 = r[best], y2 = r[best + 1], den = y0 - 2 * y1 + y2;
    float lag = best + (den < 0 ? 0.5f * (y0 - y2) / den : 0);
    return FRAME_HZ * 60 / lag;
}

// Where the beats fall: the offset (0..1 of a period, before now) whose grid of beats over the
// low-end onset history collects the most energy (kicks and bass, not off-beat hi-hats).
// Returns the phase it implies now.
static float comb_phase(float lag)
{
    const int STEPS = 16;
    float best_score = -1, best = 0;
    for (int s = 0; s < STEPS; s++) {
        float off = (float)s / STEPS, score = 0;
        for (float back = off * lag; back < ONSET_N - 2; back += lag) {
            int i = (int)back;   // analyses ago; take the stronger of the two neighbours
            float a = s_onsets_low[(s_onset_pos - 1 - i + 2 * ONSET_N) % ONSET_N];
            float b = s_onsets_low[(s_onset_pos - 2 - i + 2 * ONSET_N) % ONSET_N];
            score += fmaxf(a, b);
        }
        if (score > best_score) { best_score = score; best = off; }
    }
    return best;   // a beat was `best` periods ago: that is the phase now
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
    float gate_amp = powf(10, -(float)g_config.mic_gate / 20);
    bool signal = rms > gate_amp;

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
    float eps = gate_amp * 0.1f, flux = 0, flux_low = 0;
    for (int b = 1; b <= ONSET_HI; b++) {
        float lm = logf(s_mag[b] + eps);
        float rise = signal ? fmaxf(0, lm - s_prev_log[b]) : 0;
        if (b <= ONSET_BINS) flux_low += rise;
        flux += rise * (b <= ONSET_BINS ? 1 : ONSET_HI_W);
        s_prev_log[b] = lm;
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
    s_onset_pos = (s_onset_pos + 1) % ONSET_N;

    // Tempo, a few times a second: the beat period is the lag at which the onset curve best
    // matches itself, weighted towards ~120 BPM against half/double-time picks.
    if (++s_tempo_tick % 8 == 0) {
        float conf;
        float bpm = estimate_tempo(&conf);
        bool active = s_onset_times[0] && now - s_onset_times[0] < 3000000;   // 4 onsets in 3 s
        if (conf < 0.12f || !signal || !active) {
            if (a.bpm > 0 && now - s_tempo_seen_us > 5000000) {   // 5 s without a tempo: lost
                a.bpm = 0;
                a.period_s = 0;
            }
        } else {
            s_tempo_seen_us = now;
            if (a.bpm <= 0) {
                a.bpm = bpm;
            } else if (fabsf(bpm - a.bpm) < a.bpm * 0.06f) {
                a.bpm += 0.2f * (bpm - a.bpm);
                s_cand_n = 0;
            } else if (s_cand_n > 0 && fabsf(bpm - s_cand) < s_cand * 0.06f) {
                if (++s_cand_n >= 6) {   // a new tempo held ~1 s: the music changed
                    a.bpm = bpm;
                    s_cand_n = 0;
                }
            } else {
                s_cand = bpm;
                s_cand_n = 1;
            }
            a.period_s = 60 / a.bpm;
            // Steer the phase towards where the onsets say the beats are: a third of the way,
            // at most PHASE_STEP per update, so an odd pick can't make the beat jump.
            // beat_offset_ms > 0 leads the music, to make up for the lights' own delay.
            float target = comb_phase(FRAME_HZ * 60 / a.bpm) + g_config.beat_offset_ms / 1000.0f / a.period_s;
            float err = target - a.phase;
            err -= floorf(err + 0.5f);   // -0.5..0.5
            float step = err / 3;
            step = step > PHASE_STEP ? PHASE_STEP : step < -PHASE_STEP ? -PHASE_STEP : step;
            float p = a.phase + step;
            if (p >= 1) {                // moved forward across a beat: that's the beat
                a.beats++;
                a.last_beat_us = now;
            }
            a.phase = p - floorf(p);
        }
    }

    // Beats: with a tempo, a steady grid (the phase wrapping); without one, the onsets.
    if (a.period_s > 0) {
        float p = a.phase + dt / a.period_s;
        if (p >= 1) {
            a.beats++;
            a.last_beat_us = now;
        }
        a.phase = p - floorf(p);
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
    int64_t next = esp_timer_get_time();
    while (1) {
        bool have = false;
        if (s_rx) {   // the microphone paces the analysis, also in demo mode
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
            }
        }
        if (s_demo) {
            if (!s_rx) {   // pace it ourselves
                next += HOP_US;
                int64_t wait = next - esp_timer_get_time();
                if (wait < -100000) next = esp_timer_get_time();   // fell behind: don't burst
                if (wait > 1000) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
            }
            demo_fill(hop);
            have = true;
        } else if (!s_rx) {
            vTaskDelay(pdMS_TO_TICKS(200));
            next = esp_timer_get_time();
        }
        if (have) {
            analyse(hop, esp_timer_get_time());
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

void audio_set_demo(bool on)
{
    s_demo = on;
    portENTER_CRITICAL(&s_mux);
    s_state.demo = on;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "demo %s", on ? "on" : "off");
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
    cJSON_AddBoolToObject(r, "demo", a.demo);
    cJSON_AddBoolToObject(r, "signal", a.signal);
    cJSON_AddNumberToObject(r, "db", roundf(a.db * 10) / 10);
    cJSON_AddNumberToObject(r, "level", a.level);
    cJSON_AddNumberToObject(r, "bass", a.bass);
    cJSON_AddNumberToObject(r, "mid", a.mid);
    cJSON_AddNumberToObject(r, "high", a.high);
    cJSON_AddNumberToObject(r, "beats", a.beats);
    cJSON_AddNumberToObject(r, "bpm", roundf(a.bpm * 10) / 10);
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
