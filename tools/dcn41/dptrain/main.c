/* main.c - the dptrain harness: every scenario through Linux's training and through src/dcn41/dcn41_dp_train.c, against
 * fresh copies of one scripted sink; the traces (AUX reads and writes, PHY lane settings, patterns, delays, the link output
 * on and off) and the results must be identical, and each scenario must end the way it was written to (expect[],
 * expect_retry[]: a scenario that drifts to another path stops proving what its name says).
 *   dptrain          run all, print one line per scenario
 *   dptrain -v NAME  print both traces of one scenario */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcn41_dp_train.h"
#include "sink.h"

int linux_train(const struct scenario *s);
bool linux_train_with_retries(const struct scenario *s, uint8_t *rate, uint8_t *lanes);
extern unsigned linux_warnings;

/* one training: the result */
static const struct { const char *name; int result; } expect[] = {
    { "hbr2x4-first-try", DCN41_LT_SUCCESS },
    { "hbr3x4-cr-steps-eq-steps", DCN41_LT_SUCCESS },
    { "hbrx2-aux-interval-16ms", DCN41_LT_SUCCESS },
    { "rbrx1-old-sink-tps2", DCN41_LT_SUCCESS },
    { "hbr2x4-tps3-sink", DCN41_LT_SUCCESS },
    { "hbr2x4-cr-lane1-dead", DCN41_LT_CR_FAIL_LANE1 },
    { "hbr2x4-cr-lane3-dead", DCN41_LT_CR_FAIL_LANE23 },
    { "hbr2x2-cr-lane1-dead", DCN41_LT_CR_FAIL_LANE1 },
    { "rbrx1-cr-lane0-dead", DCN41_LT_CR_FAIL_LANE0 },
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
/* the mode-set's training: trained?, the setting it ended on, the last training's result */
static const struct { const char *name; bool ok; uint8_t rate, lanes; int last; } expect_retry[] = {
    { "retry-first-try", true, 0x14, 4, DCN41_LT_SUCCESS },
    { "retry-cr-fails-above-hbr", true, 0x0A, 4, DCN41_LT_SUCCESS },
    { "retry-eq-fails-above-2-lanes", true, 0x14, 2, DCN41_LT_SUCCESS },
    { "retry-lane1-dead-ends-on-one-lane", true, 0x14, 1, DCN41_LT_SUCCESS },
    { "retry-eq-drops-cr-lane2", true, 0x14, 2, DCN41_LT_SUCCESS },
    { "retry-eq-cap-holds-after-cr-fallback", true, 0x0A, 1, DCN41_LT_SUCCESS },
    { "retry-fallback-too-slow-for-stream", false, 0x1E, 4, DCN41_LT_CR_FAIL_LANE0 },
    { "retry-bw-one-kbps-short", false, 0x1E, 4, DCN41_LT_CR_FAIL_LANE0 },
    { "retry-bw-fits-only-at-10-bits-per-byte", true, 0x14, 4, DCN41_LT_SUCCESS },
    { "retry-eq-never-walks-every-setting", false, 0x14, 4, DCN41_LT_EQ_FAIL_EQ },
    { "retry-sink-unplugged-in-first-training", false, 0x0A, 4, DCN41_LT_CR_FAIL_LANE0 },
    { "retry-panel-mode-bit-set", true, 0x14, 4, DCN41_LT_SUCCESS },
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
static void io_phy_on(void *c, uint8_t lanes, uint8_t rate) { (void)c; trace("PHY ON rate=0x%02x lanes=%u", rate, lanes); }
static void io_phy_off(void *c) { (void)c; trace("PHY OFF"); }
static void io_stream_enc(void *c) { (void)c; trace("STREAM-ENC"); }

static const struct dcn41_dp_train_io io = { NULL, io_read, io_write, io_lanes, io_pattern, io_delay, io_phy_on, io_phy_off, io_stream_enc };

static int our_train(const struct scenario *s)
{
    const struct dcn41_dp_sink_caps sink = { s->dpcd_rev, s->tps3, s->post_lt_adj, s->tps4 };
    const struct dcn41_dp_src_caps src = { true, true, false };
    struct dcn41_dp_lt_out out;
    return (int)dcn41_dp_link_train(&io, &sink, &src, s->rate, s->lanes, &out);
}

static bool our_train_with_retries(const struct scenario *s, struct dcn41_dp_link_settings *trained, enum dcn41_dp_lt_result *last)
{
    const struct dcn41_dp_sink_caps sink = { s->dpcd_rev, s->tps3, s->post_lt_adj, s->tps4 };
    const struct dcn41_dp_src_caps src = { true, true, false };
    const struct dcn41_dp_link_settings max = { s->rate, s->lanes };
    return dcn41_dp_link_train_with_retries(&io, &sink, &src, max, s->req_kbps, s->attempts, trained, last);
}

/* the first line on which the two traces differ (1-based), 0 = identical; *lines = the number of lines of a */
static size_t first_diff(const char *a, const char *b, size_t *lines)
{
    size_t k = 0, line = 1;
    while (a[k] && a[k] == b[k]) {
        if (a[k] == '\n')
            line++;
        k++;
    }
    *lines = line - 1;
    return a[k] == b[k] ? 0 : line;
}

int main(int argc, char **argv)
{
    const char *verbose = argc > 2 && !strcmp(argv[1], "-v") ? argv[2] : NULL;
    unsigned bad = 0;
    static char lt[1 << 20];

    for (unsigned i = 0; i < n_scenarios; i++) {
        const struct scenario *s = &scenarios[i];
        char why[256] = "";
        size_t diff, lines;
        unsigned warn;

        if (verbose && strcmp(verbose, s->name))
            continue;
        trace_reset();
        sink_reset(s);
        linux_warnings = 0;
        if (s->req_kbps == 0) {
            int rl = linux_train(s), ro, want = -1;
            for (size_t e = 0; e < sizeof expect / sizeof expect[0]; e++)
                if (!strcmp(expect[e].name, s->name))
                    want = expect[e].result;
            warn = linux_warnings;
            snprintf(lt, sizeof lt, "%s", trace_text());
            trace_reset();
            sink_reset(s);
            ro = our_train(s);
            diff = first_diff(lt, trace_text(), &lines);
            if (verbose)
                printf("---- Linux (result %d, %u assert(s))\n%s---- ours (result %d)\n%s", rl, warn, lt, ro, trace_text());
            if (rl != ro || rl != want)
                snprintf(why, sizeof why, "linux=%d ours=%d expect=%d", rl, ro, want);
        } else {
            uint8_t lr = 0, ll = 0;
            bool okl = linux_train_with_retries(s, &lr, &ll), oko, want_ok = false;
            struct dcn41_dp_link_settings trained = { 0, 0 };
            enum dcn41_dp_lt_result last = DCN41_LT_BAD_ARGS;
            uint8_t want_rate = 0, want_lanes = 0;
            int want_last = -1;
            for (size_t e = 0; e < sizeof expect_retry / sizeof expect_retry[0]; e++)
                if (!strcmp(expect_retry[e].name, s->name)) {
                    want_ok = expect_retry[e].ok;
                    want_rate = expect_retry[e].rate;
                    want_lanes = expect_retry[e].lanes;
                    want_last = expect_retry[e].last;
                }
            warn = linux_warnings;
            snprintf(lt, sizeof lt, "%s", trace_text());
            trace_reset();
            sink_reset(s);
            oko = our_train_with_retries(s, &trained, &last);
            diff = first_diff(lt, trace_text(), &lines);
            if (verbose)
                printf("---- Linux (trained %d at 0x%02x x%u, %u assert(s))\n%s---- ours (trained %d at 0x%02x x%u, last %d)\n%s",
                       okl, lr, ll, warn, lt, oko, trained.rate, trained.lanes, (int)last, trace_text());
            /* Linux's cur_link_settings is cleared by its last PHY off: only a success leaves the trained setting there */
            if (okl != oko || (okl && (lr != trained.rate || ll != trained.lanes)) || oko != want_ok ||
                trained.rate != want_rate || trained.lanes != want_lanes || (int)last != want_last)
                snprintf(why, sizeof why, "linux=%d@0x%02xx%u ours=%d@0x%02xx%u last=%d expect=%d@0x%02xx%u last=%d",
                         okl, lr, ll, oko, trained.rate, trained.lanes, (int)last, want_ok, want_rate, want_lanes, want_last);
        }
        if (diff || why[0]) {
            bad++;
            printf("FAIL %-40s %s%s", s->name, why, diff ? "" : "\n");
            if (diff)
                printf("%straces differ at line %zu\n", why[0] ? ", " : "", diff);
        } else {
            printf("ok   %-40s %zu trace lines identical", s->name, lines);
            if (warn)
                printf(" (Linux asserted %u time(s): a limit Linux treats as unexpected was reached)", warn);
            printf("\n");
        }
    }
    printf("dptrain: %u scenario(s), %u failed\n", n_scenarios, bad);
    return bad ? 1 : 0;
}
