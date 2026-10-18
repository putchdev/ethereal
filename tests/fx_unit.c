/* SPDX-License-Identifier: GPL-3.0-only */
/* The FX on their own: an impulse into the bus, what comes out. Built and read by tests/fx_test.py:
 *   cc -O2 -DETHEREAL_HOST -Ifirmware/src -Ifirmware/gen -o build/host/fx_unit tests/fx_unit.c
 *   build/host/fx_unit delay PATCH     (one line per frame with an echo: frame, left, right)
 *   build/host/fx_unit reverb PATCH    (a burst of noise; its decay (s * 10), then a line per block: L*L, R*R, L*R summed)
 *   build/host/fx_unit mod PATCH BPM RUN [EVERY SYNC]   (the LFOs and the duck, a line a control step)
 *   build/host/fx_unit loud PATCH      (5 s of full-scale noise in; the largest return and the lines held at full scale) */
#define main host_main                          /* tests/host.c's, unused here */
#include "../firmware/src/ethereal.c"
#undef main

int main(int argc, char **argv)
{
    static int32_t bus[2u * HALF_FRAMES];
    uint32_t blk, i;
    if (argc < 3) return 2;
    patch = PATCHES[atoi(argv[2])];
    clock_set_bpm10(1740);
    if (!strcmp(argv[1], "delay")) {
        for (blk = 0; blk < 200u; blk++) {      /* 1.2 s */
            memset(bus, 0, sizeof bus);
            if (!blk) bus[0] = bus[1] = 10000;
            fx_delay(bus, HALF_FRAMES);
            for (i = 0; i < HALF_FRAMES; i++)
                if (dly_wet[2u * i] || dly_wet[2u * i + 1u])
                    printf("%u %d %d\n", blk * HALF_FRAMES + i, dly_wet[2u * i], dly_wet[2u * i + 1u]);
        }
        return 0;
    }
    if (!strcmp(argv[1], "reverb")) {
        uint32_t seed = 1;
        printf("%u\n", 10u + patch.rev_decay * 110u / 255u);   /* the set decay, seconds * 10 */
        for (blk = 0; blk < 2600u; blk++) {     /* 15 s */
            long long ll = 0, rr = 0, lr = 0;
            memset(bus, 0, sizeof bus);
            if (blk < 4u)                       /* 23 ms of noise: an impulse would be lost in int16 lines */
                for (i = 0; i < 2u * HALF_FRAMES; i++) { seed = seed * 1664525u + 1013904223u; bus[i] = (int32_t)seed >> 18; }
            fx_reverb(bus, HALF_FRAMES);
            for (i = 0; i < HALF_FRAMES; i++) {
                long long l = rev_wet[2u * i], r = rev_wet[2u * i + 1u];
                ll += l * l; rr += r * r; lr += l * r;
            }
            printf("%lld %lld %lld\n", ll, rr, lr);
        }
        return 0;
    }
    if (!strcmp(argv[1], "loud")) {
        uint32_t seed = 1, clipped = 0, j;
        int32_t peak = 0;
        for (blk = 0; blk < 860u; blk++) {
            for (i = 0; i < 2u * HALF_FRAMES; i++) { seed = seed * 1664525u + 1013904223u; bus[i] = (int32_t)seed >> 17; }
            fx_reverb(bus, HALF_FRAMES);
            for (i = 0; i < 2u * HALF_FRAMES; i++) if (abs(rev_wet[i]) > peak) peak = abs(rev_wet[i]);
            for (j = 0; j < 8u; j++)
                for (i = 0; i < REV_LEN; i++) clipped += abs(rev_line[j][i]) == 524272;
        }
        printf("%d %u\n", peak, clipped);
        return 0;
    }
    if (!strcmp(argv[1], "mod")) {              /* mod PATCH BPM RUN [DUCK_EVERY DUCK_SYNC]: 16 bars of the clock
                                                   from a Start; a line a step: frame made, cut, trem, duck, duck_sub */
        uint32_t k, blocks = 16u * 4u * 60u * 44100u / (uint32_t)atoi(argv[3]) / HALF_FRAMES;
        clk_inc = (uint32_t)(((uint64_t)atoi(argv[3]) << 32) / (4u * 60u * 44100u));
        clk_run = (uint8_t)atoi(argv[4]);
        duck_p.depth = 255;
        duck_p.sub = 128;
        if (argc > 6) { duck_p.every = (uint8_t)atoi(argv[5]); duck_p.sync = (uint8_t)atoi(argv[6]); }
        for (blk = 0; blk < blocks; blk++) {
            started = !blk;
            bars = (uint32_t)(((uint64_t)blk * HALF_FRAMES * clk_inc) >> 32);
            bar_ph = (uint32_t)((uint64_t)blk * HALF_FRAMES * clk_inc);
            mod_block(HALF_FRAMES);
            for (k = 0; k < NSTEP; k++)
                printf("%u %d %d %d %d\n", blk * HALF_FRAMES + (k + 1u) * ENV_CTRL, mod_cut[k], mod_trem[k + 1u],
                       mod_duck[k + 1u], mod_duck_sub[k + 1u]);
        }
        return 0;
    }
    return 2;
}
