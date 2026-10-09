/* sink.c - the scripted DP sink and the trace (see sink.h). Lane status nibble: bit 0 CR_DONE, bit 1 CHANNEL_EQ_DONE,
 * bit 2 SYMBOL_LOCKED; LANE_ALIGN_STATUS_UPDATED (0x204): bit 0 INTERLANE_ALIGN_DONE, bit 1 POST_LT_ADJ_REQ_IN_PROGRESS. */
#include "sink.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define HBR2x4_KBPS 17280000u   /* dp_link_bandwidth_kbps: rate * 27000 * 10 * lanes / 10000 * 8000 */

#define S(...) { __VA_ARGS__ }
#define NO_DEAD .cr_dead_lane = -1
#define SINK14 .dpcd_rev = 0x14, .tps3 = true, .tps4 = true
const struct scenario scenarios[] = {
    /* one training (dp_perform_link_training) */
    S(.name = "hbr2x4-first-try", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD),
    S(.name = "hbr3x4-cr-steps-eq-steps", .rate = 0x1E, .lanes = 4, SINK14, NO_DEAD, .cr_vs = 2, .cr_pe = 1, .eq_vs = 2, .eq_pe = 1),
    S(.name = "hbrx2-aux-interval-16ms", .rate = 0x0A, .lanes = 2, SINK14, .aux_rd_interval = 0x04, NO_DEAD, .cr_vs = 1, .eq_vs = 1, .eq_pe = 1),
    S(.name = "rbrx1-old-sink-tps2", .rate = 0x06, .lanes = 1, .dpcd_rev = 0x11, NO_DEAD, .cr_vs = 1, .cr_pe = 1, .eq_vs = 1, .eq_pe = 2),
    S(.name = "hbr2x4-tps3-sink", .rate = 0x14, .lanes = 4, .dpcd_rev = 0x12, .aux_rd_interval = 0x80, .tps3 = true, NO_DEAD,
      .cr_vs = 1, .eq_vs = 1, .eq_pe = 1),
    S(.name = "hbr2x4-cr-lane1-dead", .rate = 0x14, .lanes = 4, SINK14, .cr_vs = 1, .cr_dead_lane = 1),
    S(.name = "hbr2x4-cr-lane3-dead", .rate = 0x14, .lanes = 4, SINK14, .cr_vs = 1, .cr_dead_lane = 3),
    S(.name = "hbr2x2-cr-lane1-dead", .rate = 0x14, .lanes = 2, SINK14, .cr_vs = 1, .cr_dead_lane = 1),
    S(.name = "rbrx1-cr-lane0-dead", .rate = 0x06, .lanes = 1, SINK14, .cr_dead_lane = 0),
    S(.name = "hbr2x2-cr-needs-max-swing", .rate = 0x14, .lanes = 2, SINK14, .cr_vs = 3, .cr_dead_lane = 0),
    S(.name = "hbr3x4-eq-never", .rate = 0x1E, .lanes = 4, SINK14, .aux_rd_interval = 0x01, NO_DEAD, .eq_vs = 1, .eq_pe = 2, .eq_never = true),
    S(.name = "hbr2x4-eq-drops-cr-lane0", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .eq_pe = 1, .eq_drops_cr = true, .eq_drop_lane = 0),
    S(.name = "hbr2x4-eq-drops-cr-lane2", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .eq_pe = 1, .eq_drops_cr = true, .eq_drop_lane = 2),
    S(.name = "hbr2x4-no-interlane-align", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .no_align = true),
    S(.name = "hbr2x4-link-loss-after", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .loss_after = true),
    S(.name = "hbr2x4-tps3-post-lt-adjust", .rate = 0x14, .lanes = 4, .dpcd_rev = 0x12, .tps3 = true, .post_lt_adj = true, NO_DEAD,
      .cr_vs = 1, .eq_vs = 1, .eq_pe = 1, .post_adj_rounds = 2),
    S(.name = "hbr2x4-tps4-post-lt-adj-not-granted", .rate = 0x14, .lanes = 4, SINK14, .post_lt_adj = true, NO_DEAD,
      .cr_vs = 1, .eq_vs = 1, .eq_pe = 1, .post_adj_rounds = 2),
    S(.name = "hbr2x4-asks-beyond-pe-limit", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .cr_vs = 2, .cr_pe = 2, .eq_vs = 2, .eq_pe = 1),
    S(.name = "hbr2x4-asks-max-pre-emphasis", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .eq_pe = 3),
    S(.name = "hbr2x4-aux-fails-in-eq", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .eq_pe = 1, .aux_fail_at = 13),
    S(.name = "hbr2x4-cr-last-lane-wants-more", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .cr_vs = 1, .skew_last_lane = true),
    S(.name = "hbr2x4-tps3-post-lt-adjust-stuck", .rate = 0x14, .lanes = 4, .dpcd_rev = 0x12, .tps3 = true, .post_lt_adj = true, NO_DEAD,
      .post_adj_rounds = 1, .post_stuck = true),
    S(.name = "hbr2x4-aux-fails-in-cr", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .cr_vs = 2, .aux_fail_at = 9),
    S(.name = "hbr2x4-aux-fails-on-interval", .rate = 0x14, .lanes = 4, SINK14, .aux_rd_interval = 0x03, NO_DEAD, .aux_fail_at = 2),
    S(.name = "hbr2x4-aux-fails-on-loss-check", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .aux_fail_at = 13),
    /* the mode-set's training with fallback and retries (perform_link_training_with_retries) */
    S(.name = "retry-first-try", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .req_kbps = 1000000, .attempts = 4),
    S(.name = "retry-cr-fails-above-hbr", .rate = 0x1E, .lanes = 4, SINK14, NO_DEAD, .cr_fail_above_rate = 0x0A, .req_kbps = 1000000, .attempts = 4),
    S(.name = "retry-eq-fails-above-2-lanes", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .eq_fail_above_lanes = 2, .req_kbps = 1000000, .attempts = 4),
    S(.name = "retry-lane1-dead-ends-on-one-lane", .rate = 0x14, .lanes = 4, SINK14, .cr_dead_lane = 1, .req_kbps = 1000000, .attempts = 4),
    S(.name = "retry-eq-drops-cr-lane2", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .eq_drops_cr = true, .eq_drop_lane = 2,
      .req_kbps = 1000000, .attempts = 4),
    S(.name = "retry-eq-cap-holds-after-cr-fallback", .rate = 0x14, .lanes = 4, SINK14, .cr_dead_lane = 1, .cr_dead_lane_max_rate = 0x0A,
      .eq_fail_above_rate = 0x0A, .req_kbps = 1000000, .attempts = 4),
    S(.name = "retry-fallback-too-slow-for-stream", .rate = 0x1E, .lanes = 4, SINK14, NO_DEAD, .cr_fail_above_rate = 0x0A,
      .req_kbps = 12000000, .attempts = 4),
    S(.name = "retry-bw-one-kbps-short", .rate = 0x1E, .lanes = 4, SINK14, NO_DEAD, .cr_fail_above_rate = 0x14,
      .req_kbps = HBR2x4_KBPS + 1, .attempts = 2),
    S(.name = "retry-bw-fits-only-at-10-bits-per-byte", .rate = 0x1E, .lanes = 4, SINK14, NO_DEAD, .cr_fail_above_rate = 0x14,
      .req_kbps = 15000000, .attempts = 2),
    S(.name = "retry-eq-never-walks-every-setting", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .eq_never = true, .req_kbps = 1000000, .attempts = 2),
    S(.name = "retry-sink-unplugged-in-first-training", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .aux_fail_from = 9,
      .req_kbps = 1000000, .attempts = 4),
    S(.name = "retry-panel-mode-bit-set", .rate = 0x14, .lanes = 4, SINK14, NO_DEAD, .panel_mode_edp_bit = true, .req_kbps = 1000000, .attempts = 4),
};
const unsigned n_scenarios = sizeof scenarios / sizeof scenarios[0];

static const struct scenario *sc;
static uint8_t pattern, vs[4], pe[4], lane_count_set, rate_set, edp_cfg;
static uint32_t n_aux, post_left;

void sink_reset(const struct scenario *s)
{
    sc = s;
    pattern = lane_count_set = rate_set = 0;
    edp_cfg = s->panel_mode_edp_bit ? 1u : 0u;
    memset(vs, 0, sizeof vs);
    memset(pe, 0, sizeof pe);
    n_aux = 0;
    post_left = s->post_adj_rounds;
}

static uint8_t cur_lanes(void) { return (lane_count_set & 0x1Fu) ? (lane_count_set & 0x1Fu) : sc->lanes; }
static bool post_phase(void) { return pattern == 0 && (lane_count_set & 0x20u) && post_left; }

/* 0x202..0x207 */
static void status6(uint8_t *b)
{
    uint8_t nib[4] = { 0 }, adj[4] = { 0 }, lanes = cur_lanes();
    bool all_eq = true, lost_any = false;
    for (int l = 0; l < lanes; l++) {
        if (pattern == 1) {
            uint8_t need = (uint8_t)(sc->cr_vs + ((sc->skew_last_lane && l == lanes - 1) ? 1 : 0));
            bool dead = l == sc->cr_dead_lane && (!sc->cr_dead_lane_max_rate || rate_set <= sc->cr_dead_lane_max_rate);
            bool cr = !dead && vs[l] >= need && !(sc->cr_fail_above_rate && rate_set > sc->cr_fail_above_rate);
            nib[l] = cr ? 1u : 0u;
            adj[l] = (uint8_t)(need | (sc->cr_pe << 2));
        } else if (pattern == 2 || pattern == 3 || pattern == 7) {
            bool cr = !(sc->eq_drops_cr && l == sc->eq_drop_lane);
            bool eq = cr && !sc->eq_never && pe[l] >= sc->eq_pe && !(sc->eq_fail_above_lanes && lanes > sc->eq_fail_above_lanes) &&
                      !(sc->eq_fail_above_rate && rate_set > sc->eq_fail_above_rate);
            nib[l] = (uint8_t)(cr | (eq << 1) | (eq << 2));
            adj[l] = (uint8_t)(sc->eq_vs | (sc->eq_pe << 2));
            all_eq = all_eq && eq;
        } else {
            bool lost = sc->loss_after && l == lanes - 1;
            nib[l] = lost ? 1u : 7u;
            lost_any = lost_any || lost;
            /* post-LT adjust: ask for a different pre-emphasis than the current one */
            adj[l] = (post_phase() && !sc->post_stuck) ? (uint8_t)(vs[0] | (((pe[0] == 0 && vs[0] < 3) ? 1u : 0u) << 2))
                                                     : (uint8_t)(vs[0] | (pe[0] << 2));
        }
    }
    b[0] = (uint8_t)(nib[0] | (nib[1] << 4));
    b[1] = (uint8_t)(nib[2] | (nib[3] << 4));
    b[2] = 0;
    if (!sc->no_align && ((pattern != 0 && pattern != 1 && all_eq) || (pattern == 0 && !lost_any)))
        b[2] |= 1u;
    if (post_phase())
        b[2] |= 2u;
    b[3] = 0;
    b[4] = (uint8_t)(adj[0] | (adj[1] << 4));
    b[5] = (uint8_t)(adj[2] | (adj[3] << 4));
}

static void dump(const char *op, uint32_t addr, const uint8_t *buf, uint32_t len, bool fail)
{
    char s[64] = "";
    for (uint32_t i = 0; buf && i < len && i < 16; i++)
        snprintf(s + strlen(s), sizeof s - strlen(s), " %02x", buf[i]);
    trace("%s 0x%05x %u%s%s", op, addr, len, fail ? " FAIL" : "", fail ? "" : s);
}

static bool aux_fails(void)
{
    n_aux++;
    return n_aux == sc->aux_fail_at || (sc->aux_fail_from && n_aux >= sc->aux_fail_from);
}

int sink_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t b[16] = { 0 };
    if (aux_fails()) {
        dump("R", addr, NULL, len, true);
        return -1;
    }
    if (addr == 0x00E && len == 1)
        b[0] = sc->aux_rd_interval;
    else if (addr == 0x10A && len == 1)
        b[0] = edp_cfg;
    else if (addr == 0x202 && len == 6)
        status6(b);
    else if (addr == 0x200 && len == 6) {
        uint8_t s[6];
        status6(s);
        b[0] = 1;
        b[2] = s[0];
        b[3] = s[1];
        b[4] = s[2];
    } else {
        trace("R 0x%05x %u UNMODELLED", addr, len);
        return -1;
    }
    memcpy(buf, b, len);
    dump("R", addr, buf, len, false);
    return 0;
}

int sink_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (aux_fails()) {
        dump("W", addr, buf, len, true);
        return -1;
    }
    dump("W", addr, buf, len, false);
    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = addr + i;
        if (a == 0x100)
            rate_set = buf[i];
        else if (a == 0x101)
            lane_count_set = buf[i];
        else if (a == 0x102)
            pattern = buf[i] & 0x0Fu;
        else if (a >= 0x103 && a <= 0x106) {
            vs[a - 0x103] = buf[i] & 0x3u;
            pe[a - 0x103] = (buf[i] >> 3) & 0x3u;
            if (a == 0x103 && pattern == 0 && post_left)
                post_left--;
        } else if (a == 0x10A)
            edp_cfg = buf[i];
    }
    return 0;
}

static char text[1 << 20];
static size_t used;

void trace(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (used < sizeof text - 256) {
        used += (size_t)vsnprintf(text + used, sizeof text - used, fmt, ap);
        text[used++] = '\n';
        text[used] = 0;
    }
    va_end(ap);
}
const char *trace_text(void) { return text; }
void trace_reset(void) { used = 0; text[0] = 0; }
