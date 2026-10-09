/* main.c - the dptrain harness: every scenario through Linux's link training and through src/dcn41/dcn41_dp_train.c,
 * against fresh copies of one scripted sink; the traces (AUX reads and writes, PHY lane settings, patterns, delays) and the
 * results must be identical, and each scenario must end the way it was written to (expect[]: a scenario that drifts to
 * another path stops proving what its name says).
 *   dptrain          run all, print one line per scenario
 *   dptrain -v NAME  print both traces of one scenario */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcn41_dp_train.h"
#include "sink.h"

int linux_train(const struct scenario *s);
extern unsigned linux_warnings;

static const struct { const char *name; int result; } expect[] = {
    { "hbr2x4-first-try", DCN41_LT_SUCCESS },
    { "hbr3x4-cr-steps-eq-steps", DCN41_LT_SUCCESS },
    { "hbrx2-aux-interval-16ms", DCN41_LT_SUCCESS },
    { "rbrx1-old-sink-tps2", DCN41_LT_SUCCESS },
    { "hbr2x4-tps3-sink", DCN41_LT_SUCCESS },
    { "hbr2x4-cr-lane1-dead", DCN41_LT_CR_FAIL_LANE1 },
    { "hbr2x4-cr-lane3-dead", DCN41_LT_CR_FAIL_LANE23 },
    { "hbr2x2-cr-needs-max-swing", DCN41_LT_CR_FAIL_LANE0 },
    { "hbr3x4-eq-never", DCN41_LT_EQ_FAIL_EQ },
    { "hbr2x4-eq-drops-cr-lane0", DCN41_LT_EQ_FAIL_CR },
    { "hbr2x4-eq-drops-cr-lane2", DCN41_LT_EQ_FAIL_CR_PARTIAL },
    { "hbr2x4-no-interlane-align", DCN41_LT_EQ_FAIL_EQ },
    { "hbr2x4-link-loss-after", DCN41_LT_LINK_LOSS },
    { "hbr2x4-tps3-post-lt-adjust", DCN41_LT_SUCCESS },
    { "hbr2x4-tps4-post-lt-adj-not-granted", DCN41_LT_SUCCESS },
    { "hbr2x4-asks-beyond-pe-limit", DCN41_LT_SUCCESS },
    { "hbr2x4-asks-max-pre-emphasis", DCN41_LT_SUCCESS },
    { "hbr2x4-aux-fails-in-eq", DCN41_LT_SUCCESS },
    { "hbr2x4-cr-last-lane-wants-more", DCN41_LT_SUCCESS },
    { "hbr2x4-tps3-post-lt-adjust-stuck", DCN41_LT_SUCCESS },
    { "hbr2x4-aux-fails-in-cr", DCN41_LT_SUCCESS },
    { "hbr2x4-aux-fails-on-interval", DCN41_LT_SUCCESS },
    { "hbr2x4-aux-fails-on-loss-check", DCN41_LT_LINK_LOSS },
};

static int io_read(void *c, uint32_t a, uint8_t *b, uint32_t n) { (void)c; return sink_read(a, b, n); }
static int io_write(void *c, uint32_t a, const uint8_t *b, uint32_t n) { (void)c; return sink_write(a, b, n); }
static void io_lanes(void *c, uint8_t lanes, uint8_t rate, uint8_t vs, uint8_t pe)
{
    (void)c;
    trace("PHY lanes=%u rate=0x%02x vs=%u pe=%u", lanes, rate, vs, pe);
}
static void io_pattern(void *c, enum dcn41_dp_phy_pattern p)
{
    static const char *n[] = { "?", "TPS1", "TPS2", "TPS3", "TPS4", "VIDEO" };
    (void)c;
    trace("PAT %s", (unsigned)p < 6 ? n[p] : "?");
}
static void io_delay(void *c, uint32_t us) { (void)c; trace("DELAY %u", us); }

static int our_train(const struct scenario *s)
{
    const struct dcn41_dp_train_io io = { NULL, io_read, io_write, io_lanes, io_pattern, io_delay };
    const struct dcn41_dp_sink_caps sink = { s->dpcd_rev, s->tps3, s->post_lt_adj, s->tps4 };
    const struct dcn41_dp_src_caps src = { true, true, false };
    struct dcn41_dp_lt_out out;
    return (int)dcn41_dp_link_train(&io, &sink, &src, s->rate, s->lanes, &out);
}

static int expected(const char *name)
{
    for (size_t i = 0; i < sizeof expect / sizeof expect[0]; i++)
        if (!strcmp(expect[i].name, name))
            return expect[i].result;
    return -1;
}

int main(int argc, char **argv)
{
    const char *verbose = argc > 2 && !strcmp(argv[1], "-v") ? argv[2] : NULL;
    unsigned bad = 0;
    static char lt[1 << 20];

    for (unsigned i = 0; i < n_scenarios; i++) {
        const struct scenario *s = &scenarios[i];
        int rl, ro, want = expected(s->name);
        unsigned warn;
        const char *a, *b;
        size_t line = 1, k = 0;

        if (verbose && strcmp(verbose, s->name))
            continue;
        trace_reset();
        sink_reset(s);
        linux_warnings = 0;
        rl = linux_train(s);
        warn = linux_warnings;
        snprintf(lt, sizeof lt, "%s", trace_text());
        trace_reset();
        sink_reset(s);
        ro = our_train(s);
        a = lt;
        b = trace_text();
        while (a[k] && a[k] == b[k]) {
            if (a[k] == '\n')
                line++;
            k++;
        }
        if (verbose)
            printf("---- Linux (result %d, %u assert(s))\n%s---- ours (result %d)\n%s", rl, warn, a, ro, b);
        if (a[k] != b[k] || rl != ro || rl != want) {
            bad++;
            printf("FAIL %-38s linux=%d ours=%d expect=%d", s->name, rl, ro, want);
            if (a[k] != b[k])
                printf(", traces differ at line %zu", line);
            printf("\n");
        } else {
            printf("ok   %-38s result %d, %zu trace lines identical", s->name, rl, line - 1);
            if (warn)
                printf(" (Linux asserted %u time(s): a limit Linux treats as unexpected was reached)", warn);
            printf("\n");
        }
    }
    printf("dptrain: %u scenario(s), %u failed\n", n_scenarios, bad);
    return bad ? 1 : 0;
}
