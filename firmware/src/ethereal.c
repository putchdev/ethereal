/* SPDX-License-Identifier: GPL-3.0-only */
/* Ethereal: liquid drum and bass pads and sub bass for the FM-1, played from an external sequencer.
 * Done: the clock (M2), the pad (M3), the sub (M4), the FX (M5), the duck and LFOs (M6), the pages (M7).
 * This is the file to change. Everything below it (platform.c) is done: updates, the USB rescue,
 * USB-MIDI, USB audio, keys, knobs, LEDs, the screen, a settings sector.
 *
 *   keys       the lowest 12 (F3..E4, glowing) play the sub, two octaves down, the rest the pad; SEL makes them
 *              all pad. The notes go out over USB-MIDI too, on the pad's and the sub's channels
 *   PRESETS    the patch: MIST, GLASS, HALO, KEYS, STAB, then USER1..4 (or Program Change 0..8)
 *   SAVE       then PRESETS picks a user slot, SAVE writes the sound into it; a page button cancels
 *   HOME EDIT ENV LFO FX GLO   the page KNOB1..4 edit (PARAMS): perform, pad osc, envelopes, modulation, effects,
 *              sub + duck. SELECT (turned): settings, the channels, the sub's zone, how long the clock may drop
 *   PLAY       the chord latch: let go and the pad holds; the next chord replaces it. Its LED: lit when on,
 *              else a flash on each beat while the transport runs
 *   OCT- OCT+  the octave
 * *   MASTER     the volume (platform.c)
 *   MIDI       the pad's channel (1) plays it, with its velocity; CC 64 holds it, CC 123 stops it. The sub's (2)
 *              plays it, with its velocity; CC 123 stops it. On either channel: CC 20..43 turn the 24 knobs in page
 *              order (also 74 cutoff, 1 LFO 1, 93 chorus, 94 duck, 5 glide, 73 attack, 72 release), 91 the reverb
 *              send, Program Change the patch. A CC is heard, not saved. The duck pumps the pad on each beat while
 *              the transport runs. Clock and Start / Continue / Stop drive the bar. USB or TRS
 *   ALGORITHM  the internal tempo, while no clock comes in
 *   flash      all of it (the sound as the knobs left it, the user patches, the settings), 2 s after the last change once
 *              nothing sounds
 *
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division.
 * tests/host.c runs this file on a computer: the screen as a PNG, the sound as a WAV.
 * What platform.c offers: docs/API.md. The chip's rules: docs/RULES.md. */
#include "platform.c"

#ifdef ETHEREAL_HOST
#define HOST_LOG(...) printf(__VA_ARGS__)
#else
#define HOST_LOG(...) ((void)0)
#endif

/* ---- the clock. The main loop counts MIDI clock ticks (24 a beat, 96 a bar) and measures the tempo; with no
 * tick for 250 ms it makes its own at the last tempo, so the bar never stops. The audio ISR keeps the bar phase
 * (2^32 a bar), running at the measured tempo and pulled gently towards each tick, so jitter never jumps it. */
#define TPB 24u
#define TPBAR 96u
#define TICK_PH 44739243u                       /* 2^32 / 96 */
#define INC_K 259710796u                        /* bar phase per frame = INC_K / (ms per tick * 256) */
#define PER_MIN 3200u                           /* ms per tick * 256 at 200 BPM */
#define PER_MAX 10667u                          /* at 60 BPM */
#define BPM10_K 6400000u                        /* BPM * 10 = BPM10_K / (ms per tick * 256) */
static uint32_t lost_ms = 250;                  /* no clock this long: lost (a setting) */
#define HIST 384u                               /* tick times kept: the tempo is measured over up to 4 bars */

static volatile uint32_t clk_word;              /* main loop: tick in the bar | its ms (16 bits) << 8 | epoch << 24 */
static volatile uint32_t clk_inc = INC_K / 3678u;   /* main loop: bar phase per frame (174 BPM until told) */
static volatile uint32_t bar_ph;                /* audio ISR: where in the bar at the start of this block, 2^32 a bar */
static uint32_t bars;                           /* audio ISR: bars since Start (the 2- and 4-bar LFOs) */
static uint8_t started;                         /* audio ISR: Start this block */
static volatile uint8_t clk_run;                /* main loop: the transport runs (the duck) */

static void clock_audio(uint32_t n)
{
    static uint32_t epoch = 256, n_last;
    uint32_t w = clk_word, inc = clk_inc, ph = bar_ph + inc * n_last;
    uint32_t want = (w & 127u) * TICK_PH + ((fm1_ms - (w >> 8)) & 0xFFFFu) * (inc * 441u / 10u);
    started = w >> 24 != epoch;
    if (started) {                              /* Start: straight to the downbeat */
        epoch = w >> 24;
        ph = want;
        bars = 0;
    } else {
        uint32_t was = bar_ph;
        ph += (uint32_t)((int32_t)(want - ph) >> 3);
        if (ph < was && (int32_t)(ph - was) > 0) bars++;          /* (the pull to the tick may cross the bar line */
        else if (ph > was && (int32_t)(ph - was) < 0) bars--;     /* backwards too) */
    }
    bar_ph = ph;
    n_last = n;
}

static struct {
    uint32_t t[HIST], n, have;                  /* real tick times in ms, t[n % HIST]; have = how many in a row */
    uint32_t period;                            /* ms per tick * 256 */
    uint32_t last;                              /* ms * 256 of the last tick, real or made */
    uint32_t last_ext;                          /* ms of the last real tick */
    uint32_t start_ms, off;                     /* off: ticks in a row the last beat's tempo has disagreed */
    uint32_t tick, epoch;
    uint8_t ext, running, start;
} clk;

static void clock_advance(uint32_t k, uint32_t t, int real)   /* k ticks on, the last at t (ms * 256) */
{
    if (clk.start && (real || (int32_t)((t >> 8) - clk.start_ms) > (int32_t)lost_ms)) {   /* the first real tick after Start
                                                                                   is the downbeat (a made-up one only
                                                                                   when no clock follows) */
        clk.start = 0;
        clk.tick = 0;
        clk.epoch++;
    } else
        clk.tick = (clk.tick + k) % TPBAR;
    clk.last = t;
    clk_word = clk.tick | ((t >> 8) & 0xFFFFu) << 8 | (clk.epoch & 0xFFu) << 24;
}

static void clock_tempo(void)
{
    uint32_t i = clk.n - 1u, k = clk.have - 1u, kf = k < TPB ? k : TPB, fast, slow;
    int32_t d;
    fast = ((clk.t[i % HIST] - clk.t[(i - kf) % HIST]) << 8) / kf;
    slow = ((clk.t[i % HIST] - clk.t[(i - k) % HIST]) << 8) / k;
    d = (int32_t)(fast - slow);
    if ((d < 0 ? -d : d) * 25 <= (int32_t)slow)
        clk.off = 0;
    else if (++clk.off >= TPB) {                /* over 4 % off for a whole beat: a new tempo, not late clocks (a
                                                   sender's main loop can hold one back by more than a tick) */
        clk.off = 0;
        clk.have = kf + 1u;
        slow = fast;
    }
    clk.period = slow < PER_MIN ? PER_MIN : slow > PER_MAX ? PER_MAX : slow;
    clk_inc = INC_K / clk.period;
}

static void clock_ext(uint32_t now, uint32_t stamp)   /* a real tick, stamped with the low 16 bits of its ms */
{
    uint32_t t_ms = now - ((now - stamp) & 0xFFFFu), t = t_ms << 8, k;
    int32_t dt = (int32_t)(t - clk.last);
    if (clk.ext)
        k = 1;                                  /* locked: every real tick is one, however early or late */
    else {                                      /* back: as many ticks on from our made-up ones as the time says */
        k = dt <= 0 ? 0u : ((uint32_t)dt + clk.period / 2u) / clk.period;
        clk.have = 0;                           /* the old times say nothing about now */
        HOST_LOG("%6u ms  clock EXT\n", now);
    }
    clk.ext = 1;
    clk.last_ext = t_ms;
    clock_advance(k, t, 1);                     /* k = 0: the tick we made up already, now at its real time */
    clk.t[clk.n++ % HIST] = t_ms;
    if (clk.have < HIST) clk.have++;
    if (clk.have >= 2u) clock_tempo();
}

static void clock_service(uint32_t now)         /* lost clock, and the made-up ticks */
{
    if (clk.ext && (int32_t)(now - clk.last_ext) > (int32_t)lost_ms) {
        clk.ext = 0;
        HOST_LOG("%6u ms  clock INT\n", now);
    }
    if (!clk.ext)
        while ((int32_t)((now << 8) - clk.last) >= (int32_t)clk.period)
            clock_advance(1, clk.last + clk.period, 0);
}

static void clock_set_bpm10(uint32_t bpm10)
{
    clk.period = BPM10_K / (bpm10 < 600u ? 600u : bpm10 > 2000u ? 2000u : bpm10);
    clk_inc = INC_K / clk.period;
}

static uint32_t clock_bpm10(void) { return (BPM10_K + clk.period / 2u) / clk.period; }

/* ---- the pad (runs in the audio ISR: keep it short, no flash, no screen). Six voices: two detuned oscillators,
 * a sine an octave down and noise, through a 12 dB lowpass. Per frame the sources and the filter; the
 * envelopes, the pitch and the cutoff every ENV_CTRL frames, the gain and the cutoff ramped in between */
#include "ethereal_tables.h"
#define NV 6u
#define Q30 (1 << 30)
#define VOICE_GAIN 7000                         /* Q15: six voices at full level stay near full scale */
enum { OSC_SAW, OSC_TRI };
enum { ST_IDLE, ST_ATT, ST_DEC, ST_REL, ST_FADE };

typedef struct {
    char name[8];
    uint8_t osc_a, osc_b, detune;               /* detune: osc B above A in cents, 0..25 */
    uint8_t att, dec, sus, rel;                 /* times as indices 0..127, 1 ms * 16000^(i/127); sus 0..255 */
    uint8_t cutoff, res;                        /* cutoff 0..255: 40 Hz * 300^(i/255); res 0..255 */
    int8_t fenv;                                /* filter envelope: cutoff steps at full velocity (32 an octave) */
    uint8_t fatt, fdec;                         /* its attack and decay, as the times above */
    uint8_t sub, noise;                         /* 0..255: up to 50 % and 20 % */
    uint8_t cho_depth, cho_rate, cho_mix;       /* chorus: 7 ms to 7..20 ms, 0.1..1.5 Hz, 0..50 % wet */
    uint8_t dly_time, dly_fb, dly_tone, dly_send;   /* delay: DLY_8 .. DLY_4T; 0..85 %; dark..bright; 0..100 % */
    uint8_t rev_size, rev_decay, rev_damp, rev_pre, rev_send, rev_mix;   /* reverb: 50..100 %; 1..12 s; bright..dark;
                                                                            0..60 ms; 0..100 %; 0..100 % */
    uint8_t l1_shape, l1_rate, l1_depth;        /* LFO 1 on the cutoff: LFO_SINE / TRI (| LFO_FREE); 1/4, 1/2, 1, 2,
                                                   4 bars (free: 0.02..2 Hz over 0..255); 0..+-1.5 octaves */
    uint8_t l2_shape, l2_rate, l2_depth;        /* LFO 2 on the level: SINE / TRI / SQR (| LFO_FREE); 1/16, 1/8T,
                                                   1/8, 1/4 (free: 0.5..10 Hz); 0..100 % dip */
} patch_t;
enum { DLY_8, DLY_8D, DLY_4, DLY_4T };
enum { LFO_SINE, LFO_TRI, LFO_SQR, LFO_FREE = 0x80 };
enum { L1_QUARTER, L1_HALF, L1_BAR, L1_2BARS, L1_4BARS };
enum { L2_16, L2_8T, L2_8, L2_4 };

static const patch_t PATCHES[] = {
    /*            osc A    osc B    det  att dec sus  rel  cut  res  fenv fatt fdec sub noise  chorus d r mix  delay time fb tone send  reverb size decay damp pre send mix  LFO 1 shape rate depth  LFO 2 */
    {"MIST",  OSC_SAW, OSC_SAW, 12,  93, 100, 217, 109, 113, 50,  24,  96, 109,   0,  40,  150,  60, 200,  DLY_8D, 135,  90,  70,  220, 140, 150,  60, 160, 150,  LFO_SINE, L1_2BARS, 70,  LFO_SINE, L2_8,  0},   /* Hospital-style wash */
    {"GLASS", OSC_TRI, OSC_TRI,  6,  79, 100, 180, 105, 175, 90,  30,  30,  96,   0, 160,  100,  90, 160,  DLY_4,  120, 160,  90,  255, 200,  80, 100, 180, 170,  LFO_TRI,  L1_BAR,   30,  LFO_SINE, L2_16, 30},   /* soft, bell-like, air */
    {"HALO",  OSC_SAW, OSC_TRI, 18, 100, 103, 230, 114, 121, 64,  90, 103, 112,  40,  60,  200,  40, 220,  DLY_4T, 150,  70,  60,  240, 230, 120,  40, 200, 180,  LFO_SINE, L1_4BARS, 90,  LFO_TRI,  L2_8T, 25},   /* swelling, wide */
    {"KEYS",  OSC_TRI, OSC_TRI,  3,  21,  91, 100,  93, 175, 40,  70,   9,  79, 200,   0,  120, 140, 140,  DLY_8D, 105, 120,  80,  150,  60, 170, 120, 110, 120,  LFO_SINE, L1_BAR,    0,  LFO_SINE, L2_8,  60},   /* Rhodes-ish stabs */
    {"STAB",  OSC_SAW, OSC_SAW,  8,   0,  74,  40,  72, 140, 80,  60,   0,  74,   0,   0,   80,  90, 120,  DLY_8D, 150,  70, 180,  200, 140, 150,  40, 130, 130,  LFO_SINE, L1_4BARS, 40,  LFO_SINE, L2_8,   0},   /* dub / garage chords: short, filtered */
};
#define NPATCH (sizeof PATCHES / sizeof PATCHES[0])
static patch_t patch;                           /* the sound: written by the main loop, read by the ISR */

static struct {
    volatile uint32_t p_inc;                    /* main: the next note's step, f * 2^32 / 44100 */
    volatile uint32_t trig;                     /* main: + 1 a note-on, after p_inc and p_vel */
    volatile uint8_t gate, p_vel;               /* main */
    volatile uint8_t stage;                     /* ISR */
    uint8_t note, pedal;                        /* main; pedal: released while CC 64 holds it */
    uint32_t age;                               /* main */
    volatile int32_t lvl;                       /* ISR: the envelope, Q30 (main reads it to pick a voice) */
    uint32_t inc, ph_a, ph_b, ph_s, inc_a, inc_b, drift_ph, drift_inc, trig_seen, seed;   /* ISR (drift, pan, seed: boot) */
    int32_t amp, gl, gr, fade;                  /* ISR: gain now Q15; pan gains (fixed); fade step Q30 */
    uint8_t vel;                                /* ISR: the playing note's */
    int32_t flvl, f, low, band;                 /* ISR: filter envelope Q30, F now Q15, the filter's state */
    uint8_t fstage;
} voice[NV];

static int32_t osc(uint32_t ph, uint32_t inc, uint32_t type)   /* -32768..32768 */
{
    int32_t s = (int32_t)(ph >> 16) - 32768, y;
    uint32_t d;
    if (type == OSC_TRI)
        return ((s < 0 ? -s : s) << 1) - 32768;
    if (ph < inc || ph > ~inc) {                /* a frame from the saw's drop: PolyBLEP, (1 - x)^2 x the step */
        d = (ph < inc ? ph : ~ph) / ((inc >> 15) | 1u);
        y = 32768 - (int32_t)(d > 32768u ? 32768u : d);
        y = (y * y) >> 15;
        return ph < inc ? s + y : s - y;
    }
    return s;
}

static int32_t sine(uint32_t ph)                /* a full cycle over 2^32 -> -32767..32767 (a refined parabola, ~0.1 %) */
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;               /* 4x(1-|x|), Q15 */
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15; /* + 0.225 (y|y| - y) */
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}

static int32_t env_to(int32_t lvl, int32_t target, uint32_t k) { return lvl + (int32_t)(((int64_t)(target - lvl) * k) >> 24); }

#define NSTEP (HALF_FRAMES / ENV_CTRL)
static volatile int32_t ui_l1;                  /* audio ISR: LFO 1 times its depth, -32767..32767 (the sky sways with it) */
static int32_t mod_cut[NSTEP];                  /* audio ISR: LFO 1, cutoff 1/256 steps, for each control step */
static int32_t mod_trem[NSTEP + 1], mod_duck[NSTEP + 1], mod_duck_sub[NSTEP + 1];   /* Q15 gains at each step's
                                                                                      edge ([0]: the last block's end) */

static int32_t voice_ctrl(uint32_t v, uint32_t k)   /* every ENV_CTRL frames (step k of the block): envelopes and
                                                       pitch; returns the F to reach */
{
    int32_t lvl = voice[v].lvl, fl = voice[v].flvl, tri, idx;
    uint32_t inc, half;
    if (voice[v].trig != voice[v].trig_seen && voice[v].stage != ST_FADE) {
        voice[v].trig_seen = voice[v].trig;
        if (lvl > 0 && voice[v].p_inc != voice[v].inc) {
            voice[v].stage = ST_FADE;           /* stolen: out in three steps, a fourth for the gain to follow (2.9 ms), then the new note */
            voice[v].fade = lvl / 3 + 1;
        } else {                                /* silent, or the same note again: from the level it is at */
            voice[v].inc = voice[v].p_inc;
            voice[v].vel = voice[v].p_vel;
            voice[v].stage = ST_ATT;
            voice[v].fstage = ST_ATT;
        }
    }
    if (!voice[v].gate && (voice[v].stage == ST_ATT || voice[v].stage == ST_DEC))
        voice[v].stage = ST_REL;
    switch (voice[v].stage) {
    case ST_ATT:
        lvl = env_to(lvl, 1395864371 /* 1.3 in Q30: at 1.0 after the attack time */, ATT_K[patch.att]);
        if (lvl >= Q30) { lvl = Q30; voice[v].stage = ST_DEC; }
        break;
    case ST_DEC:
        lvl = env_to(lvl, (int32_t)patch.sus << 22, DEC_K[patch.dec]);
        break;
    case ST_REL:
        lvl = env_to(lvl, -(Q30 / 100), DEC_K[patch.rel]);   /* aims under 0 to get there */
        if (lvl <= 0) { lvl = 0; voice[v].stage = ST_IDLE; }
        break;
    case ST_FADE:
        if (lvl > 0) {
            lvl -= voice[v].fade;
            if (lvl < 0) lvl = 0;
        } else if (voice[v].amp == 0) {         /* the gain is down too (it ramps a step behind): from scratch,
                                                   with the newest note it was given */
            fl = 0;
            voice[v].low = voice[v].band = 0;
            voice[v].trig_seen = voice[v].trig;
            voice[v].inc = voice[v].p_inc;
            voice[v].vel = voice[v].p_vel;
            voice[v].stage = ST_ATT;
            voice[v].fstage = ST_ATT;
            HOST_LOG("%6u ms  voice %u faded\n", fm1_ms, v);
        }
        break;
    }
    voice[v].lvl = lvl;
    if (voice[v].fstage == ST_ATT) {            /* the filter's envelope: attack, then decay to nothing */
        fl = env_to(fl, 1395864371, ATT_K[patch.fatt]);
        if (fl >= Q30) { fl = Q30; voice[v].fstage = ST_DEC; }
    } else if (voice[v].fstage == ST_DEC) {
        fl = env_to(fl, -(Q30 / 100), DEC_K[patch.fdec]);
        if (fl <= 0) { fl = 0; voice[v].fstage = ST_IDLE; }
    }
    voice[v].flvl = fl;

    voice[v].drift_ph += voice[v].drift_inc;    /* a slow triangle, +-3 cents */
    tri = (int32_t)(voice[v].drift_ph >> 16) - 32768;
    tri = ((tri < 0 ? -tri : tri) << 1) - 32768;
    inc = voice[v].inc;
    inc += (uint32_t)(int32_t)(((int64_t)inc * tri * 57) >> 30);
    half = (uint32_t)(((uint64_t)inc * DETUNE_Q32[patch.detune]) >> 33);   /* half the detune each way */
    voice[v].inc_a = inc - half;
    voice[v].inc_b = inc + half;

    idx = (int32_t)patch.cutoff << 8;           /* the cutoff, in 1/256 steps: + the envelope x velocity */
    idx += (((int32_t)patch.fenv * (fl >> 14)) >> 8) * (9830 + voice[v].vel * 180) >> 15;
    idx += mod_cut[k];
    idx = idx < 0 ? 0 : idx > 255 * 256 ? 255 * 256 : idx;
    return CUT_F[idx >> 8] + (idx >> 8 < 255 ? ((CUT_F[(idx >> 8) + 1] - CUT_F[idx >> 8]) * (idx & 255)) >> 8 : 0);
}

#ifdef PROFILE                                  /* ./build.sh --profile: the render time, ISR-written, in ticks */
static volatile uint32_t prof_max, prof_pad, prof_cd, prof_rev, prof_voices;   /* the worst: all, the pads, chorus
                                                                                    and delay, the reverb */
#define PROF_STAGE(var) do { uint32_t t1 = fm1_ticks(); if (t1 - t0 > var) var = t1 - t0; t0 = t1; } while (0)
#else
#define PROF_STAGE(var) ((void)0)
#endif

static void pad_render(int32_t *out, uint32_t n)   /* the voices, added into out: the pad bus */
{
    uint32_t v, i, b;
#ifdef PROFILE
    uint32_t nv = 0;
#endif
    int32_t q = 29491 - patch.res * 75;         /* damping Q14: 1.8 .. 0.63, a mild peak at most (over 1.998 the
                                                   filter runs away at 12 kHz: Chamberlin wants q < 2/F - F/2) */
    int32_t g_sub = patch.sub * 64, g_noise = patch.noise * 26;   /* Q15: 50 %, 20 % at 255 */
    for (v = 0; v < NV; v++) {
        if (voice[v].stage == ST_IDLE && voice[v].trig == voice[v].trig_seen)
            continue;                           /* silent: costs nothing */
#ifdef PROFILE
        nv++;
#endif
        for (b = 0; b < n; b += ENV_CTRL) {
            uint32_t pa, pb, ps, ia, ib, ta = patch.osc_a, tb = patch.osc_b, seed = voice[v].seed;
            int32_t g, amp = voice[v].amp, step, gl = voice[v].gl, gr = voice[v].gr;
            int32_t f = voice[v].f, fstep, low, band, ft;
            int32_t *o = out + 2u * b;
            ft = voice_ctrl(v, b / ENV_CTRL);   /* (first: a fade's end clears the filter) */
            low = voice[v].low;
            band = voice[v].band;
            fstep = (ft - f) >> 5;
            g = (int32_t)(((int64_t)(voice[v].lvl >> 15) * (9830 + voice[v].vel * 180)) >> 15);   /* velocity: 30..100 % */
            step = (g - amp) >> 5;              /* ramped over the ENV_CTRL frames */
            pa = voice[v].ph_a; pb = voice[v].ph_b; ps = voice[v].ph_s; ia = voice[v].inc_a; ib = voice[v].inc_b;
            for (i = 0; i < ENV_CTRL; i++) {
                int32_t x, s, k;
                pa += ia;
                pb += ib;
                ps += ia >> 1;
                seed = seed * 1664525u + 1013904223u;
                amp += step;
                f += fstep;
                x = (osc(pa, ia, ta) + osc(pb, ib, tb)) >> 3;   /* +-8192 in: with resonance its state still fits 32 bits */
                x += (sine(ps) * g_sub) >> 17;
                x += (((int32_t)(seed >> 16) - 32768) * g_noise) >> 17;
                for (k = 0; k < 2; k++) {       /* Chamberlin, twice a frame */
                    low += (f * band) >> 15;
                    band += (f * (x - low - ((q * band) >> 14))) >> 15;
                }
                s = (low * amp) >> 13;
                o[2u * i] += (s * gl) >> 15;
                o[2u * i + 1u] += (s * gr) >> 15;
            }
            voice[v].ph_a = pa; voice[v].ph_b = pb; voice[v].ph_s = ps; voice[v].seed = seed;
            voice[v].amp = g;
            voice[v].f = ft; voice[v].low = low; voice[v].band = band;
        }
        if (voice[v].stage == ST_IDLE)
            voice[v].low = voice[v].band = 0;   /* silent: start the next note clean */
    }
#ifdef PROFILE
    prof_voices = nv;
#endif
}

/* ---- the FX on the pad bus (audio ISR), then the master soft clip. Their memory is in the pool */
#define CHO_LEN 2048u                           /* 46 ms a side: the longest tap is 20 ms */
static int16_t cho_buf[2][CHO_LEN] __attribute__((section(".pool")));

static int16_t sat16(int32_t x) { return (int16_t)(x > 32767 ? 32767 : x < -32767 ? -32767 : x); }

static void fx_chorus(int32_t *bus, uint32_t n)   /* a modulated tap a side, the two LFOs half a cycle apart */
{
    static uint32_t w, lfo;
    static int32_t d[2] = {309 << 8, 309 << 8};   /* the taps now, in samples Q8 */
    uint32_t b, i, c;
    int32_t mix = patch.cho_mix * 64, amp = patch.cho_depth * 577;   /* mix Q15 (50 % at 255); swing Q8 */
    if (!mix) return;                           /* (the line stops too: it fills again within 20 ms) */
    for (b = 0; b < n; b += ENV_CTRL) {
        int32_t step[2];
        lfo += 311661u + patch.cho_rate * 17111u;   /* 0.1 .. 1.5 Hz, a step every ENV_CTRL frames */
        for (c = 0; c < 2u; c++) {
            int32_t m = sine(lfo + c * 0x80000000u) + 32767;   /* 0 .. 65534 */
            int32_t target = (309 << 8) + (int32_t)(((int64_t)amp * m) >> 16);   /* 7 ms + up to 13 ms */
            step[c] = (target - d[c]) >> 5;
        }
        for (i = 0; i < ENV_CTRL; i++, w++) {
            int32_t *x = bus + 2u * (b + i);
            for (c = 0; c < 2u; c++) {
                int32_t dq, s0, s1, wet;
                uint32_t r;
                d[c] += step[c];
                dq = d[c];
                cho_buf[c][w % CHO_LEN] = sat16(x[c]);
                r = w - (uint32_t)(dq >> 8);
                s0 = cho_buf[c][r % CHO_LEN];
                s1 = cho_buf[c][(r - 1u) % CHO_LEN];
                wet = s0 + (((s1 - s0) * (dq & 255)) >> 8);
                x[c] += ((wet - x[c]) * mix) >> 15;
            }
        }
    }
}

#define DLY_LEN 18000u                          /* 408 ms a side: a 1/4 down to 148 BPM, else half of it */
static int16_t dly_buf[2][DLY_LEN] __attribute__((section(".pool")));
static int32_t dly_wet[2u * HALF_FRAMES];       /* this block's echoes, for the bus and the reverb */

static void fx_delay(const int32_t *bus, uint32_t n)   /* ping-pong: the bus into the left line, each line into the
                                                          other through the feedback and its tone */
{
    static const uint8_t TICKS[4] = {12, 18, 24, 16};
    static uint32_t w;
    static int32_t d = 15211 << 8, lp[2];       /* the length now (samples Q8), the feedback's low-pass */
#ifdef ETHEREAL_HOST
    static uint32_t logged;
#endif
    uint32_t b, i, c, target = TICKS[patch.dly_time & 3u] * TICK_PH / clk_inc;   /* frames, from the clock (24 x TICK_PH fits) */
    int32_t fb = patch.dly_fb * 85, k = 4350 + patch.dly_tone * 80, send = patch.dly_send * 128;   /* Q15: 85 %; ~1..10 kHz */
    while (target > DLY_LEN - 2u) target >>= 1;
#ifdef ETHEREAL_HOST
    if (target != logged) { logged = target; HOST_LOG("%6u ms  delay %u frames\n", fm1_ms, target); }
#endif
    for (b = 0; b < n; b += ENV_CTRL) {
        int32_t step = ((int32_t)(target << 8) - d) >> 6;   /* a tempo change glides the echoes, no jump */
        for (i = b; i < b + ENV_CTRL; i++) {
            int32_t y[2], r0, r1, s0, s1;
            d += step;
            r0 = (int32_t)w - (d >> 8);
            if (r0 < 0) r0 += DLY_LEN;
            r1 = r0 ? r0 - 1 : DLY_LEN - 1;
            for (c = 0; c < 2u; c++) {
                s0 = dly_buf[c][r0];
                s1 = dly_buf[c][r1];
                y[c] = s0 + (((s1 - s0) * (d & 255)) >> 8);
                lp[c] += ((y[c] - lp[c]) * k) >> 15;
            }
            dly_buf[0][w] = sat16(((((bus[2u * i] + bus[2u * i + 1u]) >> 1) * send) >> 15) + ((lp[1] * fb) >> 15));
            dly_buf[1][w] = sat16((lp[0] * fb) >> 15);
            dly_wet[2u * i] = y[0];
            dly_wet[2u * i + 1u] = y[1];
            if (++w == DLY_LEN) w = 0;
        }
    }
}

#define REV_LEN 2048u                           /* a line's buffer: a power of two, so a read is a mask */
#define REV_MASK (REV_LEN - 1u)
static int32_t rev_line[8][REV_LEN] __attribute__((section(".pool")));   /* Q4: in whole samples a long tail ends in
                                                                           limit cycles, not silence */
static int16_t rev_pre[REV_LEN] __attribute__((section(".pool")));
static int32_t rev_wet[2u * HALF_FRAMES];

static void fx_reverb(const int32_t *bus, uint32_t n)   /* an 8-line FDN: each line damped and scaled, the eight
                                                           mixed by a Hadamard back into all of them. It runs at
                                                           22.05 kHz, a step per two frames: half the cost */
{
    static const uint16_t LEN[8] = {967, 1103, 1229, 1361, 1451, 1601, 1723, 1847};   /* 44..84 ms, all prime */
    static uint32_t w, lfo[2] = {0, 0x80000000u};
    static int32_t lp[8], last[2];              /* lp Q4; last: the previous step's return, for the frame between */
    uint32_t b, i, j, h, len[8], t10 = 10u + patch.rev_decay * 110u / 255u, pre = patch.rev_pre * 1323u / 255u;
    int32_t g[8], k = 1024 - patch.rev_damp * 3, send = patch.rev_send * 64, mix = patch.rev_mix * 128;
    for (j = 0; j < 8u; j++) {
        uint32_t e;
        len[j] = LEN[j] * (256u + patch.rev_size) >> 9;
        e = len[j] * 296u / t10;                /* a pass loses 2^-(e / 65536): -60 dB after the decay time */
        g[j] = (int32_t)(((EXP2N[(e >> 8) & 255u] >> (e >> 16)) * 23170u + 524288u) >> 20);   /* Q12, with the 1 / sqrt 8 */
    }
    for (b = 0; b < n; b += ENV_CTRL) {
        int32_t m[2];                           /* lines 2 and 5 swing +-6 steps (Q8), against metallic ringing */
        lfo[0] += 38956u * ENV_CTRL;            /* 0.4 Hz */
        lfo[1] += 26296u * ENV_CTRL;            /* 0.27 Hz */
        m[0] = sine(lfo[0]) * 6 >> 7;
        m[1] = sine(lfo[1]) * 6 >> 7;
        for (i = b; i < b + ENV_CTRL; i += 2u, w++) {
            const int32_t *f = bus + 2u * i, *e = dly_wet + 2u * i;
            int32_t x, s[8], l, r;
            rev_pre[w & REV_MASK] = sat16((((f[0] + f[1] + f[2] + f[3] + e[0] + e[1] + e[2] + e[3]) >> 2) * send) >> 15);
            x = rev_pre[(w - pre) & REV_MASK] * 8;   /* a half, Q4 (a step for every two frames) */
            for (j = 0; j < 8u; j++) s[j] = rev_line[j][(w - len[j]) & REV_MASK];
            for (j = 0; j < 2u; j++) {
                uint32_t c = j ? 5u : 2u, d = (len[c] << 8) + (uint32_t)m[j], r0 = (w - (d >> 8)) & REV_MASK;
                int32_t s0 = rev_line[c][r0];
                s[c] = s0 + (((rev_line[c][(r0 - 1u) & REV_MASK] - s0) * (int32_t)(d & 255u)) >> 8);
            }
            l = (((s[0] - s[2] + s[4] - s[6] + 16) >> 5) * mix) >> 15;
            r = (((s[1] - s[3] + s[5] - s[7] + 16) >> 5) * mix) >> 15;
            rev_wet[2u * i] = (last[0] + l) >> 1;
            rev_wet[2u * i + 1u] = (last[1] + r) >> 1;
            rev_wet[2u * i + 2u] = last[0] = l;
            rev_wet[2u * i + 3u] = last[1] = r;
            for (j = 0; j < 8u; j++) {          /* (rounded: a shift's bias, round the loop, settles as a DC offset) */
                lp[j] += ((s[j] - lp[j]) * k + 512) >> 10;
                s[j] = (lp[j] * g[j] + 2048) >> 12;
            }
            for (h = 1; h < 8u; h <<= 1)
                for (j = 0; j < 8u; j++)
                    if (!(j & h)) { l = s[j]; s[j] = l + s[j + h]; s[j + h] = l - s[j + h]; }
            for (j = 0; j < 8u; j++) {          /* held to full scale: the damping's product stays in 32 bits */
                l = s[j] + (j & 1u ? -x : x);
                rev_line[j][w & REV_MASK] = l > 524272 ? 524272 : l < -524272 ? -524272 : l;
            }
        }
    }
}

/* ---- the sub (M4): one mono voice, dry and centred under the drums */
enum { SUB_SINE, SUB_SINE2, SUB_TRI };
typedef struct {
    uint8_t wave, glide, sat, att, rel, level;  /* glide, att, rel as time indices (0 = no glide); sat, level 0..255 */
    int8_t oct;                                 /* -2..+1, after the note */
} sub_t;
static sub_t sub_p = {SUB_SINE, 54, 38, 14, 63, 204, 0};   /* 60 ms, 15 %, 3 ms, 120 ms, 80 % */
static volatile uint32_t sub_inc;               /* main loop: the note's phase step (a legato note glides to it) */
static volatile uint8_t sub_gate, sub_trig;     /* main loop: held; bumped for a new, not legato, note */
static volatile uint8_t sub_vel = 127;          /* main loop: the next note's velocity */
static struct { uint32_t ph, inc, stage; int32_t lvl, amp, lp, gain; uint8_t trig_seen; } sub;   /* the ISR's */
#define ST_FADED (ST_FADE + 1u)                 /* the sub's: faded out, the next note may start */

static int32_t sub_wave(uint32_t ph)            /* -32767..32767, 0 at ph 0 */
{
    int32_t q;
    if (sub_p.wave == SUB_SINE2) return (sine(ph) * 3 + sine(ph * 2u)) >> 2;   /* reads on small speakers */
    if (sub_p.wave != SUB_TRI) return sine(ph);
    q = (int32_t)(ph + 0x40000000u);
    return ((q ^ (q >> 31)) >> 15) - 32768;
}

static void sub_render(int32_t *out, uint32_t n)   /* added into out after the FX */
{
    uint32_t b, i;
    int32_t drive = 4096 + sub_p.sat * 48, level = sub_p.level;   /* Q12: 1..4 x into the tanh */
    for (b = 0; b < n; b += ENV_CTRL) {
        int32_t to, da, duck, dd;
        if (sub.trig_seen != sub_trig) {
            if (sub.stage == ST_IDLE || sub.amp < 64 || sub.stage == ST_FADED) {
                sub.trig_seen = sub_trig;
                sub.ph = 0;                     /* from a zero crossing */
                sub.inc = sub_inc;
                sub.gain = 64 + sub_vel * 192 / 127;   /* Q8: 0.25 .. 1 */
                sub.lvl = 0;
                sub.stage = ST_ATT;
            } else sub.stage = ST_FADE;         /* the last note out first: 0.7 ms, no click */
        }
        if (sub.stage == ST_ATT || sub.stage == ST_DEC) {
            if (!sub_gate) sub.stage = ST_REL;
            else if (sub.stage == ST_ATT && (sub.lvl = env_to(sub.lvl, 1395864371, ATT_K[sub_p.att])) >= Q30) {
                sub.lvl = Q30;
                sub.stage = ST_DEC;             /* (held at full: no decay, no sustain level) */
            }
        }
        if (sub.stage == ST_REL && (sub.lvl = env_to(sub.lvl, 0, DEC_K[sub_p.rel])) < (Q30 >> 12)) { sub.lvl = 0; sub.stage = ST_IDLE; }
        if (sub.stage == ST_FADE) { sub.lvl = 0; to = 0; }
        else to = sub.lvl >> 15;
        if (sub.stage == ST_IDLE && !sub.amp) continue;
        if (sub.stage != ST_IDLE && sub.stage != ST_FADE && sub.stage != ST_FADED)
            sub.inc = sub_p.glide ? (uint32_t)env_to((int32_t)sub.inc, (int32_t)sub_inc, DEC_K[sub_p.glide]) : sub_inc;
        da = (to - sub.amp) / (int32_t)ENV_CTRL;
        duck = mod_duck_sub[b / ENV_CTRL];
        dd = (mod_duck_sub[b / ENV_CTRL + 1u] - duck) >> 5;
        for (i = b; i < b + ENV_CTRL; i++) {
            int32_t x, a;
            sub.ph += sub.inc;
            sub.amp += da;
            x = (((sub_wave(sub.ph) * sub.amp) >> 15) * drive) >> 12;
            a = x < 0 ? -x : x;
            a = a >= 131072 ? 32767 : TANH[a >> 7] + (((TANH[(a >> 7) + 1] - TANH[a >> 7]) * (a & 127)) >> 7);
            sub.lp += (((x < 0 ? -a : a) - sub.lp) * 1371) >> 15;   /* 300 Hz: what the saturation adds above the sub */
            x = (((((sub.lp * level) >> 8) * sub.gain) >> 8) * duck) >> 15;
            duck += dd;
            out[2u * i] += x;
            out[2u * i + 1u] += x;
        }
        sub.amp = to;
        if (sub.stage == ST_FADE) sub.stage = ST_FADED;
    }
}

/* ---- the clock's modulation (M6): LFO 1 on the cutoff, LFO 2 on the pad's level, the duck on the pad bus */
typedef struct {
    uint8_t depth, rel, sync, every, sub;       /* 0..100 %; 30..300 ms; DUCK_MS / 16 / 8 (the release instead);
                                                   1 or 2 beats; the sub's share 0..100 % */
} duck_t;
static duck_t duck_p = {0, 90, 0, 1, 0};                    /* (125 ms) */
enum { DUCK_MS, DUCK_16, DUCK_8 };

static int32_t lfo_shape(uint32_t ph, uint32_t shape)   /* -32767..32767 */
{
    int32_t q;
    if ((shape & 3u) == LFO_SINE) return sine(ph);
    if ((shape & 3u) == LFO_SQR) return ph < 0x80000000u ? 32767 : -32767;
    q = (int32_t)(ph + 0x40000000u);
    return ((q ^ (q >> 31)) >> 15) - 32768;
}

static uint32_t lfo_free_inc(uint32_t r, uint32_t top, uint32_t oct256)   /* per frame: top Hz at 255, oct256 / 256
                                                                            octaves lower at 0 */
{
    uint32_t e = (255u - r) * oct256 / 255u;
    return (uint32_t)(((uint64_t)top * EXP2N[e & 255u]) >> 16) >> (e >> 8);
}

static void mod_block(uint32_t n)               /* each control step's LFOs and duck, from the bar phase */
{
    static const uint8_t L2_PER_BAR[4] = {16, 12, 8, 4};
    static uint32_t free_ph[2], last_q, att;    /* att: the duck's steps still to go down */
    static int32_t sq, dl;                      /* LFO 2's square, smoothed; the duck, Q30 */
    uint32_t k, inc = clk_inc, step = inc * ENV_CTRL;
    uint32_t i1 = lfo_free_inc(patch.l1_rate, 194783u, 1701u), i2 = lfo_free_inc(patch.l2_rate, 973914u, 1106u);
    uint32_t dk, frames;                        /* the duck's release: k Q24 to -40 dB in that many frames */
    if (duck_p.sync == DUCK_MS) frames = (30u + duck_p.rel * 270u / 255u) * 441u / 10u;
    else frames = (duck_p.sync == DUCK_16 ? 6u : 12u) * TICK_PH / inc;
    dk = 2472456192u / frames;                  /* (32 x ln 100 x 2^24: near 1 - e^-x for a step this small) */
    mod_trem[0] = mod_trem[NSTEP];
    mod_duck[0] = mod_duck[NSTEP];
    mod_duck_sub[0] = mod_duck_sub[NSTEP];
    if (started) { free_ph[0] = free_ph[1] = 0; last_q = 4; }   /* (4: the downbeat is a new beat) */
    for (k = 0; k < n / ENV_CTRL; k++) {
        uint32_t p = bar_ph + inc * n + k * step, nb = bars + (p < bar_ph), q, ph1, ph2;   /* a block ahead: it plays
                                                   after the one playing now. (nb: bars, if this crossed one) */
        int32_t l1, l2;
        free_ph[0] += i1 * ENV_CTRL;
        free_ph[1] += i2 * ENV_CTRL;
        switch (patch.l1_rate) {
        case L1_QUARTER: ph1 = p << 2; break;
        case L1_HALF: ph1 = p << 1; break;
        case L1_BAR: ph1 = p; break;
        case L1_2BARS: ph1 = (nb & 1u) << 31 | p >> 1; break;
        default: ph1 = (nb & 3u) << 30 | p >> 2; break;
        }
        ph2 = p * L2_PER_BAR[patch.l2_rate & 3u];
        if (patch.l1_shape & LFO_FREE) ph1 = free_ph[0];
        if (patch.l2_shape & LFO_FREE) ph2 = free_ph[1];
        l1 = lfo_shape(ph1, patch.l1_shape);
        l2 = lfo_shape(ph2, patch.l2_shape);
        if ((patch.l2_shape & 3u) == LFO_SQR) l2 = sq += (l2 - sq) >> 2;   /* (3 ms: no click) */
        ui_l1 = (l1 * patch.l1_depth) >> 8;
        mod_cut[k] = (ui_l1 * 46) >> 7;         /* +-46 steps: 1.5 octaves */
        mod_trem[k + 1u] = 32768 - ((((32767 - l2) >> 1) * patch.l2_depth) >> 8);

        q = (p + step) >> 30;                   /* the beat this step ends in: the next one on ducks (a pull
                                                   back to the tick is not a new beat) */
        if (q != last_q) {
            if (clk_run && (started || q == ((last_q + 1u) & 3u)) && !(q & (duck_p.every - 1u))) att = 4;
            last_q = q;
        }
        if (att) { dl += (Q30 - dl) / (int32_t)att; att--; }   /* down in four steps, 2.9 ms: no click */
        else dl = env_to(dl, 0, dk);
        l1 = ((dl >> 15) * duck_p.depth) >> 8;
        mod_duck[k + 1u] = 32768 - l1;
        mod_duck_sub[k + 1u] = 32768 - ((l1 * duck_p.sub) >> 8);
    }
}

static void apply_gain(int32_t *bus, uint32_t n, const int32_t *g)   /* Q15, ramped between the steps' edges */
{
    uint32_t k, i;
    for (k = 0; k < n / ENV_CTRL; k++) {
        int32_t a = g[k], da = (g[k + 1u] - a) >> 5, *o = bus + 2u * k * ENV_CTRL;
        if (a == 32768 && !da) continue;
        for (i = 0; i < ENV_CTRL; i++, a += da) {
            o[2u * i] = (int32_t)(((int64_t)o[2u * i] * a) >> 15);
            o[2u * i + 1u] = (int32_t)(((int64_t)o[2u * i + 1u] * a) >> 15);
        }
    }
}

static void master_clip(int32_t *bus, uint32_t n)   /* straight to 75 %, then a soft knee to full scale */
{
    uint32_t i;
    for (i = 0; i < 2u * n; i++) {
        int32_t x = bus[i], a = x < 0 ? -x : x, y;
        if (a < 24576) continue;                /* (the table is straight there too) */
        if (a >= 65536) y = 32767;
        else y = CLIP[a >> 6] + (((CLIP[(a >> 6) + 1] - CLIP[a >> 6]) * (a & 63)) >> 6);
        bus[i] = x < 0 ? -y : y;
    }
}

static volatile uint32_t out_peak;              /* audio ISR: the last block's samples OR'd (a level, cheaply) */

#ifdef DIAG                                     /* ./build.sh --diag: what the FM-1 sees of the clock, out over USB-MIDI */
static volatile uint32_t diag_frames;           /* audio ISR: frames rendered since boot */
#endif

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i;
#ifdef PROFILE
    uint32_t t0 = fm1_ticks(), ts = t0, dt;
#endif
#ifdef DIAG
    diag_frames += n;
#endif
    clock_audio(n);
    mod_block(n);
    pad_render(out, n);
    apply_gain(out, n, mod_trem);               /* the tremolo, before the FX: they smear it */
    PROF_STAGE(prof_pad);
    fx_chorus(out, n);
    fx_delay(out, n);
    PROF_STAGE(prof_cd);
    fx_reverb(out, n);                          /* the bus and the echoes */
    PROF_STAGE(prof_rev);
    for (i = 0; i < 2u * n; i++) out[i] += dly_wet[i] + rev_wet[i];
    apply_gain(out, n, mod_duck);               /* the whole pad bus, dry and returns */
    sub_render(out, n);
    master_clip(out, n);
    {
        uint32_t peak = 0;                      /* (a save stalls the sound: only when even the tails are gone) */
        for (i = 0; i < 2u * n; i++) peak |= (uint32_t)(out[i] < 0 ? -out[i] : out[i]);
        out_peak = peak;
    }
#ifdef PROFILE
    dt = fm1_ticks() - ts;                      /* (the USB and key scan interrupts count too: they are load) */
    if (dt > prof_max) prof_max = dt;
#endif
}

static void pad_init(void)                      /* each voice its own place in the stereo field and its own drift */
{
    uint32_t v, r = 0x2545F491u;
    for (v = 0; v < NV; v++) {
        int32_t p;
        r = r * 1664525u + 1013904223u;
        p = (int32_t)(r >> 17) % 26215 - 13107; /* +-40 % */
        voice[v].gl = (VOICE_GAIN * (32768 - p)) >> 15;
        voice[v].gr = (VOICE_GAIN * (32768 + p)) >> 15;
        r = r * 1664525u + 1013904223u;
        voice[v].drift_inc = 200000u + (r >> 12) % 400000u;   /* one round in 5 to 15 s */
        voice[v].drift_ph = r;
        voice[v].ph_b = r * 7u;                 /* the two oscillators start apart */
        voice[v].seed = r ^ 0x9E3779B9u;
        voice[v].f = CUT_F[0];
    }
}

/* ---- what survives a power-off (up to 256 bytes; a new size starts from the defaults) */
#define NUSER 4                                 /* user patches, after the factory ones */
#define SET_VER 3                               /* a new layout starts from the defaults */
static struct {
    uint8_t ver;
    int8_t oct;
    uint8_t slot, split, latch;                 /* slot: 0..NPATCH-1 factory, then the user ones */
    uint16_t bpm10;
    uint8_t pad_ch, sub_ch, split_n, lost;      /* channels 0..15; keys in the sub's zone; LOST_OPT index */
    uint8_t save_to;                            /* the user slot SAVE offered last */
    patch_t edit;                               /* the sound as edited, and the sub's and duck's */
    sub_t sub;
    duck_t duck;
    patch_t user[NUSER];
} set;
_Static_assert(sizeof set <= 256, "the settings sector holds 256 bytes");
static const uint16_t LOST_OPT[4] = {250, 500, 1000, 2000};
#define NSLOT (NPATCH + NUSER)
#define PAD_CH set.pad_ch
#define SUB_CH set.sub_ch

static void slot_load(uint32_t slot)            /* the sound, and what power-off keeps of it */
{
    patch = slot < NPATCH ? PATCHES[slot] : set.user[slot - NPATCH];
    set.edit = patch;
}

static void settings_defaults(void)
{
    uint32_t i;
    memset(&set, 0, sizeof set);
    set.ver = SET_VER;
    set.bpm10 = 1740;
    set.split = 1;
    set.pad_ch = 0;
    set.sub_ch = 1;
    set.split_n = 12;
    set.edit = PATCHES[0];
    set.sub = sub_p;
    set.duck = duck_p;
    for (i = 0; i < NUSER; i++) {               /* the user slots start as copies of the factory patches */
        set.user[i] = PATCHES[i % NPATCH];
        memcpy(set.user[i].name, "USER1\0\0", 8);
        set.user[i].name[4] = (char)('1' + i);
    }
}

static void settings_check(void)                /* (the CRC holds the bytes; this keeps an old layout out) */
{
    if (set.ver == 2) { if (set.slot >= 4) set.slot++; set.ver = 3; }   /* 2: four factory patches, before STAB */
    if (set.ver != SET_VER) settings_defaults();
    if (set.oct < -2 || set.oct > 2) set.oct = 0;
    if (set.bpm10 < 600u || set.bpm10 > 2000u) set.bpm10 = 1740;
    if (set.slot >= NSLOT) set.slot = 0;
    if (set.split_n < 1u || set.split_n > 26u) set.split_n = 12;
    if (set.save_to >= NUSER) set.save_to = 0;
    set.split = set.split != 0;
    set.latch = set.latch != 0;
    set.pad_ch &= 15u;
    set.sub_ch &= 15u;
    set.lost &= 3u;
}

/* ---- notes (main loop) */
static const uint32_t INC_C9[12] = {            /* MIDI 120..131 (C9..B9); lower octaves shift right */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
static uint32_t ages;

static uint8_t sustain;                         /* CC 64 down */

static uint8_t sub_held[16], sub_n;             /* the sub's held notes, the last on top */
static uint8_t sub_shown;                       /* the note the sub plays or last played (the sky's teal star) */

static uint32_t note_inc(int32_t note) { note = note < 0 ? 0 : note > 127 ? 127 : note; return INC_C9[note % 12] >> (10 - note / 12); }
static void sub_drop(uint32_t note)
{
    uint32_t i, j;
    for (i = j = 0; i < sub_n; i++)
        if (sub_held[i] != note) sub_held[j++] = sub_held[i];
    sub_n = (uint8_t)j;
}
static void sub_on(uint32_t note, uint32_t vel)   /* mono, the last note wins; with one held already it is legato */
{
    uint32_t legato;
    sub_drop(note);
    legato = sub_n != 0;
    if (sub_n == sizeof sub_held) sub_drop(sub_held[0]);   /* (no libc: the oldest goes) */
    sub_held[sub_n++] = (uint8_t)note;
    HOST_LOG("%6u ms  sub %u%s\n", fm1_ms, note, legato ? " (legato)" : "");
    sub_inc = note_inc((int32_t)note + 12 * sub_p.oct);
    sub_shown = (uint8_t)note;
    sub_gate = 1;
    if (!legato) { sub_vel = (uint8_t)vel; sub_trig++; }   /* last: the ISR starts the note when it sees this */
}
static void sub_off(uint32_t note)              /* back to the note held before, legato */
{
    sub_drop(note);
    if (sub_n) { sub_inc = note_inc((int32_t)sub_held[sub_n - 1u] + 12 * sub_p.oct); sub_shown = sub_held[sub_n - 1u]; }
    else sub_gate = 0;
}


static void play(uint32_t note, uint32_t vel)   /* the same note again, else a free voice, else the quietest
                                                   released one, else the oldest held: it fades out first */
{
    uint32_t v, pick = NV, best = 0xFFFFFFFFu;
    const char *why = "";
    if (note > 127u || !vel) return;
    for (v = 0; v < NV && pick == NV; v++)
        if (voice[v].note == note && (voice[v].gate || voice[v].stage != ST_IDLE)) { pick = v; why = "again"; }
    for (v = 0; v < NV && pick == NV; v++)
        if (!voice[v].gate && voice[v].stage == ST_IDLE && voice[v].trig == voice[v].trig_seen) { pick = v; why = "free"; }
    if (pick == NV)
        for (v = 0; v < NV; v++)
            if (!voice[v].gate && (uint32_t)voice[v].lvl < best) { best = (uint32_t)voice[v].lvl; pick = v; why = "released"; }
    if (pick == NV)
        for (v = 0, best = 0xFFFFFFFFu; v < NV; v++)
            if (voice[v].age < best) { best = voice[v].age; pick = v; why = "held"; }
    HOST_LOG("%6u ms  note %u -> voice %u (%s)\n", fm1_ms, note, pick, why);
    voice[pick].note = (uint8_t)note;
    voice[pick].age = ++ages;
    voice[pick].pedal = 0;
    voice[pick].p_inc = INC_C9[note % 12u] >> (10u - note / 12u);
    voice[pick].p_vel = (uint8_t)vel;
    voice[pick].gate = 1;
    voice[pick].trig++;                         /* last: the ISR takes the note when it sees this */
}
static void stop(uint32_t note)
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (voice[v].note == note && voice[v].gate && !voice[v].pedal) {
            if (sustain) voice[v].pedal = 1;
            else voice[v].gate = 0;
        }
}
static void pedal_up(void)
{
    uint32_t v;
    sustain = 0;
    for (v = 0; v < NV; v++)
        if (voice[v].pedal) { voice[v].pedal = 0; voice[v].gate = 0; }
}
static int knob_cc(uint32_t cc, uint32_t val);  /* (the pages, below) */
static void pad_cc(uint32_t ch, uint32_t cc, uint32_t val)
{
    if (knob_cc(cc, val)) return;
    if (cc == 91u) patch.rev_send = (uint8_t)(val * 2u + (val >> 6));   /* 0..127 -> 0..255 */
    else if (cc == 123u && ch == SUB_CH) { sub_n = 0; sub_gate = 0; }
    else if (cc == 64u && ch == PAD_CH) { if (val >= 64u) sustain = 1; else pedal_up(); }
    else if (cc == 123u && ch == PAD_CH) {      /* all notes off: what a sequencer sends at stop */
        uint32_t v;
        pedal_up();
        for (v = 0; v < NV; v++) voice[v].gate = 0;
    }
}

static int quiet(void)                          /* nothing sounds, not even a release */
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (voice[v].gate || voice[v].stage != ST_IDLE) return 0;
    return !sub_gate && sub.stage == ST_IDLE;
}

#define KEY0 53u                                /* key 0 = F3 */

/* ---- the pages (M7): the page buttons pick what KNOB1..4 edit; HOME is the one to play with */
enum { PG_HOME, PG_EDIT, PG_ENV, PG_LFO, PG_FX, PG_GLO, PG_SET, NPAGE };   /* PG_SET: SELECT turned */
#define NPAGE_BTN PG_SET
enum { P_CUTOFF, P_REVMIX, P_DUCK, P_SUBLVL,  P_OSC, P_DETUNE, P_NOISE, P_SUBOCT,  P_ATT, P_REL, P_FENV, P_FDEC,
       P_L1RATE, P_L1DEPTH, P_L2RATE, P_L2DEPTH,  P_CHO, P_DLYTIME, P_DLYFB, P_REVDEC,
       P_SUBWAVE, P_GLIDE, P_SAT, P_DUCKREL,  P_PADCH, P_SUBCH, P_SPLIT, P_LOST };   /* four a page, in page order */
enum { F_PCT, F_HZ, F_TIME, F_GLIDE, F_CENTS, F_BIPCT, F_FB, F_DECAY, F_DUCKREL, F_LIST, F_CH, F_KEY };
typedef struct { const char *label; uint8_t fmt; int16_t lo, hi; const char *const *opts; } param_t;
static const char *const O_OSC[] = {"SAW SAW", "SAW TRI", "TRI SAW", "TRI TRI"};
static const char *const O_L1[] = {"1/4 BAR", "1/2 BAR", "1 BAR", "2 BARS", "4 BARS"};
static const char *const O_L2[] = {"1/16", "1/8T", "1/8", "1/4"};
static const char *const O_DLY[] = {"1/8", "1/8 DOT", "1/4", "1/4T"};
static const char *const O_WAVE[] = {"SINE", "SINE+2", "TRI"};
static const char *const O_DREL[] = {"1/16", "1/8"};
static const char *const O_LOST[] = {"250 MS", "500 MS", "1 S", "2 S"};
static const param_t PARAMS[] = {
    {"CUTOFF", F_HZ, 0, 255, 0},      {"REVERB", F_PCT, 0, 255, 0},    {"DUCK", F_PCT, 0, 255, 0},      {"SUB LVL", F_PCT, 0, 255, 0},
    {"OSC", F_LIST, 0, 3, O_OSC},     {"DETUNE", F_CENTS, 0, 25, 0},   {"NOISE", F_PCT, 0, 255, 0},     {"SUB OCT", F_PCT, 0, 255, 0},
    {"ATTACK", F_TIME, 0, 127, 0},    {"RELEASE", F_TIME, 0, 127, 0},  {"FLT ENV", F_BIPCT, -127, 127, 0}, {"FLT DEC", F_TIME, 0, 127, 0},
    {"LFO1", F_LIST, 0, 4, O_L1},     {"LFO1 AMT", F_PCT, 0, 255, 0},  {"LFO2", F_LIST, 0, 3, O_L2},    {"LFO2 AMT", F_PCT, 0, 255, 0},
    {"CHORUS", F_PCT, 0, 255, 0},     {"DELAY", F_LIST, 0, 3, O_DLY},  {"FEEDBACK", F_FB, 0, 255, 0},   {"DECAY", F_DECAY, 0, 255, 0},
    {"SUB WAVE", F_LIST, 0, 2, O_WAVE}, {"GLIDE", F_GLIDE, 0, 82, 0},  {"SATURATE", F_PCT, 0, 255, 0},  {"DUCK REL", F_DUCKREL, 0, 257, 0},
    {"PAD CH", F_CH, 0, 15, 0},       {"SUB CH", F_CH, 0, 15, 0},      {"SUB UPTO", F_KEY, 1, 26, 0},   {"NO CLOCK", F_LIST, 0, 3, O_LOST},
};
static const char *const PAGE_NAME[NPAGE] = {"PERFORM", "PAD OSC", "ENVELOPES", "MODULATION", "EFFECTS", "SUB + DUCK",
                                             "SETTINGS"};
static const uint8_t PAGE_BTN[NPAGE_BTN] = {BTN_HOME, BTN_EDIT, BTN_ENV, BTN_LFO, BTN_FX, BTN_GLO};

static int32_t param_get(uint32_t id)
{
    switch (id) {
    case P_CUTOFF: return patch.cutoff;
    case P_REVMIX: return patch.rev_mix;
    case P_DUCK: return duck_p.depth;
    case P_SUBLVL: return sub_p.level;
    case P_OSC: return (patch.osc_a == OSC_TRI) * 2 + (patch.osc_b == OSC_TRI);
    case P_DETUNE: return patch.detune;
    case P_NOISE: return patch.noise;
    case P_SUBOCT: return patch.sub;
    case P_ATT: return patch.att;
    case P_REL: return patch.rel;
    case P_FENV: return patch.fenv;
    case P_FDEC: return patch.fdec;
    case P_L1RATE: return patch.l1_rate > L1_4BARS ? L1_4BARS : patch.l1_rate;
    case P_L1DEPTH: return patch.l1_depth;
    case P_L2RATE: return patch.l2_rate & 3u;
    case P_L2DEPTH: return patch.l2_depth;
    case P_CHO: return patch.cho_depth;
    case P_DLYTIME: return patch.dly_time & 3u;
    case P_DLYFB: return patch.dly_fb;
    case P_REVDEC: return patch.rev_decay;
    case P_SUBWAVE: return sub_p.wave;
    case P_GLIDE: return sub_p.glide;
    case P_SAT: return sub_p.sat;
    case P_PADCH: return set.pad_ch;
    case P_SUBCH: return set.sub_ch;
    case P_SPLIT: return set.split_n;
    case P_LOST: return set.lost;
    default: return duck_p.sync == DUCK_MS ? 2 + duck_p.rel : duck_p.sync - 1;   /* 0 1/16, 1 1/8, then the ms */
    }
}

static void sound_set(patch_t *p, sub_t *sp, duck_t *dp, uint32_t id, int32_t v)   /* the knobs that are the sound */
{
    uint8_t u = (uint8_t)v;
    switch (id) {
    case P_CUTOFF: p->cutoff = u; break;
    case P_REVMIX: p->rev_mix = u; break;
    case P_DUCK: dp->depth = u; break;
    case P_SUBLVL: sp->level = u; break;
    case P_OSC: p->osc_a = v >> 1 ? OSC_TRI : OSC_SAW; p->osc_b = v & 1 ? OSC_TRI : OSC_SAW; break;
    case P_DETUNE: p->detune = u; break;
    case P_NOISE: p->noise = u; break;
    case P_SUBOCT: p->sub = u; break;
    case P_ATT: p->att = u; break;
    case P_REL: p->rel = u; break;
    case P_FENV: p->fenv = (int8_t)v; break;
    case P_FDEC: p->fdec = u; break;
    case P_L1RATE: p->l1_rate = u; break;
    case P_L1DEPTH: p->l1_depth = u; break;
    case P_L2RATE: p->l2_rate = u; break;
    case P_L2DEPTH: p->l2_depth = u; break;
    case P_CHO: p->cho_depth = u; break;
    case P_DLYTIME: p->dly_time = u; break;
    case P_DLYFB: p->dly_fb = u; break;
    case P_REVDEC: p->rev_decay = u; break;
    case P_SUBWAVE: sp->wave = u; break;
    case P_GLIDE: sp->glide = u; break;
    case P_SAT: sp->sat = u; break;
    default:
        if (v < 2) dp->sync = (uint8_t)(v + 1);
        else { dp->rel = (uint8_t)(v - 2); dp->sync = DUCK_MS; }
        break;
    }
}


static void param_set(uint32_t id, int32_t v)   /* (a byte each: the ISR reads them whole) */
{
    uint8_t u;
    v = v < PARAMS[id].lo ? PARAMS[id].lo : v > PARAMS[id].hi ? PARAMS[id].hi : v;
    u = (uint8_t)v;
    switch (id) {
    case P_PADCH: case P_SUBCH:                 /* a new channel: what the old one started stops, here and downstream */
        if (u != (id == P_PADCH ? set.pad_ch : set.sub_ch)) {
            midi_send(0xB0 | set.pad_ch, 123, 0);
            midi_send(0xB0 | set.sub_ch, 123, 0);
            pad_cc(set.pad_ch, 123, 0);
            pad_cc(set.sub_ch, 123, 0);
            HOST_LOG("%6u ms  all notes off\n", fm1_ms);
        }
        if (id == P_PADCH) set.pad_ch = u; else set.sub_ch = u;
        break;
    case P_SPLIT: set.split_n = u; break;
    case P_LOST: set.lost = u; lost_ms = LOST_OPT[u]; break;
    default: sound_set(&patch, &sub_p, &duck_p, id, v); break;
    }
}

/* CC 20..43 turn the 24 knobs in page order; 74, 1, 93, 94, 5, 73, 72 are the usual names for some of them */
#define CC_KNOB0 20u
static const uint8_t CC_ALIAS[][2] = {{74, P_CUTOFF}, {1, P_L1DEPTH}, {93, P_CHO}, {94, P_DUCK}, {5, P_GLIDE},
                                      {73, P_ATT}, {72, P_REL}};
static int knob_cc(uint32_t cc, uint32_t val)
{
    uint32_t i, id = cc - CC_KNOB0;
    for (i = 0; i < sizeof CC_ALIAS / sizeof CC_ALIAS[0]; i++)
        if (CC_ALIAS[i][0] == cc) id = CC_ALIAS[i][1];
    if (id >= P_PADCH) return 0;
    param_set(id, PARAMS[id].lo + ((int32_t)val * (PARAMS[id].hi - PARAMS[id].lo) + 63) / 127);
    HOST_LOG("%6u ms  cc %u %s %d\n", fm1_ms, cc, PARAMS[id].label, param_get(id));
    return 1;
}

static char *put_num(char *s, uint32_t v, uint32_t tenths)   /* v in decimal; with tenths, v / 10 and a point */
{
    char d[12];
    uint32_t n = 0;
    do { d[n++] = (char)('0' + v % 10u); v /= 10u; } while (v || (tenths && n < 2u));
    while (n) {
        *s++ = d[--n];
        if (tenths && n == 1u) *s++ = '.';
    }
    *s = 0;
    return s;
}
static char *put_str(char *s, const char *t) { while (*t) *s++ = *t++; *s = 0; return s; }
static char *put_ms(char *s, uint32_t ms)       /* 12 MS, 1.2 S, 12 S */
{
    if (ms < 1000u) return put_str(put_num(s, ms, 0), " MS");
    if (ms < 10000u) return put_str(put_num(s, (ms + 50u) / 100u, 1), " S");
    return put_str(put_num(s, (ms + 500u) / 1000u, 0), " S");
}

static void param_text(uint32_t id, int32_t v, char *s)
{
    switch (PARAMS[id].fmt) {
    case F_PCT: put_str(put_num(s, (uint32_t)v * 100u / 255u, 0), "%"); break;
    case F_HZ:
        if (CUT_HZ[v] >= 1000u) put_str(put_num(s, (CUT_HZ[v] + 50u) / 100u, 1), " KHZ");
        else put_str(put_num(s, CUT_HZ[v], 0), " HZ");
        break;
    case F_TIME: put_ms(s, TIME_MS[v]); break;
    case F_GLIDE: if (v) put_ms(s, TIME_MS[v]); else put_str(s, "OFF"); break;
    case F_CENTS: put_str(put_num(s, (uint32_t)v, 0), " CENT"); break;
    case F_BIPCT: *s = v < 0 ? '-' : '+'; put_str(put_num(s + 1, (uint32_t)(v < 0 ? -v : v) * 100u / 127u, 0), "%"); break;
    case F_FB: put_str(put_num(s, (uint32_t)v * 85u / 255u, 0), "%"); break;
    case F_DECAY: put_str(put_num(s, 10u + (uint32_t)v * 110u / 255u, 1), " S"); break;
    case F_DUCKREL: if (v < 2) put_str(s, O_DREL[v]); else put_ms(s, 30u + (uint32_t)(v - 2) * 270u / 255u); break;
    case F_CH: put_num(s, (uint32_t)v + 1u, 0); break;
    case F_KEY: {                               /* the zone's top key, at octave 0 */
        static const char NAMES[] = "C C#D D#E F F#G G#A A#B ";
        uint32_t n = KEY0 + (uint32_t)v - 1u;
        s[0] = NAMES[2u * (n % 12u)];
        s[1] = NAMES[2u * (n % 12u) + 1u];
        put_num(s + (s[1] == ' ' ? 1 : 2), n / 12u - 1u, 0);
        break;
    }
    default: put_str(s, PARAMS[id].opts[v]); break;
    }
}

/* ---- the screen (the Sky theme): the patch and the clock along the top; a night sky where an aurora sways with
 * LFO 1 and dims with the duck, and each sounding note is a star (a chord draws its own constellation, the sub a teal
 * star); then the four knobs. Nothing waits on the LCD: one part is drawn a pass, when the last transfer is done */
#define C_ACC RGB(180, 140, 255)                /* lilac, as the logo */
#define C_TXT RGB(150, 140, 200)                /* labels */
#define C_VAL RGB(236, 232, 255)                /* values */
#define C_OFF RGB(104, 96, 150)                 /* quiet text */
#define C_RULE RGB(52, 44, 90)                  /* hairlines, tracks */
#define C_GLOW RGB(92, 64, 156)                 /* a bar's halo, then its body, then its core */
#define C_BODY RGB(150, 116, 236)
#define C_CORE RGB(228, 214, 255)
#define C_SUB RGB(110, 225, 215)
#define C_STAR RGB(235, 222, 255)
#define C_LINK RGB(120, 100, 190)
#define SKY_Y 30
#define SKY_H 120
#define SKY_LOW 24                              /* the sky's rows from here (aurora, text) go out every frame; above
                                                   them only when the stars change */
#define Y_ROW 152                               /* the first knob's row; 22 px each */
#define ROW_H 22

static struct { uint16_t *px; int32_t w, h; } sf;   /* where the blends draw: the canvas or the sky */
static void sf_cv(void) { sf.px = cv_px; sf.w = (int32_t)cv_w; sf.h = (int32_t)cv_h; }

static void sf_mist(int32_t y0)                 /* the background, (4, 4, 16) at the top to (30, 14, 50) at the bottom */
{
    static const uint8_t BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    int32_t x, y;
    for (y = 0; y < sf.h; y++) {
        uint32_t t = (uint32_t)(y0 + y), r = (4u << 8) + 26u * 256u * t / 239u, g = (4u << 8) + 10u * 256u * t / 239u,
                 b = (16u << 8) + 34u * 256u * t / 239u;
        for (x = 0; x < sf.w; x++) {            /* (ordered dither: 16-bit colour has few shades this dark) */
            uint32_t d = BAYER[t % 4u * 4u + (uint32_t)x % 4u];
            sf.px[y * sf.w + x] = swap16(((r + d * 128u) >> 11) << 11 | ((g + d * 64u) >> 10) << 5 | ((b + d * 128u) >> 11));
        }
    }
}
static void sf_blend(int32_t x, int32_t y, uint16_t c, uint32_t a)   /* c over what is there, a of 15 */
{
    uint32_t d, r, g, b;
    if ((uint32_t)x >= (uint32_t)sf.w || (uint32_t)y >= (uint32_t)sf.h) return;
    d = swap16(sf.px[y * sf.w + x]);
    r = ((d >> 11) * (15u - a) + (c >> 11u) * a) / 15u;
    g = (((d >> 5) & 63u) * (15u - a) + ((c >> 5) & 63u) * a) / 15u;
    b = ((d & 31u) * (15u - a) + (c & 31u) * a) / 15u;
    sf.px[y * sf.w + x] = swap16(r << 11 | g << 5 | b);
}
static void sf_add(int32_t x, int32_t y, uint16_t c, uint32_t a)     /* light added: a of 16 */
{
    uint32_t d, r, g, b;
    if ((uint32_t)x >= (uint32_t)sf.w || (uint32_t)y >= (uint32_t)sf.h) return;
    d = swap16(sf.px[y * sf.w + x]);
    r = (d >> 11) + (((c >> 11u) * a) >> 4);
    g = ((d >> 5) & 63u) + ((((c >> 5) & 63u) * a) >> 4);
    b = (d & 31u) + (((c & 31u) * a) >> 4);
    sf.px[y * sf.w + x] = swap16((r > 31u ? 31u : r) << 11 | (g > 63u ? 63u : g) << 5 | (b > 31u ? 31u : b));
}
static int32_t text_ws(const char *s, int32_t sp) { int32_t n = 0; const char *t = s; while (*t) t++, n++; return text_w(&FONT_S, s) + (n ? sp * (n - 1) : 0); }
static int32_t sf_text(int32_t x, int32_t y, const char *s, uint16_t c, int32_t sp)   /* letter spaced by sp; edges
                                                                                  blended over what is there */
{
    for (; *s; s++) {
        uint32_t gi = glyph(&FONT_S, (uint8_t)*s), gx, gy, w = FONT_S.bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = FONT_S.data + FONT_S.off[gi];
        for (gy = 0; gy < FONT_S.h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy * bpr + gx / 2u];
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v) sf_blend(x - FONT_S.pad + (int32_t)gx, y + (int32_t)gy, c, v);
            }
        x += FONT_S.adv[gi] + sp;
    }
    return x;
}

#define NSTARS 64
static void sf_stars(int32_t y0)                /* the fixed stars on this surface (its top is screen row y0) */
{
    uint32_t i;
    for (i = 0; i < NSTARS; i++) {
        uint32_t h = (i + 1u) * 2654435761u, b;
        int32_t x, y;
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        x = (int32_t)(h % 240u);
        y = (int32_t)((h >> 8) % (uint32_t)(SKY_Y + SKY_H - 4)) - y0;
        b = 3u + (h >> 20) % 9u;
        sf_add(x, y, C_STAR, b);
        if (b > 9u) { sf_add(x - 1, y, C_STAR, b / 4u); sf_add(x + 1, y, C_STAR, b / 4u); sf_add(x, y - 1, C_STAR, b / 4u); sf_add(x, y + 1, C_STAR, b / 4u); }
    }
}
static void sf_star(int32_t x, int32_t y, uint16_t c, uint32_t b, int32_t big)   /* a glowing point, b 0..16 */
{
    int32_t i, j;
    for (j = -4; j <= 4; j++)
        for (i = -4; i <= 4; i++) {
            int32_t d = i * i + j * j;
            uint32_t a = d <= big ? b : d <= 2 + big ? b * 2u / 3u : d <= 5 + big ? b / 3u : d <= 10 + big ? b / 6u : d <= 17 ? b / 12u : 0u;
            if (a) sf_add(x + i, y + j, c, a);
        }
}
static void sf_link(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t a)   /* a faint line between stars */
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1, dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy, n = 600;
    while (n--) {
        int32_t e2 = 2 * err;
        sf_add(x0, y0, C_LINK, a);
        if (x0 == x1 && y0 == y1) break;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static uint16_t sky_bg[240u * SKY_H] __attribute__((section(".pool")));   /* the empty sky, drawn once */
static uint16_t sky_px[240u * SKY_H] __attribute__((section(".pool")));   /* this frame (the LCD reads it) */
static void sky_init(void) { sf.px = sky_bg; sf.w = 240; sf.h = SKY_H; sf_mist(SKY_Y); sf_stars(SKY_Y); }

static void aurora(uint32_t tp, int32_t sway, uint32_t bright)   /* three ribbons; tp a phase that turns once a bar
                                                                   (or every 4 s); bright 0..16 */
{
    static const struct { uint16_t c; uint8_t base, amp, depth; uint32_t len, mul; } R[3] = {
        {RGB(180, 140, 255), 76, 10, 30, 200, 1}, {RGB(110, 225, 215), 84, 7, 22, 140, 2}, {RGB(240, 170, 230), 68, 5, 18, 260, 1}};
    uint32_t r;
    int32_t x, dy;
    for (r = 0; r < 3u; r++) {
        uint32_t kx = 0xFFFFFFFFu / R[r].len, ph = tp * R[r].mul + (uint32_t)(sway << 14) * (r == 1 ? 0xFFFFFFFFu : 1u);
        int32_t amp = R[r].amp + ((sway < 0 ? -sway : sway) >> 13), depth = R[r].depth;
        for (x = 0; x < 240; x++) {
            int32_t y = R[r].base + ((amp * sine((uint32_t)x * kx + ph)) >> 15) + ((3 * sine((uint32_t)x * kx * 3u - ph * 2u)) >> 15);
            sf_add(x, y, R[r].c, bright * 9u / 16u);
            sf_add(x, y + 1, R[r].c, bright / 4u);
            for (dy = 1; dy < depth; dy++) sf_add(x, y - dy, R[r].c, bright * (uint32_t)(depth - dy) / (uint32_t)(depth * 3));
        }
    }
}

static void star_xy(uint32_t note, int32_t *x, int32_t *y)   /* x by pitch; height by the note's place in the circle
                                                               of fifths, so a chord keeps its shape in any key */
{
    int32_t n = note < 28u ? 28 : note > 96u ? 96 : (int32_t)note;
    *x = 12 + (n - 28) * 216 / 68;
    *y = 12 + (int32_t)(note % 12u * 7u % 12u) * 4 + (int32_t)(note / 12u & 1u) * 3;
}

#ifdef PROFILE
static uint32_t sky_us;                         /* the last frame's drawing time */
#endif
static uint32_t sky_frame(const char *left, uint16_t left_c, const char *right)   /* draws and sends the sky; 1 if
                                                                                  it sent all of it */
{
    static uint32_t sig_sent;
    uint8_t nt[NV], br[NV];
    uint32_t v, n = 0, sig = 2166136261u, bright, tp, full;
    int32_t x, y, px = 0, py = 0;
#ifdef PROFILE
    uint32_t t0 = fm1_ticks();
#endif
    for (v = 0; v < NV; v++)                    /* the stars: each sounding voice, its brightness its envelope */
        if (voice[v].stage != ST_IDLE && voice[v].lvl > 0) {
            uint32_t b = (uint32_t)(voice[v].lvl >> 25), k = n++;
            for (; k && nt[k - 1u] > voice[v].note; k--) { nt[k] = nt[k - 1u]; br[k] = br[k - 1u]; }
            nt[k] = voice[v].note;
            br[k] = (uint8_t)(b < 3u ? 3u : b > 16u ? 16u : b);
        }
    for (v = 0; v < n; v++) sig = (sig ^ (nt[v] | (uint32_t)(br[v] >> 1) << 8)) * 16777619u;
    if (sub.stage != ST_IDLE && sub.lvl > 0) sig = (sig ^ (1u << 16 | sub_shown | (uint32_t)(sub.lvl >> 27) << 8)) * 16777619u;
    full = sig != sig_sent;
    sig_sent = sig;

    lcd_sync();                                 /* (lcd_idle said so: no wait) */
    memcpy(sky_px, sky_bg, sizeof sky_px);
    sf.px = sky_px; sf.w = 240; sf.h = SKY_H;
    tp = clk_run ? bar_ph : fm1_ms * 1073742u;  /* a bar, or 4 s */
    bright = 4u + (uint32_t)mod_duck[NSTEP] * 12u / 32768u;
    aurora(tp + fm1_ms * 53687u, ui_l1, bright > 16u ? 16u : bright);
    for (v = 0; v < n; v++) {                   /* a chord: joined in pitch order, and closed */
        star_xy(nt[v], &x, &y);
        if (v) sf_link(px, py, x, y, (uint32_t)(br[v] < br[v - 1u] ? br[v] : br[v - 1u]) / 2u);
        px = x; py = y;
    }
    if (n > 2u) { star_xy(nt[0], &x, &y); sf_link(px, py, x, y, br[0] / 3u); }
    for (v = 0; v < n; v++) { star_xy(nt[v], &x, &y); sf_star(x, y, C_STAR, br[v], 1); }
    if (sub.stage != ST_IDLE && sub.lvl > 0) {
        uint32_t b = (uint32_t)(sub.lvl >> 25);
        star_xy(sub_shown + 24u, &x, &y);   /* (two octaves up: the sub's range across the sky) */
        sf_star(x, 66, C_SUB, b > 16u ? 16u : b, 3);
    }
    sf_text(10, SKY_H - 19, left, left_c, 2);
    sf_text(230 - text_ws(right, 1), SKY_H - 19, right, C_OFF, 1);
#ifdef PROFILE
    sky_us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
#endif
    if (full) lcd_blit(0, SKY_Y, 240, SKY_H, sky_px);
    else lcd_blit(0, SKY_Y + SKY_LOW, 240, SKY_H - SKY_LOW, sky_px + 240u * SKY_LOW);
    return full;
}

static void draw_top(uint32_t ext, uint32_t bpm10, uint32_t beat, uint32_t running)
{
    char s[12] = "INT 000.0";
    uint32_t i;
    if (ext) { s[0] = 'E'; s[1] = 'X'; s[2] = 'T'; }
    s[4] = bpm10 >= 1000u ? (char)('0' + bpm10 / 1000u) : ' ';
    s[5] = (char)('0' + bpm10 / 100u % 10u);
    s[6] = (char)('0' + bpm10 / 10u % 10u);
    s[8] = (char)('0' + bpm10 % 10u);
    cv_begin(240, SKY_Y, 0);
    sf_cv();
    sf_mist(0);
    sf_stars(0);
    sf_text(10, 6, patch.name, C_VAL, 4);
    sf_text(230 - text_ws(s, 0), 2, s, ext ? C_TXT : C_OFF, 0);
    for (i = 0; i < 4u; i++) {                  /* the beat: a small star, the others specks */
        int32_t x = 203 + (int32_t)i * 8;
        if (running && i == beat) sf_star(x, 24, C_ACC, 14, 0);
        else sf_add(x, 24, C_TXT, 16);
    }
    cv_blit(0, 0);
}

static void sf_glow(int32_t x, int32_t w)       /* a lit hairline: core, body, halo, soft ends */
{
    int32_t i;
    if (w <= 0) return;
    cv_rect(x, 11, w, 1, C_CORE);
    for (i = x; i < x + w; i++) {
        sf_add(i, 10, C_BODY, 7); sf_add(i, 12, C_BODY, 7);
        sf_add(i, 9, C_GLOW, 5); sf_add(i, 13, C_GLOW, 5);
    }
    sf_add(x - 1, 11, C_BODY, 6); sf_add(x + w, 11, C_BODY, 6);
}

static void draw_row(uint32_t r, uint32_t id, int32_t v)   /* KNOB r + 1: its name, a hairline lit to the value, the value */
{
    const param_t *p = &PARAMS[id];
    char s[16];
    int32_t w = 76, x0 = 92, span = p->hi - p->lo;
    cv_begin(240, ROW_H, 0);
    sf_cv();
    sf_mist(Y_ROW + ROW_H * (int32_t)r);
    sf_text(10, 3, p->label, C_TXT, 1);
    cv_rect(x0, 11, w, 1, C_RULE);
    if (p->lo < 0) {                            /* bipolar: from the middle */
        int32_t m = x0 + w / 2, d = v * (w / 2) / p->hi;
        sf_glow(d < 0 ? m + d : m, d < 0 ? -d : d);
        cv_rect(m, 7, 1, 9, C_OFF);
    } else
        sf_glow(x0, (v - p->lo) * w / (span ? span : 1));
    param_text(id, v, s);
    sf_text(230 - text_ws(s, 0), 3, s, C_VAL, 0);
    cv_blit(0, (uint32_t)(Y_ROW + ROW_H * (int32_t)r));
}

#ifdef ETHEREAL_HOST
static void led_glow(uint32_t id, int on) { (void)id; (void)on; }
#else
static void led_glow(uint32_t id, int on)       /* the dim layer (fm1_led_dim), as fm1_led_key */
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                if (on) fm1_led_dim[p] |= (uint8_t)(1u << r);
                else fm1_led_dim[p] &= (uint8_t)~(1u << r);
            }
}
#endif

static void app_main(void)
{
    static __typeof__(set) last;                /* the settings as last seen: a change restarts the save's wait */
    uint32_t prev = 0, k, t_change = 0, dirty = 0, on_sub = 0, top_drawn = 0, clk_drawn = ~0u, t_clk = 0, beat_seen = 9;
    uint32_t page = PG_HOME, page_drawn = ~0u, t_knob[4] = {0}, saving = 0, latched = 0, t_sky = 0;
    int32_t row_drawn[4];
    uint8_t held[27] = {0};                     /* the note each key plays */
    if (settings_load(&set, sizeof set)) settings_defaults();
    settings_check();
    patch = set.edit;
    sub_p = set.sub;
    duck_p = set.duck;
    lost_ms = LOST_OPT[set.lost];
    last = set;
    clock_set_bpm10(set.bpm10);
    pad_init();
    clk.last = fm1_ms << 8;

    for (k = 0; k < 240u; k += 48u) {           /* the background, once; the parts draw over it */
        cv_begin(240, 48, 0);
        sf_cv();
        sf_mist((int32_t)k);
        if (k == 144u) cv_rect(0, 150 - 144, 240, 1, C_RULE);   /* under the sky */
        cv_blit(0, k);
    }
    sky_init();

    for (;;) {
        uint32_t keys, down, up, rel, b, lit = 0, sub_lit = 0, base = (uint32_t)(KEY0 + 12 * set.oct), was_ext = clk.ext, c;
        int32_t e;
        uint8_t st, d1, d2;
        platform_poll();

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        c = set.split ? ~0u << set.split_n : ~0u;   /* the pad's keys */
        if (latched && down & c && !(keys & ~down & c)) {
            for (k = 0; k < 27u; k++)           /* a chord after all were let go: the latched one stops */
                if (latched >> k & 1u) {
                    HOST_LOG("%6u ms  unlatch %u\n", fm1_ms, held[k]);
                    stop(held[k]);
                    midi_send(0x80 | PAD_CH, held[k], 0);
                }
            latched = 0;
        }
        for (k = 0; k < 27u; k++) {             /* (a key stops the note it started: the octave or split may change meanwhile) */
            if (down >> k & 1u) {
                if (latched >> k & 1u) { stop(held[k]); midi_send(0x80 | PAD_CH, held[k], 0); latched &= ~(1u << k); }
                if (set.split && k < set.split_n) {   /* the zone plays the sub, two octaves down */
                    held[k] = (uint8_t)(base + k - 24u);
                    on_sub |= 1u << k;
                    sub_on(held[k], 127);
                    midi_send(0x90 | SUB_CH, held[k], 100);
                } else {
                    held[k] = (uint8_t)(base + k);
                    on_sub &= ~(1u << k);
                    play(held[k], 100);
                    midi_send(0x90 | PAD_CH, held[k], 100);
                }
            }
            if (up >> k & 1u) {
                if (on_sub >> k & 1u) { sub_off(held[k]); midi_send(0x80 | SUB_CH, held[k], 0); }
                else if (set.latch) latched |= 1u << k;
                else { stop(held[k]); midi_send(0x80 | PAD_CH, held[k], 0); }
            }
        }

        b = fm1_input_edges(&rel);              /* buttons pressed since the last call (bit = button id) */
        if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) set.oct--;
        if (b & (1u << BTN_OCT_UP) && set.oct < 2) set.oct++;
        if (b & (1u << BTN_SEL)) { set.split ^= 1u; HOST_LOG("%6u ms  split %u\n", fm1_ms, set.split); }
        if (b & (1u << BTN_PLAY)) {             /* the chord latch: let go, the pad holds; off, it lets go too */
            set.latch ^= 1u;
            HOST_LOG("%6u ms  latch %u\n", fm1_ms, set.latch);
            if (!set.latch) {
                for (k = 0; k < 27u; k++)
                    if (latched >> k & 1u) { stop(held[k]); midi_send(0x80 | PAD_CH, held[k], 0); }
                latched = 0;
            }
        }
        for (k = 0; k < NPAGE_BTN; k++)
            if (b & (1u << PAGE_BTN[k])) { page = k; page_drawn = ~0u; saving = 0; }
        if (fm1_enc_take(ENC_SELECT)) { page = PG_SET; page_drawn = ~0u; saving = 0; }
        if (b & (1u << BTN_SAVE)) {
            if (!saving) { saving = 1; page_drawn = ~0u; }
            else {                              /* the sound as it is now, into the user slot */
                set.user[set.save_to] = patch;
                memcpy(set.user[set.save_to].name, "USER1\0\0", 8);
                set.user[set.save_to].name[4] = (char)('1' + set.save_to);
                memcpy(patch.name, set.user[set.save_to].name, 8);
                set.slot = (uint8_t)(NPATCH + set.save_to);
                set.edit = patch;
                HOST_LOG("%6u ms  saved %s\n", fm1_ms, patch.name);
                saving = 0;
                page_drawn = ~0u;
                top_drawn = 0;
            }
        }
        for (k = 0; k < 4u; k++)                /* the knobs: a detent a step; quick turns over a wide range, 4 */
            if ((e = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
                uint32_t id = page * 4u + k;
                int32_t step = PARAMS[id].hi - PARAMS[id].lo > 64 && fm1_ms - t_knob[k] < 60u ? 4 : 1;
                t_knob[k] = fm1_ms;
                param_set(id, param_get(id) + e * step);
                if (id < P_PADCH) sound_set(&set.edit, &set.sub, &set.duck, id, param_get(id));
                HOST_LOG("%6u ms  knob %s %d\n", fm1_ms, PARAMS[id].label, param_get(id));
            }

        while (midi_get(&st, &d1, &d2)) {       /* USB-MIDI in, any channel */
            if (st == 0xF8u) {
                clock_ext(fm1_ms, (uint32_t)d1 | (uint32_t)d2 << 8);
#ifdef DIAG
                if (clk.tick == 0u) {           /* each downbeat, on channel 16: CC 102/103 the tick's stamp (ms),
                                                   104/105 now (ms), 106..108 frames rendered, 109/110 the bar
                                                   phase's top 14 bits (each 14-bit value as high 7, low 7) */
                    uint32_t v[4] = {(uint32_t)d1 | (uint32_t)d2 << 8, fm1_ms, diag_frames, bar_ph >> 18}, j;
                    midi_send(0xBF, 102, v[0] >> 7 & 127u); midi_send(0xBF, 103, v[0] & 127u);
                    midi_send(0xBF, 104, v[1] >> 7 & 127u); midi_send(0xBF, 105, v[1] & 127u);
                    for (j = 0; j < 3u; j++) midi_send(0xBF, 106 + j, v[2] >> (14u - 7u * j) & 127u);
                    midi_send(0xBF, 109, v[3] >> 7 & 127u); midi_send(0xBF, 110, v[3] & 127u);
                }
#endif
            }
            else if (st == 0xFAu) { clk.start = 1; clk.start_ms = fm1_ms; clk.running = 1; HOST_LOG("%6u ms  start\n", fm1_ms); }
            else if (st == 0xFBu) { clk.running = 1; HOST_LOG("%6u ms  continue\n", fm1_ms); }
            else if (st == 0xFCu) { clk.running = 0; HOST_LOG("%6u ms  stop\n", fm1_ms); }
            else if (st == (0x90u | PAD_CH) && d2) play(d1, d2);
            else if (st == (0x80u | PAD_CH) || st == (0x90u | PAD_CH)) stop(d1);
            else if (st == (0x90u | SUB_CH) && d2) sub_on(d1, d2);
            else if (st == (0x80u | SUB_CH) || st == (0x90u | SUB_CH)) sub_off(d1);
            else if ((st & 0xF0u) == 0xB0u && ((st & 0x0Fu) == PAD_CH || (st & 0x0Fu) == SUB_CH)) pad_cc(st & 0x0Fu, d1, d2);
            else if ((st & 0xF0u) == 0xC0u && ((st & 0x0Fu) == PAD_CH || (st & 0x0Fu) == SUB_CH) && d1 < NSLOT) {
                set.slot = d1;                  /* Program Change: 0.. the factory patches, then the user ones */
                slot_load(d1);
                top_drawn = 0;
                HOST_LOG("%6u ms  patch %s\n", fm1_ms, patch.name);
            }
        }
        clock_service(fm1_ms);
        clk_run = clk.running;
        if (was_ext && !clk.ext) {              /* lost: free-run at this tempo, and after a power-off */
            set.bpm10 = (uint16_t)clock_bpm10();
        }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {   /* the patch: the factory ones, then the user ones */
            if (saving) {
                set.save_to = (uint8_t)(((int32_t)set.save_to + e % NUSER + NUSER) % NUSER);
                page_drawn = ~0u;
            } else {
                set.slot = (uint8_t)(((int32_t)set.slot + e % (int32_t)NSLOT + (int32_t)NSLOT) % (int32_t)NSLOT);
                slot_load(set.slot);
                top_drawn = 0;
                HOST_LOG("%6u ms  patch %s\n", fm1_ms, patch.name);
            }
        }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0 && !clk.ext) {   /* the internal tempo, 1 BPM a step */
            e = set.bpm10 / 10 + e;
            set.bpm10 = (uint16_t)((e < 60 ? 60 : e > 200 ? 200 : e) * 10);
            clock_set_bpm10(set.bpm10);
        }

        base = (uint32_t)(KEY0 + 12 * set.oct);
        for (k = 0; k < NV; k++)                /* what sounds, on the keys of this octave */
            if (voice[k].gate && voice[k].note >= base && voice[k].note < base + 27u)
                lit |= 1u << (voice[k].note - base);
        if (sub_gate && sub_n) {                /* the sub's note, on its key: in the split zone, or as the pad's */
            k = sub_held[sub_n - 1u] + (set.split ? 24u : 0u);
            if (k >= base && k < base + (set.split ? set.split_n : 27u)) sub_lit = 1u << (k - base);
        }
        for (k = 0; k < 27u; k++) {
            fm1_led_key(KEY_LED(k), (lit | sub_lit) >> k & 1u);
            led_glow(KEY_LED(k), set.split && k < set.split_n);   /* the sub's zone */
        }
        fm1_led_key(BTN_SEL, set.split);
        for (k = 0; k < NPAGE_BTN; k++) fm1_led_key(PAGE_BTN[k], k == page);
        fm1_led_key(BTN_SAVE, saving && fm1_ms % 500u < 250u);
        fm1_led_key(BTN_OCT_DOWN, set.oct < 0);
        fm1_led_key(BTN_OCT_UP, set.oct > 0);
        fm1_led_key(BTN_PLAY, set.latch || (clk.running && clk.tick % TPB < 3u));   /* latched, else a flash a beat */

        if (clk.tick / TPB != beat_seen) {
            beat_seen = clk.tick / TPB;
            HOST_LOG("%6u ms  beat %u  %s %u.%u  bar %u\n", fm1_ms, beat_seen, clk.ext ? "EXT" : "INT",
                     clock_bpm10() / 10u, clock_bpm10() % 10u, (uint32_t)(((uint64_t)bar_ph * 9600u) >> 32));
        }
        c = clk.ext | clk.running << 1 | beat_seen << 2 | clock_bpm10() << 4;
        if (c != clk_drawn && (c >> 4 == clk_drawn >> 4 || fm1_ms - t_clk >= 250u)) {   /* the tempo at most 4 times a second */
            clk_drawn = c;
            t_clk = fm1_ms;
            top_drawn = 0;
        }
        if (page != page_drawn) { page_drawn = page; for (k = 0; k < 4u; k++) row_drawn[k] = -999; }
#ifdef PROFILE
        {   /* "MAX 2345 P1100 D300 R700 S900" (us): the audio's worst, its stages, the sky's last frame. REC clears */
            static uint32_t t_prof;
            static char prof_s[40];
            if (b & (1u << BTN_REC)) prof_max = prof_pad = prof_cd = prof_rev = 0;
            if (fm1_ms - t_prof >= 500u) {
                uint32_t i = 0, j, val[5] = {prof_max / FM1_TICKS_PER_US, prof_pad / FM1_TICKS_PER_US,
                                             prof_cd / FM1_TICKS_PER_US, prof_rev / FM1_TICKS_PER_US, sky_us};
                static const char *const LBL[5] = {"MAX ", " P", " D", " R", " S"};
                t_prof = fm1_ms;
                for (j = 0; j < 5u; j++) {
                    const char *l = LBL[j];
                    uint32_t d = 10000u, x = val[j], lead = 1;
                    while (*l) prof_s[i++] = *l++;
                    for (; d; d /= 10u)
                        if (x / d || !lead || d == 1u) { prof_s[i++] = (char)('0' + x / d % 10u); lead = 0; }
                }
                prof_s[i] = 0;
            }
#define STATUS_TEXT prof_s
#else
        {
            char status[24], *e = status;       /* only what is not the usual: the octave, all pad, the latch */
            *e = 0;
            if (set.oct) { e = put_str(e, "OCT +0  "); e[-4] = set.oct < 0 ? '-' : '+'; e[-3] = (char)('0' + (set.oct < 0 ? -set.oct : set.oct)); }
            if (!set.split) e = put_str(e, "ALL PAD  ");
            if (set.latch) e = put_str(e, "HOLD");
#define STATUS_TEXT status
#endif
            if (lcd_idle()) {                   /* one part a pass, once the last has gone out: the loop never waits */
                for (k = 0; k < 4u && param_get(page * 4u + k) == row_drawn[k]; k++)
                    ;
                if (!top_drawn) { top_drawn = 1; draw_top(clk.ext, clock_bpm10(), clk.tick / TPB, clk.running); }
                else if (k < 4u) { row_drawn[k] = param_get(page * 4u + k); draw_row(k, page * 4u + k, row_drawn[k]); }
                else if (fm1_ms - t_sky >= 80u) {   /* about 12 frames a second */
                    char save_s[24];
                    t_sky = fm1_ms;
                    put_str(put_str(put_str(save_s, "SAVE TO "), set.user[set.save_to].name), "?");
                    sky_frame(saving ? save_s : PAGE_NAME[page], saving ? C_VAL : C_ACC, STATUS_TEXT);
                }
            }
        }
#undef STATUS_TEXT

        if (memcmp(&set, &last, sizeof set)) { last = set; dirty = 1; t_change = fm1_ms; }
        if (dirty && quiet() && out_peak < 64u && !saving && fm1_ms - t_change > 2000u) {   /* 2 s after the last change, when
                                                                     even the tails are gone (the erase stalls the sound) */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
