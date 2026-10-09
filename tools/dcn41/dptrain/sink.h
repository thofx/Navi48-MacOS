/* sink.h - a scripted DisplayPort sink (its DPCD) and the trace both sides of the dptrain harness write.
 * Linux's dp_perform_link_training and src/dcn41/dcn41_dp_train.c each run against a fresh sink of one scenario; the
 * harness compares the two traces line for line. */
#ifndef N48_DPTRAIN_SINK_H
#define N48_DPTRAIN_SINK_H

#include <stdbool.h>
#include <stdint.h>

struct scenario {
    const char *name;
    uint8_t rate, lanes;            /* what the source trains at (DPCD rate code, 1/2/4) */
    uint8_t dpcd_rev;               /* 0x000 */
    uint8_t aux_rd_interval;        /* 0x00E */
    bool tps3, tps4, post_lt_adj;   /* 0x002 bit 6, 0x003 bit 7, 0x002 bit 5 */
    /* clock recovery: a lane locks once the swing is >= cr_vs; the sink asks for cr_vs / cr_pe meanwhile */
    uint8_t cr_vs, cr_pe;
    int cr_dead_lane;               /* -1, or a lane that never locks */
    /* channel EQ: lanes lock once the pre-emphasis is >= eq_pe; the sink asks for eq_vs / eq_pe meanwhile */
    uint8_t eq_vs, eq_pe;
    bool eq_never;                  /* EQ never completes */
    bool eq_drops_cr;               /* lane eq_drop_lane loses CR during EQ */
    int eq_drop_lane;
    bool no_align;                  /* INTERLANE_ALIGN_DONE never set */
    bool loss_after;                /* the status read after training shows a lost lane */
    uint8_t post_adj_rounds;        /* post-LT adjust: requests that many changes, then clears IN_PROGRESS */
    uint32_t aux_fail_at;           /* the n-th AUX transaction (1-based) fails; 0 = none */
    bool skew_last_lane;            /* during CR the last lane needs, and asks for, one swing level more */
    bool post_stuck;                /* post-LT adjust: IN_PROGRESS stays set and the request never changes */
};

extern const struct scenario scenarios[];
extern const unsigned n_scenarios;

/* the sink of the running scenario */
void sink_reset(const struct scenario *s);
int sink_read(uint32_t addr, uint8_t *buf, uint32_t len);          /* 0 ok, -1 failed (buf untouched) */
int sink_write(uint32_t addr, const uint8_t *buf, uint32_t len);

/* the trace */
void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *trace_text(void);
void trace_reset(void);

#endif
