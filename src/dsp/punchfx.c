/*
 * Punch-In FX — PO-33 style effects for Ableton Move
 *
 * 16 pressure-sensitive effects mapped to the left 4x4 pad grid.
 * Up to 3 effects can be stacked simultaneously in series.
 *
 * Audio FX plugin for Move-Anything (audio_fx_api_v2).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

/* Move-Anything API headers */
#include "plugin_api_v1.h"
#include "audio_fx_api_v2.h"

/* ───────────────────────── Constants ───────────────────────── */

#define SR              44100
#define CAPTURE_SEC     4
#define CAPTURE_FRAMES  (SR * CAPTURE_SEC)          /* 176400 */
#define PROC_BUF        4096                        /* power of 2 */
#define PROC_MASK       (PROC_BUF - 1)
#define GRAIN_SIZE      768                         /* ~17ms grains for pitch */
#define NUM_FX          16
#define MAX_ACTIVE      3
#define PI_F            3.14159265358979323846f

/* Default envelope times (in samples) */
#define DEFAULT_ATTACK  441                         /* 10 ms */
#define DEFAULT_RELEASE 1323                        /* 30 ms */

/* Effect indices */
enum {
    FX_LOOP_16 = 0,  FX_LOOP_12,  FX_LOOP_SHORT,  FX_LOOP_SHORTER,
    FX_UNISON,       FX_UNISON_LOW, FX_OCTAVE_UP,  FX_OCTAVE_DOWN,
    FX_STUTTER_4,    FX_STUTTER_3,  FX_SCRATCH,    FX_SCRATCH_FAST,
    FX_QUANTIZE_68,  FX_RETRIGGER,  FX_REVERSE,    FX_NO_EFFECT
};

/*
 * Pad-to-MIDI-note mapping (left 4x4 of Move's 4x8 grid).
 * Move pads: notes 68-99, bottom-left to top-right, 4 rows of 8.
 *
 *   Top row (92-95):    Loop16  Loop12  LoopShort  LoopShorter
 *   Row 3   (84-87):    Unison  UniLow  OctUp      OctDown
 *   Row 2   (76-79):    Stut4   Stut3   Scratch    ScratchFast
 *   Bottom  (68-71):    6/8Q    Retrig  Reverse    NoEffect
 */
static const uint8_t PAD_NOTES[NUM_FX] = {
    92, 93, 94, 95,     /* row 4 (top)    — loops */
    84, 85, 86, 87,     /* row 3          — modulation */
    76, 77, 78, 79,     /* row 2          — stutter / scratch */
    68, 69, 70, 71      /* row 1 (bottom) — time / transform */
};

/* ───────────────────────── Types ───────────────────────── */

typedef struct {
    /* Activation */
    int   active;               /* pad is currently held */
    float gate;                 /* envelope 0..1 */
    float pressure;             /* velocity / aftertouch 0..1 */
    int   activation_order;     /* for sorting active effects */

    /* Capture-based state (loop, scratch, reverse, retrigger) */
    int   seg_start;            /* frame offset in capture buffer */
    int   seg_len;              /* segment length in frames */
    float read_pos;             /* playback position within segment */

    /* Scratch */
    float scratch_dir;          /* +1 or -1 */
    float scratch_timer;        /* frames until direction flip */

    /* Retrigger */
    int   chunk_order[8];
    int   num_chunks;
    int   chunk_size;           /* frames per chunk */
    int   chunk_idx;            /* current chunk being played */
    int   chunk_pos;            /* frame within current chunk */

    /* Processing buffer (pitch shift / chorus) */
    float pbuf_l[PROC_BUF];
    float pbuf_r[PROC_BUF];
    int   pbuf_w;               /* write position */
    float pr_a, pr_b;           /* read heads A and B */
    float grain_phase;          /* 0..grain_size counter */

    /* Chorus LFO */
    float lfo_phase;

    /* Stutter */
    float stutter_phase;

    /* 6/8 quantize */
    float q68_phase;

} effect_state_t;

typedef struct {
    /* Capture ring buffer (stereo interleaved int16) */
    int16_t  capture[CAPTURE_FRAMES * 2];
    int      cap_pos;                       /* write head (frames) */

    /* Per-effect state */
    effect_state_t fx[NUM_FX];

    /* Envelope config */
    float attack_inc;       /* gate increment per sample */
    float release_dec;      /* gate decrement per sample */

    /* Tempo */
    int   bpm;
    int   spb;              /* samples per beat (cached) */

    /* Master mix and volume */
    float master_mix;       /* 0..1 global wet/dry */
    float master_volume;    /* 0..2 overall FX volume, pre-dry/wet */

    /* Activation counter for ordering stacked effects */
    int   activation_seq;

    /* Shift key tracking — FX only trigger when Shift is held */
    int   shift_held;

    /* MIDI clock tracking */
    uint32_t clock_sample_counter;
    uint32_t last_clock_sample;
    int      clock_count;

    /* PRNG state */
    uint32_t rng;

    /* Shadow control shared memory for reading Shift button state.
     * CC 49 is intercepted by the shadow host shim before it reaches
     * the chain, so we read shift_held directly from /move-shadow-control. */
    volatile uint8_t *shadow_ctrl;  /* mmap'd shadow_control_t */
    int shadow_ctrl_fd;             /* shm file descriptor */

    /* Slot identity — used to filter FX_BROADCAST MIDI so only the
     * instance on the currently selected track responds to pads.
     * -1 = unassigned (responds to all), 0-3 = track slot. */
    int my_slot;

} punchfx_instance_t;

static const host_api_v1_t *g_host = NULL;

/* ───────────────────────── Helpers ───────────────────────── */

/* File-based logging — works from any thread including audio render */
#define PLOG_PATH "/data/UserData/schwung/punchfx_debug.log"
static int plog_count = 0;

static void plog(const char *msg) {
    /* Limit total log lines to prevent filling disk */
    if (plog_count > 200) return;
    FILE *f = fopen(PLOG_PATH, "a");
    if (f) {
        fprintf(f, "[punchfx] %s\n", msg);
        fclose(f);
        plog_count++;
    }
}

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float soft_clip(float x) {
    if (x > 1.0f)  return 1.0f - 1.0f / (1.0f + (x - 1.0f));
    if (x < -1.0f) return -1.0f + 1.0f / (1.0f - (x + 1.0f));
    return x;
}

/* Linear interpolation in a float circular buffer (handles negative pos) */
static inline float lerp_buf(const float *buf, int mask, float pos) {
    int size = mask + 1;
    float fp = pos - floorf(pos);
    int i0 = ((int)floorf(pos)) % size;
    if (i0 < 0) i0 += size;
    int i1 = (i0 + 1) & mask;
    return buf[i0] + fp * (buf[i1] - buf[i0]);
}

/* Linear interpolation in the capture buffer (stereo int16, non-power-of-2) */
static inline float cap_lerp(const int16_t *cap, int total_frames, float fpos, int ch) {
    int i0 = ((int)fpos % total_frames + total_frames) % total_frames;
    int i1 = (i0 + 1) % total_frames;
    float f = fpos - floorf(fpos);
    float s0 = cap[i0 * 2 + ch] / 32768.0f;
    float s1 = cap[i1 * 2 + ch] / 32768.0f;
    return s0 + f * (s1 - s0);
}

/* Simple PRNG */
static inline uint32_t prng(uint32_t *state) {
    *state = *state * 1103515245u + 12345u;
    return (*state >> 16) & 0x7FFF;
}

/* Fisher-Yates shuffle */
static void shuffle(int *arr, int n, uint32_t *rng_state) {
    for (int i = n - 1; i > 0; i--) {
        int j = prng(rng_state) % (i + 1);
        int tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
    }
}

/* Samples per beat division: div=1 → quarter, div=2 → 8th, etc. */
static inline int samples_per_div(int spb, int div) {
    return spb / div;
}

/* Map MIDI note to effect index, or -1 if not a left-half pad */
static int note_to_fx(uint8_t note) {
    for (int i = 0; i < NUM_FX; i++) {
        if (PAD_NOTES[i] == note) return i;
    }
    return -1;
}

/* Recalculate cached tempo values */
static void update_tempo(punchfx_instance_t *inst) {
    inst->spb = SR * 60 / inst->bpm;
}

/* ──────────────────── Effect: Loop (0-3) ──────────────────── */

static void loop_activate(punchfx_instance_t *inst, int fx_idx) {
    effect_state_t *fx = &inst->fx[fx_idx];
    int div;
    switch (fx_idx) {
        case FX_LOOP_16:      div = 4; break;   /* 16th notes */
        case FX_LOOP_12:      div = 3; break;   /* triplet 8ths */
        case FX_LOOP_SHORT:   div = 8; break;   /* 32nd notes */
        case FX_LOOP_SHORTER: div = 12; break;  /* 48th notes */
        default: div = 4;
    }
    fx->seg_len = samples_per_div(inst->spb, div);
    if (fx->seg_len < 64) fx->seg_len = 64;
    if (fx->seg_len > CAPTURE_FRAMES / 2) fx->seg_len = CAPTURE_FRAMES / 2;
    fx->seg_start = (inst->cap_pos - fx->seg_len + CAPTURE_FRAMES) % CAPTURE_FRAMES;
    fx->read_pos = 0.0f;
}

static void loop_process(punchfx_instance_t *inst, int fx_idx,
                          float *l, float *r, float gate) {
    effect_state_t *fx = &inst->fx[fx_idx];
    float pos = (float)fx->seg_start + fx->read_pos;
    float loop_l = cap_lerp(inst->capture, CAPTURE_FRAMES, pos, 0);
    float loop_r = cap_lerp(inst->capture, CAPTURE_FRAMES, pos, 1);

    fx->read_pos += 1.0f;
    if (fx->read_pos >= (float)fx->seg_len) fx->read_pos -= (float)fx->seg_len;

    /* Gate controls wet/dry crossfade (100% wet when fully open) */
    *l = *l * (1.0f - gate) + loop_l * 1.2f * gate;
    *r = *r * (1.0f - gate) + loop_r * 1.2f * gate;
}

/* ──────────────── Effect: Unison / Chorus (4-5) ──────────── */

static void unison_activate(punchfx_instance_t *inst, int fx_idx) {
    effect_state_t *fx = &inst->fx[fx_idx];
    memset(fx->pbuf_l, 0, sizeof(fx->pbuf_l));
    memset(fx->pbuf_r, 0, sizeof(fx->pbuf_r));
    fx->pbuf_w = 0;
    fx->lfo_phase = 0.0f;
}

static void unison_process(punchfx_instance_t *inst, int fx_idx,
                            float *l, float *r, float gate) {
    effect_state_t *fx = &inst->fx[fx_idx];
    int is_low = (fx_idx == FX_UNISON_LOW);

    /* LFO rate & max detune */
    float lfo_hz  = is_low ? 0.8f : 1.5f;
    float max_ms  = is_low ? 5.0f : 8.0f;          /* max delay modulation in ms */
    float max_del = max_ms * 0.001f * (float)SR;    /* in samples */

    /* Detune amount scales with pressure */
    float detune = 0.2f + fx->pressure * 0.8f;      /* 20%-100% of max */
    float delay_mod = detune * max_del;

    /* Write to delay line */
    fx->pbuf_l[fx->pbuf_w] = *l;
    fx->pbuf_r[fx->pbuf_w] = *r;
    fx->pbuf_w = (fx->pbuf_w + 1) & PROC_MASK;

    /* Advance LFO */
    fx->lfo_phase += lfo_hz / (float)SR;
    if (fx->lfo_phase >= 1.0f) fx->lfo_phase -= 1.0f;

    /* Two detuned voices (+ and - modulation) */
    float mod1 = sinf(fx->lfo_phase * 2.0f * PI_F) * delay_mod;
    float mod2 = sinf((fx->lfo_phase + 0.5f) * 2.0f * PI_F) * delay_mod;

    float center_delay = max_del + 2.0f;  /* center point of modulation */
    float d1 = center_delay + mod1;
    float d2 = center_delay + mod2;

    float pos1 = (float)fx->pbuf_w - d1;
    float pos2 = (float)fx->pbuf_w - d2;

    float v1_l = lerp_buf(fx->pbuf_l, PROC_MASK, pos1);
    float v1_r = lerp_buf(fx->pbuf_r, PROC_MASK, pos1);
    float v2_l = lerp_buf(fx->pbuf_l, PROC_MASK, pos2);
    float v2_r = lerp_buf(fx->pbuf_r, PROC_MASK, pos2);

    /* Mix: original + two detuned voices, gate controls crossfade */
    float wet_l = (*l + v1_l + v2_l) * 0.667f;
    float wet_r = (*r + v1_r + v2_r) * 0.667f;
    *l = *l * (1.0f - gate) + wet_l * gate;
    *r = *r * (1.0f - gate) + wet_r * gate;
}

/* ──────────────── Effect: Octave Up/Down (6-7) ──────────── */

static void pitch_activate(punchfx_instance_t *inst, int fx_idx) {
    effect_state_t *fx = &inst->fx[fx_idx];
    memset(fx->pbuf_l, 0, sizeof(fx->pbuf_l));
    memset(fx->pbuf_r, 0, sizeof(fx->pbuf_r));
    fx->pbuf_w = 0;
    fx->pr_a = 0.0f;
    fx->pr_b = (float)(GRAIN_SIZE / 2);
    fx->grain_phase = 0.0f;
}

static void pitch_process(punchfx_instance_t *inst, int fx_idx,
                           float *l, float *r, float gate) {
    effect_state_t *fx = &inst->fx[fx_idx];
    float ratio = (fx_idx == FX_OCTAVE_UP) ? 2.0f : 0.5f;

    /* Write input to processing buffer */
    fx->pbuf_l[fx->pbuf_w] = *l;
    fx->pbuf_r[fx->pbuf_w] = *r;
    fx->pbuf_w = (fx->pbuf_w + 1) & PROC_MASK;

    /* Advance read heads at pitch ratio */
    fx->pr_a += ratio;
    if (fx->pr_a >= PROC_BUF) fx->pr_a -= PROC_BUF;
    if (fx->pr_a < 0) fx->pr_a += PROC_BUF;
    fx->pr_b += ratio;
    if (fx->pr_b >= PROC_BUF) fx->pr_b -= PROC_BUF;
    if (fx->pr_b < 0) fx->pr_b += PROC_BUF;

    /* Advance grain phase */
    fx->grain_phase += 1.0f;

    /* Reset head A at the start of each grain period */
    if (fx->grain_phase >= GRAIN_SIZE) {
        fx->grain_phase -= GRAIN_SIZE;
        fx->pr_a = (float)fx->pbuf_w - PROC_BUF / 2.0f;
        if (fx->pr_a < 0) fx->pr_a += PROC_BUF;
    }
    /* Reset head B at the midpoint */
    if (fx->grain_phase >= GRAIN_SIZE / 2 &&
        fx->grain_phase - 1.0f < GRAIN_SIZE / 2) {
        fx->pr_b = (float)fx->pbuf_w - PROC_BUF / 2.0f;
        if (fx->pr_b < 0) fx->pr_b += PROC_BUF;
    }

    /* Hann crossfade: win_a + win_b = 1.0 always */
    float t = fx->grain_phase / (float)GRAIN_SIZE;
    float win_a = 0.5f * (1.0f - cosf(2.0f * PI_F * t));
    float win_b = 0.5f * (1.0f + cosf(2.0f * PI_F * t));

    float a_l = lerp_buf(fx->pbuf_l, PROC_MASK, fx->pr_a);
    float a_r = lerp_buf(fx->pbuf_r, PROC_MASK, fx->pr_a);
    float b_l = lerp_buf(fx->pbuf_l, PROC_MASK, fx->pr_b);
    float b_r = lerp_buf(fx->pbuf_r, PROC_MASK, fx->pr_b);

    float wet_l = a_l * win_a + b_l * win_b;
    float wet_r = a_r * win_a + b_r * win_b;

    /* Gate controls crossfade — 100% wet when fully open */
    *l = *l * (1.0f - gate) + wet_l * gate;
    *r = *r * (1.0f - gate) + wet_r * gate;
}

/* ─────────────── Effect: Stutter (8-9) ─────────────── */

static void stutter_process(punchfx_instance_t *inst, int fx_idx,
                             float *l, float *r, float gate) {
    effect_state_t *fx = &inst->fx[fx_idx];
    int div = (fx_idx == FX_STUTTER_4) ? 2 : 3;    /* 8ths or triplet 8ths */
    int period = samples_per_div(inst->spb, div);
    if (period < 64) period = 64;

    /* Pressure controls duty cycle: light=sparse(20%), hard=dense(80%) */
    float duty = 0.2f + fx->pressure * 0.6f;

    fx->stutter_phase += 1.0f;
    if (fx->stutter_phase >= (float)period) fx->stutter_phase -= (float)period;

    float phase_norm = fx->stutter_phase / (float)period;
    float gate_val;

    /* Smooth edges with short cosine ramp (~2ms) */
    float ramp = 88.0f / (float)period;  /* ~2ms ramp as fraction of period */
    if (phase_norm < duty) {
        if (phase_norm < ramp)
            gate_val = 0.5f * (1.0f - cosf(PI_F * phase_norm / ramp));
        else if (phase_norm > duty - ramp)
            gate_val = 0.5f * (1.0f + cosf(PI_F * (phase_norm - duty + ramp) / ramp));
        else
            gate_val = 1.0f;
    } else {
        gate_val = 0.0f;
    }

    *l *= gate_val;
    *r *= gate_val;
}

/* ─────────────── Effect: Scratch (10-11) ─────────────── */

static void scratch_activate(punchfx_instance_t *inst, int fx_idx) {
    effect_state_t *fx = &inst->fx[fx_idx];
    fx->seg_len = inst->spb;   /* 1 beat of audio */
    if (fx->seg_len > CAPTURE_FRAMES / 2) fx->seg_len = CAPTURE_FRAMES / 2;
    fx->seg_start = (inst->cap_pos - fx->seg_len + CAPTURE_FRAMES) % CAPTURE_FRAMES;
    fx->read_pos = 0.0f;
    fx->scratch_dir = 1.0f;
    fx->scratch_timer = 0.0f;
}

static void scratch_process(punchfx_instance_t *inst, int fx_idx,
                             float *l, float *r, float gate) {
    effect_state_t *fx = &inst->fx[fx_idx];
    int is_fast = (fx_idx == FX_SCRATCH_FAST);

    /* Speed: 1x-4x (normal) or 2x-5x (fast), scales with pressure */
    float base_speed = is_fast ? 2.0f : 1.0f;
    float max_speed  = is_fast ? 5.0f : 4.0f;
    float speed = base_speed + fx->pressure * (max_speed - base_speed);

    /* Direction change rate: faster at higher pressure */
    float change_period = inst->spb / (2.0f + fx->pressure * 6.0f);
    if (change_period < 256.0f) change_period = 256.0f;

    fx->scratch_timer += 1.0f;
    if (fx->scratch_timer >= change_period) {
        fx->scratch_timer -= change_period;
        fx->scratch_dir *= -1.0f;
    }

    /* Advance read position */
    fx->read_pos += speed * fx->scratch_dir;

    /* Wrap within segment */
    while (fx->read_pos >= (float)fx->seg_len) fx->read_pos -= (float)fx->seg_len;
    while (fx->read_pos < 0.0f) fx->read_pos += (float)fx->seg_len;

    float pos = (float)fx->seg_start + fx->read_pos;
    float scr_l = cap_lerp(inst->capture, CAPTURE_FRAMES, pos, 0);
    float scr_r = cap_lerp(inst->capture, CAPTURE_FRAMES, pos, 1);

    /* Gate controls crossfade — 100% wet when fully open */
    *l = *l * (1.0f - gate) + scr_l * gate;
    *r = *r * (1.0f - gate) + scr_r * gate;
}

/* ──────────── Effect: 6/8 Quantize (12) ──────────── */

static void q68_activate(punchfx_instance_t *inst, int fx_idx) {
    effect_state_t *fx = &inst->fx[fx_idx];
    memset(fx->pbuf_l, 0, sizeof(fx->pbuf_l));
    memset(fx->pbuf_r, 0, sizeof(fx->pbuf_r));
    fx->pbuf_w = 0;
    fx->pr_a = 0.0f;
    fx->q68_phase = 0.0f;
}

static void q68_process(punchfx_instance_t *inst, int fx_idx,
                         float *l, float *r, float gate) {
    effect_state_t *fx = &inst->fx[fx_idx];

    /* Write input to buffer */
    fx->pbuf_l[fx->pbuf_w] = *l;
    fx->pbuf_r[fx->pbuf_w] = *r;
    fx->pbuf_w = (fx->pbuf_w + 1) & PROC_MASK;

    /*
     * Create 6/8 feel by varying read speed in a pattern:
     * Long-short-short (dotted quarter + eighth + eighth).
     * Within each beat pair, the first 2/3 is stretched, last 1/3 compressed.
     */
    float beat_pair = (float)(inst->spb * 2);   /* two beats = one 6/8 bar */
    if (beat_pair < 128.0f) beat_pair = 128.0f;

    fx->q68_phase += 1.0f;
    if (fx->q68_phase >= beat_pair) fx->q68_phase -= beat_pair;

    float t = fx->q68_phase / beat_pair;        /* 0..1 within beat pair */
    float speed;
    if (t < 0.667f) {
        speed = 1.0f / 1.333f;                  /* stretch (slower) */
    } else {
        speed = 1.0f / 0.5f;                    /* compress (faster) */
    }

    /* Blend with normal speed based on pressure */
    float strength = fx->pressure * fx->pressure;
    float actual_speed = 1.0f + (speed - 1.0f) * strength;

    fx->pr_a += actual_speed;
    if (fx->pr_a >= PROC_BUF) fx->pr_a -= PROC_BUF;
    if (fx->pr_a < 0) fx->pr_a += PROC_BUF;

    *l = lerp_buf(fx->pbuf_l, PROC_MASK, fx->pr_a);
    *r = lerp_buf(fx->pbuf_r, PROC_MASK, fx->pr_a);
}

/* ──────────── Effect: Retrigger (13) ──────────── */

static void retrigger_activate(punchfx_instance_t *inst, int fx_idx) {
    effect_state_t *fx = &inst->fx[fx_idx];

    /* Capture 2 beats of audio, divide into chunks */
    int total = inst->spb * 2;
    if (total > CAPTURE_FRAMES / 2) total = CAPTURE_FRAMES / 2;

    fx->num_chunks = 4 + (int)(inst->fx[fx_idx].pressure * 4.0f); /* 4-8 chunks */
    if (fx->num_chunks > 8) fx->num_chunks = 8;
    fx->chunk_size = total / fx->num_chunks;
    if (fx->chunk_size < 64) fx->chunk_size = 64;

    fx->seg_start = (inst->cap_pos - total + CAPTURE_FRAMES) % CAPTURE_FRAMES;
    fx->seg_len = total;

    /* Initialize and shuffle chunk order */
    for (int i = 0; i < fx->num_chunks; i++) fx->chunk_order[i] = i;
    shuffle(fx->chunk_order, fx->num_chunks, &inst->rng);

    fx->chunk_idx = 0;
    fx->chunk_pos = 0;
}

static void retrigger_process(punchfx_instance_t *inst, int fx_idx,
                               float *l, float *r, float gate) {
    effect_state_t *fx = &inst->fx[fx_idx];

    /* Which chunk and where within it */
    int chunk = fx->chunk_order[fx->chunk_idx];
    int frame_in_capture = fx->seg_start + chunk * fx->chunk_size + fx->chunk_pos;
    float pos = (float)(frame_in_capture % CAPTURE_FRAMES);

    float ret_l = cap_lerp(inst->capture, CAPTURE_FRAMES, pos, 0);
    float ret_r = cap_lerp(inst->capture, CAPTURE_FRAMES, pos, 1);

    fx->chunk_pos++;
    if (fx->chunk_pos >= fx->chunk_size) {
        fx->chunk_pos = 0;
        fx->chunk_idx++;
        if (fx->chunk_idx >= fx->num_chunks) {
            fx->chunk_idx = 0;
            /* Re-shuffle on each loop (more randomization at high pressure) */
            if (fx->pressure > 0.5f) {
                shuffle(fx->chunk_order, fx->num_chunks, &inst->rng);
            }
        }
    }

    /* Gate controls crossfade — 100% wet when fully open */
    *l = *l * (1.0f - gate) + ret_l * gate;
    *r = *r * (1.0f - gate) + ret_r * gate;
}

/* ──────────── Effect: Reverse (14) ──────────── */

static void reverse_activate(punchfx_instance_t *inst, int fx_idx) {
    effect_state_t *fx = &inst->fx[fx_idx];
    fx->seg_len = inst->spb * 2;       /* 2 beats */
    if (fx->seg_len > CAPTURE_FRAMES / 2) fx->seg_len = CAPTURE_FRAMES / 2;
    fx->seg_start = (inst->cap_pos - fx->seg_len + CAPTURE_FRAMES) % CAPTURE_FRAMES;
    fx->read_pos = (float)(fx->seg_len - 1);   /* start at end, go backwards */
}

static void reverse_process(punchfx_instance_t *inst, int fx_idx,
                             float *l, float *r, float gate) {
    effect_state_t *fx = &inst->fx[fx_idx];

    /* Speed: 1x at low pressure, 1.3x at high pressure */
    float speed = 1.0f + fx->pressure * 0.3f;

    float pos = (float)fx->seg_start + fx->read_pos;
    float rev_l = cap_lerp(inst->capture, CAPTURE_FRAMES, pos, 0);
    float rev_r = cap_lerp(inst->capture, CAPTURE_FRAMES, pos, 1);

    fx->read_pos -= speed;
    if (fx->read_pos < 0.0f) fx->read_pos += (float)fx->seg_len;

    /* Gate controls crossfade — 100% wet when fully open */
    *l = *l * (1.0f - gate) + rev_l * gate;
    *r = *r * (1.0f - gate) + rev_r * gate;
}

/* ──────────── Effect: No Effect (15) ──────────── */
/* Pad 16 clears all other effects */

static void noeffect_activate(punchfx_instance_t *inst) {
    for (int i = 0; i < NUM_FX - 1; i++) {
        inst->fx[i].active = 0;
        /* Gate will decay via release envelope in process_block */
    }
}

/* ──────────── Effect dispatcher ──────────── */

static void activate_effect(punchfx_instance_t *inst, int idx) {
    switch (idx) {
        case FX_LOOP_16: case FX_LOOP_12:
        case FX_LOOP_SHORT: case FX_LOOP_SHORTER:
            loop_activate(inst, idx);
            break;
        case FX_UNISON: case FX_UNISON_LOW:
            unison_activate(inst, idx);
            break;
        case FX_OCTAVE_UP: case FX_OCTAVE_DOWN:
            pitch_activate(inst, idx);
            break;
        case FX_STUTTER_4: case FX_STUTTER_3:
            /* Stutter needs no special activation */
            inst->fx[idx].stutter_phase = 0.0f;
            break;
        case FX_SCRATCH: case FX_SCRATCH_FAST:
            scratch_activate(inst, idx);
            break;
        case FX_QUANTIZE_68:
            q68_activate(inst, idx);
            break;
        case FX_RETRIGGER:
            retrigger_activate(inst, idx);
            break;
        case FX_REVERSE:
            reverse_activate(inst, idx);
            break;
        case FX_NO_EFFECT:
            noeffect_activate(inst);
            break;
    }
}

static void process_effect(punchfx_instance_t *inst, int idx,
                            float *l, float *r, float gate) {
    switch (idx) {
        case FX_LOOP_16: case FX_LOOP_12:
        case FX_LOOP_SHORT: case FX_LOOP_SHORTER:
            loop_process(inst, idx, l, r, gate);
            break;
        case FX_UNISON: case FX_UNISON_LOW:
            unison_process(inst, idx, l, r, gate);
            break;
        case FX_OCTAVE_UP: case FX_OCTAVE_DOWN:
            pitch_process(inst, idx, l, r, gate);
            break;
        case FX_STUTTER_4: case FX_STUTTER_3:
            stutter_process(inst, idx, l, r, gate);
            break;
        case FX_SCRATCH: case FX_SCRATCH_FAST:
            scratch_process(inst, idx, l, r, gate);
            break;
        case FX_QUANTIZE_68:
            q68_process(inst, idx, l, r, gate);
            break;
        case FX_RETRIGGER:
            retrigger_process(inst, idx, l, r, gate);
            break;
        case FX_REVERSE:
            reverse_process(inst, idx, l, r, gate);
            break;
        /* FX_NO_EFFECT: does nothing to audio */
    }
}

/* ── Read Shift state from shadow control shared memory ── */

/* Byte offsets in shadow_control_t (from shadow_constants.h) */
#define SHADOW_CTRL_UI_SLOT       6
#define SHADOW_CTRL_SELECTED_SLOT 20
#define SHADOW_CTRL_SHIFT_OFFSET  21
#define SHADOW_CTRL_SIZE          64   /* CONTROL_BUFFER_SIZE */
#define SHM_SHADOW_CONTROL        "/move-shadow-control"

static void read_shift_from_shm(punchfx_instance_t *inst) {
    if (!inst->shadow_ctrl) return;

    int now = inst->shadow_ctrl[SHADOW_CTRL_SHIFT_OFFSET] ? 1 : 0;
    int was = inst->shift_held;
    inst->shift_held = now;

    /* Shift released → release all active FX */
    if (was && !now) {
        for (int e = 0; e < NUM_FX; e++) {
            inst->fx[e].active = 0;
        }
    }
}

/* ──────────── Audio processing ──────────── */

static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {
    punchfx_instance_t *inst = (punchfx_instance_t *)instance;
    if (!inst || !audio_inout) return;

    /* Read Shift button state from shadow control shared memory */
    read_shift_from_shm(inst);

    /* Quick check: are ANY effects currently sounding (gate > 0)?
     * If nothing is active or releasing, leave audio_inout completely untouched. */
    int any_sounding = 0;
    for (int e = 0; e < NUM_FX; e++) {
        if (inst->fx[e].active || inst->fx[e].gate > 0.0001f) {
            any_sounding = 1;
            break;
        }
    }

    /* Capture audio into ring buffer only when Shift is held or FX are active.
     * This provides audio to loop/scratch/reverse when FX are triggered. */
    if (inst->shift_held || any_sounding) {
        for (int i = 0; i < frames; i++) {
            inst->capture[inst->cap_pos * 2]     = audio_inout[i * 2];
            inst->capture[inst->cap_pos * 2 + 1] = audio_inout[i * 2 + 1];
            inst->cap_pos = (inst->cap_pos + 1) % CAPTURE_FRAMES;
        }
    }

    /* If nothing is sounding, don't touch the audio buffer at all — true bypass */
    if (!any_sounding) return;

    /* Process audio with active effects */
    for (int i = 0; i < frames; i++) {
        float dry_l = audio_inout[i * 2]     / 32768.0f;
        float dry_r = audio_inout[i * 2 + 1] / 32768.0f;

        /* Update gate envelopes for all effects */
        for (int e = 0; e < NUM_FX; e++) {
            effect_state_t *fx = &inst->fx[e];
            if (fx->active) {
                fx->gate += inst->attack_inc;
                if (fx->gate > 1.0f) fx->gate = 1.0f;
            } else {
                fx->gate -= inst->release_dec;
                if (fx->gate < 0.0f) fx->gate = 0.0f;
            }
        }

        /* Collect active effects (gate > 0), sorted by activation order */
        int active_ids[NUM_FX];
        int active_count = 0;
        for (int e = 0; e < NUM_FX; e++) {
            if (inst->fx[e].gate > 0.0001f && e != FX_NO_EFFECT) {
                active_ids[active_count++] = e;
            }
        }

        /* Sort by activation_order (simple insertion sort, max 16 elements) */
        for (int a = 1; a < active_count; a++) {
            int key = active_ids[a];
            int j = a - 1;
            while (j >= 0 && inst->fx[active_ids[j]].activation_order >
                             inst->fx[key].activation_order) {
                active_ids[j + 1] = active_ids[j];
                j--;
            }
            active_ids[j + 1] = key;
        }

        /* Limit to MAX_ACTIVE (keep the first 3 activated) */
        if (active_count > MAX_ACTIVE) active_count = MAX_ACTIVE;

        /* Process effect chain in series */
        float out_l = dry_l;
        float out_r = dry_r;

        for (int a = 0; a < active_count; a++) {
            int e = active_ids[a];
            /* Gate controls wet/dry crossfade (smooth attack/release).
             * Pressure modulates effect character (speed, detune, duty)
             * inside each effect — NOT the mix amount. */
            process_effect(inst, e, &out_l, &out_r, inst->fx[e].gate);
        }

        /* Apply master volume (pre-dry/wet) then dry/wet mix */
        if (active_count > 0) {
            out_l *= inst->master_volume;
            out_r *= inst->master_volume;
            out_l = dry_l * (1.0f - inst->master_mix) + out_l * inst->master_mix;
            out_r = dry_r * (1.0f - inst->master_mix) + out_r * inst->master_mix;
        }

        /* Soft clip and write back */
        out_l = soft_clip(out_l);
        out_r = soft_clip(out_r);
        audio_inout[i * 2]     = (int16_t)(out_l * 32767.0f);
        audio_inout[i * 2 + 1] = (int16_t)(out_r * 32767.0f);
    }
}

/* ──────────── MIDI handling ──────────── */

/*
 * on_midi: handles pad note triggers, aftertouch, and MIDI clock.
 *
 * Left-half pads (notes 68-71, 76-79, 84-87, 92-95) trigger FX directly.
 * CC 49 (Shift) doesn't reach audio FX on_midi (shadow host intercepts it),
 * so Shift gating is handled by ui_chain.js when the edit page is active.
 */
static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    punchfx_instance_t *inst = (punchfx_instance_t *)instance;
    if (!inst || len < 1) return;

    /* MIDI Clock (24 ppqn) — auto-detect BPM */
    if (msg[0] == 0xF8) {
        inst->clock_sample_counter += MOVE_FRAMES_PER_BLOCK;
        inst->clock_count++;
        if (inst->clock_count >= 24) {
            if (inst->last_clock_sample > 0) {
                uint32_t elapsed = inst->clock_sample_counter - inst->last_clock_sample;
                if (elapsed > 0) {
                    int detected_bpm = (int)((float)SR * 60.0f / (float)elapsed + 0.5f);
                    if (detected_bpm >= 40 && detected_bpm <= 300) {
                        inst->bpm = detected_bpm;
                        update_tempo(inst);
                    }
                }
            }
            inst->last_clock_sample = inst->clock_sample_counter;
            inst->clock_count = 0;
        }
        return;
    }

    if (len < 3) return;
    uint8_t channel = msg[0] & 0x0F;
    uint8_t status  = msg[0] & 0xF0;
    uint8_t d1      = msg[1];
    uint8_t d2      = msg[2];

    /* Only respond to channel 0 — Move pads are always ch 0.
     * MoveOriginal sequences use other channels and must be ignored. */
    if (channel != 0) return;

    /* Note On: activate FX only if Shift is held AND this is the selected slot */
    if (status == 0x90 && d2 > 0) {
        if (!inst->shift_held) return;  /* No Shift → let synth handle it */

        /* Only respond on the currently selected track */
        if (inst->my_slot >= 0 && inst->shadow_ctrl) {
            int selected = inst->shadow_ctrl[SHADOW_CTRL_SELECTED_SLOT];
            if (inst->my_slot != selected) return;
        }

        int fx_idx = note_to_fx(d1);
        if (fx_idx < 0) return;

        effect_state_t *fx = &inst->fx[fx_idx];
        fx->active = 1;
        fx->pressure = (float)d2 / 127.0f;
        fx->activation_order = inst->activation_seq++;
        activate_effect(inst, fx_idx);
        return;
    }

    /* Note Off: deactivate FX if it was active */
    if (status == 0x80 || (status == 0x90 && d2 == 0)) {
        int fx_idx = note_to_fx(d1);
        if (fx_idx >= 0 && inst->fx[fx_idx].active) {
            inst->fx[fx_idx].active = 0;
        }
        return;
    }

    /* Polyphonic Aftertouch: update pressure on active FX */
    if (status == 0xA0) {
        int fx_idx = note_to_fx(d1);
        if (fx_idx >= 0 && inst->fx[fx_idx].active) {
            inst->fx[fx_idx].pressure = (float)d2 / 127.0f;
        }
        return;
    }

    /* Channel Aftertouch: update all active effects */
    if (status == 0xD0) {
        float p = (float)d1 / 127.0f;
        for (int e = 0; e < NUM_FX; e++) {
            if (inst->fx[e].active) {
                inst->fx[e].pressure = p;
            }
        }
    }
}

/* ──────────── Parameter handling ──────────── */

static int json_get_float(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    *out = atof(p);
    return 0;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    punchfx_instance_t *inst = (punchfx_instance_t *)instance;
    if (!inst) return;

    /* Auto-detect our slot index from shadow_control->ui_slot.
     * ui_slot is set by the chain host when editing a specific slot's FX.
     * Skip for "state" (called during chain restore for ALL slots) and
     * "knob_" keys (framework overlay — ui_slot may differ from selected_slot). */
    if (strcmp(key, "state") != 0 && strncmp(key, "knob_", 5) != 0 && inst->shadow_ctrl) {
        int slot = inst->shadow_ctrl[SHADOW_CTRL_UI_SLOT];
        if (slot <= 3 && inst->my_slot != slot) {
            inst->my_slot = slot;
            char sbuf[64];
            snprintf(sbuf, sizeof(sbuf), "Slot assigned via ui_slot: %d (key=%s)", slot, key);
            plog(sbuf);
        }
    }

    if (strcmp(key, "state") == 0) {
        /* Also restore slot from saved state */
        float v;
        if (json_get_float(val, "slot", &v) == 0) {
            int slot = (int)v;
            if (slot >= 0 && slot <= 3) {
                inst->my_slot = slot;
                char sbuf[64];
                snprintf(sbuf, sizeof(sbuf), "Slot restored from state: %d", slot);
                plog(sbuf);
            }
        }
        if (json_get_float(val, "bpm", &v) == 0) {
            int bpm = (int)(v + 0.5f);
            if (bpm >= 40 && bpm <= 300) { inst->bpm = bpm; update_tempo(inst); }
        }
        if (json_get_float(val, "mix", &v) == 0)
            inst->master_mix = clampf(v, 0.0f, 1.0f);
        if (json_get_float(val, "volume", &v) == 0)
            inst->master_volume = clampf(v, 0.0f, 2.0f);
        /* Attack/release always use defaults — not restored from state */
        return;
    }

    /*
     * Pad trigger via set_param (workaround: chain host doesn't route
     * pad MIDI to audio FX on_midi, so ui_chain.js sends these instead).
     *
     *   pad_on   = "<fx_index> <velocity>"    (0-15, 0-127)
     *   pad_off  = "<fx_index>"
     *   pad_at   = "<fx_index> <pressure>"    (aftertouch update)
     */
    if (strcmp(key, "pad_on") == 0) {
        int idx = 0, vel = 100;
        if (sscanf(val, "%d %d", &idx, &vel) >= 1 && idx >= 0 && idx < NUM_FX) {
            effect_state_t *fx = &inst->fx[idx];
            fx->active = 1;
            fx->pressure = (float)vel / 127.0f;
            fx->activation_order = inst->activation_seq++;
            activate_effect(inst, idx);
        }
        return;
    }
    if (strcmp(key, "pad_off") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < NUM_FX) {
            inst->fx[idx].active = 0;
        }
        return;
    }
    if (strcmp(key, "pad_at") == 0) {
        int idx = 0, prs = 64;
        if (sscanf(val, "%d %d", &idx, &prs) >= 1 && idx >= 0 && idx < NUM_FX) {
            if (inst->fx[idx].active) {
                inst->fx[idx].pressure = (float)prs / 127.0f;
            }
        }
        return;
    }

    /* Shift state from ui_chain.js (fallback if CC 49 doesn't reach on_midi) */
    if (strcmp(key, "shift") == 0) {
        int was = inst->shift_held;
        inst->shift_held = atoi(val);
        /* Shift released → release all active FX */
        if (was && !inst->shift_held) {
            for (int e = 0; e < NUM_FX; e++) {
                inst->fx[e].active = 0;
            }
        }
        return;
    }

    /* Knob adjust: knob 1=BPM, knob 2=Volume, knob 3=Dry/Wet */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_adjust")) {
        int idx = atoi(key + 5) - 1;
        int delta = atoi(val);
        if (idx == 0) {
            inst->bpm += delta;
            if (inst->bpm < 40) inst->bpm = 40;
            if (inst->bpm > 300) inst->bpm = 300;
            update_tempo(inst);
        } else if (idx == 1) {
            inst->master_volume = clampf(inst->master_volume + delta * 0.02f, 0.0f, 2.0f);
        } else if (idx == 2) {
            inst->master_mix = clampf(inst->master_mix + delta * 0.01f, 0.0f, 1.0f);
        }
        return;
    }

    float v = atof(val);

    if (strcmp(key, "bpm") == 0) {
        int bpm = (int)(v + 0.5f);
        if (bpm >= 40 && bpm <= 300) { inst->bpm = bpm; update_tempo(inst); }
    } else if (strcmp(key, "mix") == 0) {
        inst->master_mix = clampf(v, 0.0f, 1.0f);
    } else if (strcmp(key, "volume") == 0) {
        inst->master_volume = clampf(v * 2.0f, 0.0f, 2.0f);
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    punchfx_instance_t *inst = (punchfx_instance_t *)instance;
    if (!inst) return -1;

    if (strcmp(key, "bpm") == 0)
        return snprintf(buf, buf_len, "%d", inst->bpm);
    if (strcmp(key, "mix") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->master_mix);
    if (strcmp(key, "volume") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->master_volume / 2.0f);
    if (strcmp(key, "name") == 0)
        return snprintf(buf, buf_len, "Punch-In FX");
    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"bpm\":%d,\"mix\":%.4f,\"volume\":%.4f,\"slot\":%d}",
            inst->bpm, inst->master_mix, inst->master_volume, inst->my_slot);
    }
    if (strcmp(key, "chain_params") == 0) {
        static const char *cp = "["
            "{\"key\":\"bpm\",\"name\":\"BPM\",\"type\":\"int\",\"min\":40,\"max\":300,\"default\":120,\"step\":1},"
            "{\"key\":\"volume\",\"name\":\"Volume\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.5,\"step\":0.01},"
            "{\"key\":\"mix\",\"name\":\"Dry/Wet\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":1,\"step\":0.01}"
        "]";
        int l = (int)strlen(cp);
        if (l >= buf_len) return -1;
        memcpy(buf, cp, l + 1);
        return l;
    }
    if (strcmp(key, "ui_hierarchy") == 0) {
        static const char *h = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"bpm\",\"volume\",\"mix\"],"
                    "\"params\":[\"bpm\",\"volume\",\"mix\"]"
                "}"
            "}"
        "}";
        int l = (int)strlen(h);
        if (l >= buf_len) return -1;
        memcpy(buf, h, l + 1);
        return l;
    }
    /* Knob overlay: knob 1=BPM, knob 2=Volume, knob 3=Dry/Wet */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_name")) {
        int idx = atoi(key + 5) - 1;
        const char *names[] = {"BPM", "Volume", "Dry/Wet"};
        if (idx >= 0 && idx < 3) return snprintf(buf, buf_len, "%s", names[idx]);
        return -1;
    }
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_value")) {
        int idx = atoi(key + 5) - 1;
        if (idx == 0) return snprintf(buf, buf_len, "%d", inst->bpm);
        if (idx == 1) return snprintf(buf, buf_len, "%d%%", (int)(inst->master_volume * 50.0f));
        if (idx == 2) return snprintf(buf, buf_len, "%d%%", (int)(inst->master_mix * 100.0f));
        return -1;
    }
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_adjust")) {
        /* Handled in set_param */
        return -1;
    }

    return -1;
}

/* ──────────── Lifecycle ──────────── */

static void *v2_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;

    plog("Creating Punch-In FX instance");

    punchfx_instance_t *inst = (punchfx_instance_t *)calloc(1, sizeof(punchfx_instance_t));
    if (!inst) {
        plog("Failed to allocate instance");
        return NULL;
    }

    /* Defaults */
    inst->bpm = 120;
    inst->master_mix = 1.0f;
    inst->master_volume = 1.0f;
    inst->attack_inc = 1.0f / (float)DEFAULT_ATTACK;
    inst->release_dec = 1.0f / (float)DEFAULT_RELEASE;
    inst->rng = 42;
    inst->my_slot = -1;   /* Unknown slot — will auto-detect on first UI interaction */
    update_tempo(inst);

    /* Apply config_json defaults if provided */
    if (config_json) {
        float v;
        if (json_get_float(config_json, "bpm", &v) == 0) {
            int bpm = (int)(v + 0.5f);
            if (bpm >= 40 && bpm <= 300) inst->bpm = bpm;
        }
        if (json_get_float(config_json, "mix", &v) == 0)
            inst->master_mix = clampf(v, 0.0f, 1.0f);
        if (json_get_float(config_json, "volume", &v) == 0)
            inst->master_volume = clampf(v, 0.0f, 2.0f);
        update_tempo(inst);
    }

    /* Map shadow control shared memory to read Shift button state.
     * The shadow host writes shift_held at byte offset 21. */
    inst->shadow_ctrl_fd = shm_open(SHM_SHADOW_CONTROL, O_RDONLY, 0);
    if (inst->shadow_ctrl_fd >= 0) {
        inst->shadow_ctrl = (volatile uint8_t *)mmap(NULL, SHADOW_CTRL_SIZE,
                                                      PROT_READ, MAP_SHARED,
                                                      inst->shadow_ctrl_fd, 0);
        if (inst->shadow_ctrl == MAP_FAILED) {
            inst->shadow_ctrl = NULL;
            close(inst->shadow_ctrl_fd);
            inst->shadow_ctrl_fd = -1;
            plog("shadow-control mmap failed — Shift detection disabled");
        } else {
            plog("shadow-control mmap OK — Shift detection via SHM enabled");
        }
    } else {
        inst->shadow_ctrl = NULL;
        plog("shadow-control shm_open failed — Shift detection disabled");
    }

    char logbuf[128];
    snprintf(logbuf, sizeof(logbuf),
             "Instance created (%d BPM, %d spb, %.1f MB)",
             inst->bpm, inst->spb,
             (float)sizeof(punchfx_instance_t) / (1024.0f * 1024.0f));
    plog(logbuf);

    return inst;
}

static void v2_destroy_instance(void *instance) {
    if (!instance) return;
    punchfx_instance_t *inst = (punchfx_instance_t *)instance;
    if (inst->shadow_ctrl) {
        munmap((void *)inst->shadow_ctrl, SHADOW_CTRL_SIZE);
    }
    if (inst->shadow_ctrl_fd >= 0) {
        close(inst->shadow_ctrl_fd);
    }
    plog("Destroying instance");
    free(instance);
}

/* ──────────── Plugin entry point ──────────── */

static audio_fx_api_v2_t g_api;

/*
 * Exported MIDI handler symbol — the chain host resolves this via dlsym()
 * to route pad MIDI (notes, aftertouch) to audio FX modules.
 * Without this export, the chain host silently skips MIDI delivery.
 */
void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    v2_on_midi(instance, msg, len, source);
}

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_api, 0, sizeof(g_api));
    g_api.api_version      = AUDIO_FX_API_VERSION_2;
    g_api.create_instance  = v2_create_instance;
    g_api.destroy_instance = v2_destroy_instance;
    g_api.process_block    = v2_process_block;
    g_api.set_param        = v2_set_param;
    g_api.get_param        = v2_get_param;
    g_api.on_midi          = v2_on_midi;

    plog("Punch-In FX plugin initialized (v2)");

    return &g_api;
}
