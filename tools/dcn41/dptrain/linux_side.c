/* linux_side.c - Linux's dp_perform_link_training and perform_link_training_with_retries (link_dp_training.c,
 * link_dp_training_8b_10b.c, link_dp_phy.c, link_dp_capability.c, link_validation.c, all compiled unmodified) on one
 * dc_link wired to the scripted sink (link_edp_panel_control.c too, for the panel-mode step of the mode-set). The functions defined here are the ones a training reaches outside those files;
 * they behave as on DCN 4.01 with a DP sink connected directly to the DIO (no LTTPR, no FEC, no DPIA, no eDP, no MST).
 * Every other undefined symbol is a generated stub that aborts if a scenario ever reaches it (run.sh). */
#include "link_dp_training.h"
#include "link_dp_phy.h"
#include "link_dpcd.h"
#include "link_hwss.h"
#include "link_enc_cfg.h"
#include "core_types.h"
#include "clk_mgr.h"

#include "sink.h"

static struct dc dc;
static struct dc_context ctx;
static struct dc_link link;
static struct link_encoder enc;
static struct link_encoder_funcs enc_funcs;
static struct link_hwss hwss;
static struct clk_mgr clk_mgr;
static struct clk_mgr_funcs clk_mgr_funcs;
static struct clock_source clock_source;
static struct dc_stream_state stream;
static struct pipe_ctx pipe;
static struct dc_state state;
static const struct scenario *running;
unsigned linux_warnings;

void dcn41_shim_warn(const char *file, int line) { (void)file; (void)line; linux_warnings++; }
void dcn41_shim_udelay(unsigned long us) { trace("DELAY %lu", us); }

enum dc_status core_link_read_dpcd(struct dc_link *l, uint32_t address, uint8_t *data, uint32_t size)
{
    (void)l;
    return sink_read(address, data, size) ? DC_ERROR_UNEXPECTED : DC_OK;
}
enum dc_status core_link_write_dpcd(struct dc_link *l, uint32_t address, const uint8_t *data, uint32_t size)
{
    (void)l;
    return sink_write(address, data, size) ? DC_ERROR_UNEXPECTED : DC_OK;
}

/* link_hwss: the PHY's lane settings and test pattern, the stream encoder */
static void set_lanes(struct dc_link *l, const struct link_resource *res, const struct dc_link_settings *s,
                      const struct dc_lane_settings ls[LANE_COUNT_DP_MAX])
{
    (void)l; (void)res;
    for (int i = 1; i < (int)s->lane_count; i++)
        if (ls[i].VOLTAGE_SWING != ls[0].VOLTAGE_SWING || ls[i].PRE_EMPHASIS != ls[0].PRE_EMPHASIS)
            trace("PHY LANES DIFFER");
    trace("PHY lanes=%u rate=0x%02x vs=%u pe=%u", (unsigned)s->lane_count, (unsigned)s->link_rate,
          (unsigned)ls[0].VOLTAGE_SWING, (unsigned)ls[0].PRE_EMPHASIS);
}
static void set_pattern(struct dc_link *l, const struct link_resource *res, struct encoder_set_dp_phy_pattern_param *p)
{
    (void)l; (void)res;
    switch (p->dp_phy_pattern) {
    case DP_TEST_PATTERN_TRAINING_PATTERN1: trace("PAT TPS1"); break;
    case DP_TEST_PATTERN_TRAINING_PATTERN2: trace("PAT TPS2"); break;
    case DP_TEST_PATTERN_TRAINING_PATTERN3: trace("PAT TPS3"); break;
    case DP_TEST_PATTERN_TRAINING_PATTERN4: trace("PAT TPS4"); break;
    case DP_TEST_PATTERN_VIDEO_MODE: trace("PAT VIDEO"); break;
    default: trace("PAT other %d", (int)p->dp_phy_pattern); break;
    }
}
static void setup_stream_encoder(struct pipe_ctx *p) { (void)p; trace("STREAM-ENC"); }
const struct link_hwss *get_link_hwss(const struct dc_link *l, const struct link_resource *res) { (void)l; (void)res; return &hwss; }
static void fec_set_ready(struct link_encoder *e, bool ready) { (void)e; trace("FEC ready=%d", ready); }

/* dc hwss: the link output */
static void enable_dp_link_output(struct dc_link *l, const struct link_resource *res, enum signal_type signal,
                                  enum clock_source_id cs, const struct dc_link_settings *s)
{
    (void)l; (void)res; (void)signal; (void)cs;
    trace("PHY ON rate=0x%02x lanes=%u", (unsigned)s->link_rate, (unsigned)s->lane_count);
}
static void disable_link_output(struct dc_link *l, const struct link_resource *res, enum signal_type signal)
{
    (void)l; (void)res; (void)signal;
    trace("PHY OFF");
}

struct link_encoder *link_enc_cfg_get_link_enc(const struct dc_link *l) { (void)l; return &enc; }
/* dp_is_fec_supported asks for the DIO encoder this way first (resource.c): the one encoder, with fec_supported 0 */
struct link_encoder *get_temp_dio_link_enc(const struct resource_context *res_ctx, const struct resource_pool *const pool,
                                           const struct dc_link *l)
{
    (void)res_ctx; (void)pool; (void)l;
    return &enc;
}
/* dp_is_lttpr_present, dp_should_enable_fec, link_dp_get_encoding_format and the LTTPR helpers come from Linux's own
 * link_dp_capability.c (with the zeroed dpcd_caps: no LTTPR, no FEC); dp_get_panel_mode, dp_set_panel_mode and
 * edp_set_panel_assr from link_edp_panel_control.c (a DP sink: DP_PANEL_MODE_DEFAULT). */
/* the stream's bandwidth is a scenario input, not something under test */
uint32_t dc_bandwidth_in_kbps_from_timing(const struct dc_crtc_timing *timing, const enum dc_link_encoding_format enc_fmt)
{
    (void)timing; (void)enc_fmt;
    return running->req_kbps;
}
/* debug bookkeeping (link_dp_trace.c): nothing a training decides on */
void dp_trace_lt_total_count_increment(struct dc_link *l, bool in_detection) { (void)l; (void)in_detection; }
void dp_trace_lt_fail_count_update(struct dc_link *l, unsigned int fail_count, bool in_detection) { (void)l; (void)fail_count; (void)in_detection; }
void dp_trace_lt_result_update(struct dc_link *l, enum link_training_result result, bool in_detection) { (void)l; (void)result; (void)in_detection; }
void dp_trace_set_lt_start_timestamp(struct dc_link *l, bool in_detection) { (void)l; (void)in_detection; }
void dp_trace_set_lt_end_timestamp(struct dc_link *l, bool in_detection) { (void)l; (void)in_detection; }
void dp_trace_commit_lt_init(struct dc_link *l) { (void)l; }

static void setup(const struct scenario *s)
{
    running = s;
    memset(&dc, 0, sizeof dc);
    memset(&ctx, 0, sizeof ctx);
    memset(&link, 0, sizeof link);
    memset(&enc, 0, sizeof enc);
    memset(&hwss, 0, sizeof hwss);
    memset(&enc_funcs, 0, sizeof enc_funcs);
    memset(&clk_mgr, 0, sizeof clk_mgr);
    memset(&clk_mgr_funcs, 0, sizeof clk_mgr_funcs);
    memset(&clock_source, 0, sizeof clock_source);
    memset(&stream, 0, sizeof stream);
    memset(&pipe, 0, sizeof pipe);
    memset(&state, 0, sizeof state);
    ctx.dc = &dc;
    dc.ctx = &ctx;
    dc.clk_mgr = &clk_mgr;
    dc.current_state = &state;
    clk_mgr.funcs = &clk_mgr_funcs;
    dc.hwss.enable_dp_link_output = enable_dp_link_output;
    dc.hwss.disable_link_output = disable_link_output;
    link.ctx = &ctx;
    link.dc = &dc;
    link.ep_type = DISPLAY_ENDPOINT_PHY;
    link.connector_signal = SIGNAL_TYPE_DISPLAY_PORT;
    link.type = dc_connection_single;
    link.dpcd_caps.dpcd_rev.raw = s->dpcd_rev;
    link.dpcd_caps.max_ln_count.bits.TPS3_SUPPORTED = s->tps3;
    link.dpcd_caps.max_ln_count.bits.POST_LT_ADJ_REQ_SUPPORTED = s->post_lt_adj;
    link.dpcd_caps.max_down_spread.bits.TPS4_SUPPORTED = s->tps4;
    enc.features.flags.bits.IS_TPS3_CAPABLE = 1;
    enc.features.flags.bits.IS_TPS4_CAPABLE = 1;
    enc.funcs = &enc_funcs;
    enc_funcs.fec_set_ready = fec_set_ready;
    hwss.ext.set_dp_lane_settings = set_lanes;
    hwss.ext.set_dp_link_test_pattern = set_pattern;
    hwss.setup_stream_encoder = setup_stream_encoder;
    stream.link = &link;
    stream.ctx = &ctx;
    stream.signal = SIGNAL_TYPE_DISPLAY_PORT;
    pipe.stream = &stream;
    pipe.link_res.dio_link_enc = &enc;
    pipe.clock_source = &clock_source;
}

int linux_train(const struct scenario *s)
{
    struct dc_link_settings ls = { 0 };

    setup(s);
    ls.lane_count = (enum dc_lane_count)s->lanes;
    ls.link_rate = (enum dc_link_rate)s->rate;
    link.cur_link_settings = ls;
    return (int)dp_perform_link_training(&link, &pipe.link_res, &ls, false);
}

bool linux_train_with_retries(const struct scenario *s, uint8_t *rate, uint8_t *lanes)
{
    struct dc_link_settings ls = { 0 };
    bool ok;

    setup(s);
    ls.lane_count = (enum dc_lane_count)s->lanes;
    ls.link_rate = (enum dc_link_rate)s->rate;
    ok = perform_link_training_with_retries(&ls, false, (int)s->attempts, &pipe, SIGNAL_TYPE_DISPLAY_PORT, true);
    *rate = (uint8_t)link.cur_link_settings.link_rate;
    *lanes = (uint8_t)link.cur_link_settings.lane_count;
    return ok;
}
