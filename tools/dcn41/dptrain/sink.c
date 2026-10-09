/* sink.c - the scripted DP sink and the trace (see sink.h). Lane status nibble: bit 0 CR_DONE, bit 1 CHANNEL_EQ_DONE,
 * bit 2 SYMBOL_LOCKED; LANE_ALIGN_STATUS_UPDATED (0x204): bit 0 INTERLANE_ALIGN_DONE, bit 1 POST_LT_ADJ_REQ_IN_PROGRESS. */
#include "sink.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*                    name                    rate  ln rev   aux   tps3   tps4   post   crvs crpe dead eqvs eqpe never  drop  dl noalign loss  post fail  skew   stuck */
const struct scenario scenarios[] = {
    { "hbr2x4-first-try",                      0x14, 4, 0x14, 0x00, true, true, false, 0, 0, -1, 0, 0, false, false, 0, false, false, 0, 0, false, false },
    { "hbr3x4-cr-steps-eq-steps",              0x1E, 4, 0x14, 0x00, true, true, false, 2, 1, -1, 2, 1, false, false, 0, false, false, 0, 0, false, false },
    { "hbrx2-aux-interval-16ms",               0x0A, 2, 0x14, 0x04, true, true, false, 1, 0, -1, 1, 1, false, false, 0, false, false, 0, 0, false, false },
    { "rbrx1-old-sink-tps2",                   0x06, 1, 0x11, 0x00, false, false, false, 1, 1, -1, 1, 2, false, false, 0, false, false, 0, 0, false, false },
    { "hbr2x4-tps3-sink",                      0x14, 4, 0x12, 0x80, true, false, false, 1, 0, -1, 1, 1, false, false, 0, false, false, 0, 0, false, false },
    { "hbr2x4-cr-lane1-dead",                  0x14, 4, 0x14, 0x00, true, true, false, 1, 0, 1, 0, 0, false, false, 0, false, false, 0, 0, false, false },
    { "hbr2x4-cr-lane3-dead",                  0x14, 4, 0x14, 0x00, true, true, false, 1, 0, 3, 0, 0, false, false, 0, false, false, 0, 0, false, false },
    { "hbr2x2-cr-needs-max-swing",             0x14, 2, 0x14, 0x00, true, true, false, 3, 0, 0, 0, 0, false, false, 0, false, false, 0, 0, false, false },
    { "hbr3x4-eq-never",                       0x1E, 4, 0x14, 0x01, true, true, false, 0, 0, -1, 1, 2, true, false, 0, false, false, 0, 0, false, false },
    { "hbr2x4-eq-drops-cr-lane0",              0x14, 4, 0x14, 0x00, true, true, false, 0, 0, -1, 0, 1, false, true, 0, false, false, 0, 0, false, false },
    { "hbr2x4-eq-drops-cr-lane2",              0x14, 4, 0x14, 0x00, true, true, false, 0, 0, -1, 0, 1, false, true, 2, false, false, 0, 0, false, false },
    { "hbr2x4-no-interlane-align",             0x14, 4, 0x14, 0x00, true, true, false, 0, 0, -1, 0, 0, false, false, 0, true, false, 0, 0, false, false },
    { "hbr2x4-link-loss-after",                0x14, 4, 0x14, 0x00, true, true, false, 0, 0, -1, 0, 0, false, false, 0, false, true, 0, 0, false, false },
    { "hbr2x4-tps3-post-lt-adjust",            0x14, 4, 0x12, 0x00, true, false, true, 1, 0, -1, 1, 1, false, false, 0, false, false, 2, 0, false, false },
    { "hbr2x4-tps4-post-lt-adj-not-granted",   0x14, 4, 0x14, 0x00, true, true, true, 1, 0, -1, 1, 1, false, false, 0, false, false, 2, 0, false, false },
    { "hbr2x4-asks-beyond-pe-limit",           0x14, 4, 0x14, 0x00, true, true, false, 2, 2, -1, 2, 1, false, false, 0, false, false, 0, 0, false, false },
    { "hbr2x4-asks-max-pre-emphasis",          0x14, 4, 0x14, 0x00, true, true, false, 0, 0, -1, 0, 3, false, false, 0, false, false, 0, 0, false, false },
    { "hbr2x4-aux-fails-in-eq",                0x14, 4, 0x14, 0x00, true, true, false, 0, 0, -1, 0, 1, false, false, 0, false, false, 0, 13, false, false },
    { "hbr2x4-cr-last-lane-wants-more",        0x14, 4, 0x14, 0x00, true, true, false, 1, 0, -1, 0, 0, false, false, 0, false, false, 0, 0, true, false },
    { "hbr2x4-tps3-post-lt-adjust-stuck",      0x14, 4, 0x12, 0x00, true, false, true, 0, 0, -1, 0, 0, false, false, 0, false, false, 1, 0, false, true },
    { "hbr2x4-aux-fails-in-cr",                0x14, 4, 0x14, 0x00, true, true, false, 2, 0, -1, 0, 0, false, false, 0, false, false, 0, 9, false, false },
    { "hbr2x4-aux-fails-on-interval",          0x14, 4, 0x14, 0x03, true, true, false, 0, 0, -1, 0, 0, false, false, 0, false, false, 0, 2, false, false },
    { "hbr2x4-aux-fails-on-loss-check",        0x14, 4, 0x14, 0x00, true, true, false, 0, 0, -1, 0, 0, false, false, 0, false, false, 0, 13, false, false },
};
const unsigned n_scenarios = sizeof scenarios / sizeof scenarios[0];

static const struct scenario *sc;
static uint8_t pattern, vs[4], pe[4], lane_count_set;
static uint32_t n_aux, post_left;

void sink_reset(const struct scenario *s)
{
    sc = s;
    pattern = lane_count_set = 0;
    memset(vs, 0, sizeof vs);
    memset(pe, 0, sizeof pe);
    n_aux = 0;
    post_left = s->post_adj_rounds;
}

static bool post_phase(void) { return pattern == 0 && (lane_count_set & 0x20u) && post_left; }

/* 0x202..0x207 */
static void status6(uint8_t *b)
{
    uint8_t nib[4] = { 0 }, adj[4] = { 0 };
    bool all_eq = true, lost_any = false;
    for (int l = 0; l < sc->lanes; l++) {
        if (pattern == 1) {
            uint8_t need = (uint8_t)(sc->cr_vs + ((sc->skew_last_lane && l == sc->lanes - 1) ? 1 : 0));
            bool cr = l != sc->cr_dead_lane && vs[l] >= need;
            nib[l] = cr ? 1u : 0u;
            adj[l] = (uint8_t)(need | (sc->cr_pe << 2));
        } else if (pattern == 2 || pattern == 3 || pattern == 7) {
            bool cr = !(sc->eq_drops_cr && l == sc->eq_drop_lane);
            bool eq = cr && !sc->eq_never && pe[l] >= sc->eq_pe;
            nib[l] = (uint8_t)(cr | (eq << 1) | (eq << 2));
            adj[l] = (uint8_t)(sc->eq_vs | (sc->eq_pe << 2));
            all_eq = all_eq && eq;
        } else {
            bool lost = sc->loss_after && l == sc->lanes - 1;
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

int sink_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t b[16] = { 0 };
    if (++n_aux == sc->aux_fail_at) {
        dump("R", addr, NULL, len, true);
        return -1;
    }
    if (addr == 0x00E && len == 1)
        b[0] = sc->aux_rd_interval;
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
    if (++n_aux == sc->aux_fail_at) {
        dump("W", addr, buf, len, true);
        return -1;
    }
    dump("W", addr, buf, len, false);
    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = addr + i;
        if (a == 0x101)
            lane_count_set = buf[i];
        else if (a == 0x102)
            pattern = buf[i] & 0x0Fu;
        else if (a >= 0x103 && a <= 0x106) {
            vs[a - 0x103] = buf[i] & 0x3u;
            pe[a - 0x103] = (buf[i] >> 3) & 0x3u;
            if (a == 0x103 && pattern == 0 && post_left)
                post_left--;
        }
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
