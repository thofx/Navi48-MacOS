/* dcn41_dp_train.c - DisplayPort 8b/10b link training, ported from Linux 238650ef6c7c dc/link (MIT); see the header.
 * Each block names the Linux function it follows; tools/dcn41/dptrain proves the AUX / PHY / delay trace identical. */
#include "dcn41_dp_train.h"

#include <stddef.h>

#pragma GCC poison float double   /* after the includes: <stddef.h> itself names long double */

#define LT_MAX_CR_ROUNDS       100u   /* LINK_TRAINING_MAX_CR_RETRY */
#define LT_MAX_SAME_VS         5u     /* LINK_TRAINING_MAX_RETRY_COUNT */
#define LT_MAX_EQ_ROUND        5u     /* the EQ loop runs while retries <= LINK_TRAINING_MAX_RETRY_COUNT */
#define LT_POST_ADJ_LIMIT      6u     /* POST_LT_ADJ_REQ_LIMIT */
#define LT_POST_ADJ_TIMEOUT    200u   /* POST_LT_ADJ_REQ_TIMEOUT (1 ms each) */
#define LT_MAX_LEVEL           3u     /* VOLTAGE_SWING_MAX_LEVEL, PRE_EMPHASIS_MAX_LEVEL */
#define LT_SPREAD_05_30KHZ     0x10u  /* LINK_SPREAD_05_DOWNSPREAD_30KHZ, written raw to DOWNSPREAD_CTRL */
#define LT_ENCODING_8B10B      1u     /* DP_8b_10b_ENCODING, written raw to MAIN_LINK_CHANNEL_CODING_SET */
#define LT_RETRY_DELAY_MS      50u    /* LINK_TRAINING_RETRY_DELAY */

/* union lane_count_set (DPCD 0x101) */
#define LCS_ENHANCED_FRAMING        0x80u
#define LCS_POST_LT_ADJ_REQ_GRANTED 0x20u
/* union dpcd_edp_config (0x10A) */
#define EDP_CFG_PANEL_MODE_EDP      0x01u
/* DP_SET_POWER (0x600) */
#define DP_POWER_D0 1u
#define DP_POWER_D3 2u

/* union lane_status nibble / union lane_align_status_updated */
#define LS_CR_DONE       0x1u
#define LS_EQ_DONE       0x2u
#define LS_SYMBOL_LOCKED 0x4u
#define AL_INTERLANE_ALIGN_DONE        0x01u
#define AL_POST_LT_ADJ_REQ_IN_PROGRESS 0x02u

/* dp_link_bandwidth_kbps (link_validation.c): LINK_RATE_REF_FREQ_IN_KHZ, BITS_PER_DP_BYTE, DATA_EFFICIENCY_8b_10b_x10000 */
#define BW_REF_KHZ          27000u
#define BW_BITS_PER_DP_BYTE 10u
#define BW_EFFICIENCY_X10000 8000u

struct lt {
    const struct dcn41_dp_train_io *io;
    uint8_t rate, lanes;
    enum dcn41_dp_phy_pattern pattern_eq;
    uint8_t spread;
    bool post_lt_adj_granted;
    uint32_t cr_wait_us, eq_wait_us;
    uint8_t vs, pe;                 /* the hardware levels; disallow_per_lane_settings: one value for every lane */
    uint8_t status[4], adjust[4], align;
};

static void zero_bytes(void *p, size_t n)
{
    unsigned char *b = (unsigned char *)p;
    for (size_t i = 0; i < n; i++)
        b[i] = 0;
}

/* dp_get_nibble_at_index: lane l's nibble of a DPCD lane-status or adjust-request byte pair */
static uint8_t nibble(const uint8_t *b, uint8_t l) { return (uint8_t)((b[l / 2] >> (4 * (l % 2))) & 0xFu); }
/* union lane_adjust: VOLTAGE_SWING_LANE:2 PRE_EMPHASIS_LANE:2 */
static uint8_t adj_vs(uint8_t a) { return a & 0x3u; }
static uint8_t adj_pe(uint8_t a) { return (uint8_t)((a >> 2) & 0x3u); }

/* union dpcd_training_lane: VOLTAGE_SWING_SET:2 MAX_SWING_REACHED:1 PRE_EMPHASIS_SET:2 MAX_PRE_EMPHASIS_REACHED:1 */
static uint8_t lane_set_byte(uint8_t vs, uint8_t pe)
{
    return (uint8_t)(vs | ((vs == LT_MAX_LEVEL) ? 0x04u : 0u) | (uint8_t)(pe << 3) | ((pe == LT_MAX_LEVEL) ? 0x20u : 0u));
}

/* dp_training_pattern_to_dpcd_training_pattern + dp_initialize_scrambling_data_symbols (SCRAMBLING_DISABLE is bit 5) */
static uint8_t pattern_set_byte(enum dcn41_dp_phy_pattern p)
{
    switch (p) {
    case DCN41_DP_PHY_TPS1: return 0x21u;
    case DCN41_DP_PHY_TPS2: return 0x22u;
    case DCN41_DP_PHY_TPS3: return 0x23u;
    case DCN41_DP_PHY_TPS4: return 0x07u;
    default: return 0x00u;
    }
}

static void phy_lanes(struct lt *t) { t->io->phy_lanes(t->io->ctx, t->lanes, t->rate, t->vs, t->pe); }

/* dpcd_set_lt_pattern_and_lane_settings: pattern and lane bytes in one (1 + lanes)-byte burst */
static void write_pattern_and_lanes(struct lt *t, enum dcn41_dp_phy_pattern p)
{
    uint8_t buf[5] = { pattern_set_byte(p), 0, 0, 0, 0 };
    for (uint8_t l = 0; l < t->lanes; l++)
        buf[1 + l] = lane_set_byte(t->vs, t->pe);
    (void)t->io->aux_write(t->io->ctx, DCN41_DPCD_TRAINING_PATTERN_SET, buf, 1u + t->lanes);
}

/* dpcd_set_lane_settings */
static void write_lanes(struct lt *t)
{
    uint8_t buf[4];
    for (uint8_t l = 0; l < t->lanes; l++)
        buf[l] = lane_set_byte(t->vs, t->pe);
    (void)t->io->aux_write(t->io->ctx, DCN41_DPCD_TRAINING_LANE0_SET, buf, t->lanes);
}

/* The lane status, align status and adjust requests start every sequence cleared (Linux: fresh locals). */
static void reset_status(struct lt *t)
{
    zero_bytes(t->status, sizeof t->status);
    zero_bytes(t->adjust, sizeof t->adjust);
    t->align = 0;
}

/* dp_get_lane_status_and_lane_adjust: on a failed read the previous status and requests stand */
static void read_status(struct lt *t)
{
    uint8_t buf[6] = { 0 };
    if (t->io->aux_read(t->io->ctx, DCN41_DPCD_LANE0_1_STATUS, buf, sizeof buf) != 0)
        return;
    for (uint8_t l = 0; l < t->lanes; l++) {
        t->status[l] = nibble(buf, l);
        t->adjust[l] = nibble(buf + 4, l);
    }
    t->align = buf[2];
}

static bool all_lanes(const struct lt *t, uint8_t bits)
{
    for (uint8_t l = 0; l < t->lanes; l++)
        if ((t->status[l] & bits) != bits)
            return false;
    return true;
}

/* dp_decide_lane_settings with disallow_per_lane_settings: the largest request over the trained lanes, clamped
 * (maximize_lane_settings, get_max_pre_emphasis_for_voltage_swing: 3 - swing) */
static void decide_lanes(struct lt *t)
{
    uint8_t vs = adj_vs(t->adjust[0]), pe = adj_pe(t->adjust[0]);
    for (uint8_t l = 1; l < t->lanes; l++) {
        if (adj_vs(t->adjust[l]) > vs)
            vs = adj_vs(t->adjust[l]);
        if (adj_pe(t->adjust[l]) > pe)
            pe = adj_pe(t->adjust[l]);
    }
    if (pe > LT_MAX_LEVEL - vs)
        pe = (uint8_t)(LT_MAX_LEVEL - vs);
    t->vs = vs;
    t->pe = pe;
}

/* perform_8b_10b_clock_recovery_sequence (offset DPRX) */
static enum dcn41_dp_lt_result clock_recovery(struct lt *t)
{
    uint32_t same_vs = 0, rounds = 0;

    reset_status(t);
    t->io->phy_pattern(t->io->ctx, DCN41_DP_PHY_TPS1);
    while (same_vs < LT_MAX_SAME_VS && rounds < LT_MAX_CR_ROUNDS) {
        phy_lanes(t);
        if (!rounds)
            write_pattern_and_lanes(t, DCN41_DP_PHY_TPS1);
        else
            write_lanes(t);
        t->io->delay_us(t->io->ctx, t->cr_wait_us);
        read_status(t);
        if (all_lanes(t, LS_CR_DONE))
            return DCN41_LT_SUCCESS;
        if (t->vs == LT_MAX_LEVEL)          /* dp_is_max_vs_reached */
            break;
        if (t->vs == adj_vs(t->adjust[0]))
            same_vs++;
        else
            same_vs = 0;
        decide_lanes(t);
        rounds++;
    }
    /* dp_get_cr_failure */
    if (!(t->status[0] & LS_CR_DONE))
        return DCN41_LT_CR_FAIL_LANE0;
    if (t->lanes >= 2 && !(t->status[1] & LS_CR_DONE))
        return DCN41_LT_CR_FAIL_LANE1;
    if (t->lanes >= 4 && (!(t->status[2] & LS_CR_DONE) || !(t->status[3] & LS_CR_DONE)))
        return DCN41_LT_CR_FAIL_LANE23;
    return DCN41_LT_SUCCESS;
}

/* perform_8b_10b_channel_equalization_sequence (offset DPRX) */
static enum dcn41_dp_lt_result channel_equalization(struct lt *t)
{
    reset_status(t);
    t->io->phy_pattern(t->io->ctx, t->pattern_eq);
    for (uint32_t round = 0; round <= LT_MAX_EQ_ROUND; round++) {
        phy_lanes(t);
        if (!round)
            write_pattern_and_lanes(t, t->pattern_eq);
        else
            write_lanes(t);
        t->io->delay_us(t->io->ctx, t->eq_wait_us);
        read_status(t);
        if (!all_lanes(t, LS_CR_DONE))
            return (t->status[0] & LS_CR_DONE) ? DCN41_LT_EQ_FAIL_CR_PARTIAL : DCN41_LT_EQ_FAIL_CR;
        if (all_lanes(t, LS_EQ_DONE | LS_SYMBOL_LOCKED) && (t->align & AL_INTERLANE_ALIGN_DONE))
            return DCN41_LT_SUCCESS;
        decide_lanes(t);
    }
    return DCN41_LT_EQ_FAIL_EQ;
}

/* perform_post_lt_adj_req_sequence */
static bool post_lt_adjust(struct lt *t)
{
    reset_status(t);
    for (uint32_t count = 0; count < LT_POST_ADJ_LIMIT; count++) {
        bool changed = false;
        for (uint32_t timer = 0; timer < LT_POST_ADJ_TIMEOUT; timer++) {
            read_status(t);
            if (!(t->align & AL_POST_LT_ADJ_REQ_IN_PROGRESS))
                return true;
            if (!all_lanes(t, LS_CR_DONE))
                return false;
            if (!all_lanes(t, LS_EQ_DONE | LS_SYMBOL_LOCKED) || !(t->align & AL_INTERLANE_ALIGN_DONE))
                return false;
            for (uint8_t l = 0; l < t->lanes; l++)
                if (t->vs != adj_vs(t->adjust[l]) || t->pe != adj_pe(t->adjust[l])) {
                    changed = true;
                    break;
                }
            if (changed) {
                decide_lanes(t);
                phy_lanes(t);          /* dp_set_drive_settings */
                write_lanes(t);
                break;
            }
            t->io->delay_us(t->io->ctx, 1000);
        }
        if (!changed)
            return true;
    }
    return true;
}

/* dp_check_link_loss_status: SINK_COUNT .. LANE_ALIGN_STATUS_UPDATED in one read; a failed read reads as loss */
static enum dcn41_dp_lt_result link_loss(struct lt *t)
{
    uint8_t buf[6] = { 0 };
    (void)t->io->aux_read(t->io->ctx, DCN41_DPCD_SINK_COUNT, buf, sizeof buf);
    for (uint8_t l = 0; l < t->lanes; l++) {
        uint8_t s = nibble(buf + 2, l);
        if ((s & (LS_CR_DONE | LS_EQ_DONE | LS_SYMBOL_LOCKED)) != (LS_CR_DONE | LS_EQ_DONE | LS_SYMBOL_LOCKED) ||
            !(buf[4] & AL_INTERLANE_ALIGN_DONE))
            return DCN41_LT_LINK_LOSS;
    }
    return DCN41_LT_SUCCESS;
}

/* get_eq_training_aux_rd_interval (8b/10b) */
static uint32_t eq_wait(uint8_t raw)
{
    static const uint32_t us[] = { 400, 4000, 8000, 12000, 16000, 32000, 64000 };
    uint8_t i = raw & 0x7Fu;
    return i < sizeof us / sizeof us[0] ? us[i] : 400u;
}

enum dcn41_dp_lt_result dcn41_dp_link_train(const struct dcn41_dp_train_io *io, const struct dcn41_dp_sink_caps *sink,
                                            const struct dcn41_dp_src_caps *src, uint8_t link_rate, uint8_t lane_count,
                                            struct dcn41_dp_lt_out *out)
{
    struct lt t;
    enum dcn41_dp_lt_result r;
    uint8_t raw, b;

    if (!io || !sink || !src || (lane_count != 1 && lane_count != 2 && lane_count != 4) ||
        (link_rate != DCN41_DP_RBR && link_rate != DCN41_DP_HBR && link_rate != DCN41_DP_HBR2 && link_rate != DCN41_DP_HBR3))
        return DCN41_LT_BAD_ARGS;
    zero_bytes(&t, sizeof t);
    t.io = io;
    t.rate = link_rate;
    t.lanes = lane_count;

    /* decide_8b_10b_training_settings: the EQ interval is read first, then the CR one (two reads of 0x00E) */
    t.spread = src->spread_off ? 0u : LT_SPREAD_05_30KHZ;
    raw = 0;
    if (sink->dpcd_rev >= 0x12u)
        (void)io->aux_read(io->ctx, DCN41_DPCD_TRAINING_AUX_RD_INTERVAL, &raw, 1);
    t.eq_wait_us = eq_wait(raw);
    t.pattern_eq = (src->tps4 && sink->tps4) ? DCN41_DP_PHY_TPS4 : (src->tps3 && sink->tps3) ? DCN41_DP_PHY_TPS3 : DCN41_DP_PHY_TPS2;
    raw = 0;
    t.cr_wait_us = 100;                                 /* get_cr_training_aux_rd_interval, no LTTPR */
    if (sink->dpcd_rev >= 0x12u)
        (void)io->aux_read(io->ctx, DCN41_DPCD_TRAINING_AUX_RD_INTERVAL, &raw, 1);
    if (raw) {
        t.cr_wait_us = 400;
        if (raw & 0x7Fu)
            t.cr_wait_us = (uint32_t)(raw & 0x7Fu) * 4000u;
    }
    t.post_lt_adj_granted = t.pattern_eq != DCN41_DP_PHY_TPS4 && sink->post_lt_adj_req;

    /* dp_perform_link_training: leave training, channel coding (FEC: not set ready, see the header) */
    b = 0;
    (void)io->aux_write(io->ctx, DCN41_DPCD_TRAINING_PATTERN_SET, &b, 1);
    b = LT_ENCODING_8B10B;
    (void)io->aux_write(io->ctx, DCN41_DPCD_MAIN_LINK_CHANNEL_CODING_SET, &b, 1);

    /* dpcd_set_link_settings */
    (void)io->aux_write(io->ctx, DCN41_DPCD_DOWNSPREAD_CTRL, &t.spread, 1);
    b = (uint8_t)(lane_count | LCS_ENHANCED_FRAMING | (t.post_lt_adj_granted ? LCS_POST_LT_ADJ_REQ_GRANTED : 0u));
    (void)io->aux_write(io->ctx, DCN41_DPCD_LANE_COUNT_SET, &b, 1);
    (void)io->aux_write(io->ctx, DCN41_DPCD_LINK_BW_SET, &link_rate, 1);

    r = clock_recovery(&t);
    if (r == DCN41_LT_SUCCESS)
        r = channel_equalization(&t);

    b = 0;
    (void)io->aux_write(io->ctx, DCN41_DPCD_TRAINING_PATTERN_SET, &b, 1);

    /* dp_transition_to_video_idle */
    io->phy_pattern(io->ctx, DCN41_DP_PHY_VIDEO);
    if (!sink->post_lt_adj_req || t.pattern_eq == DCN41_DP_PHY_TPS4) {
        if (r == DCN41_LT_SUCCESS) {
            io->delay_us(io->ctx, 5000);
            r = link_loss(&t);
        }
    } else {
        if (r == DCN41_LT_SUCCESS && !post_lt_adjust(&t))
            r = DCN41_LT_LQA_FAIL;
        b = (uint8_t)(lane_count | LCS_ENHANCED_FRAMING);
        (void)io->aux_write(io->ctx, DCN41_DPCD_LANE_COUNT_SET, &b, 1);
    }
    if (out) {
        out->vs = t.vs;
        out->pe = t.pe;
    }
    return r;
}

/* ---- the fallback policy and the retry loop: link_dp_capability.c, link_dp_training.c, link_validation.c ---- */

static bool at_min_rate(uint8_t rate) { return rate <= DCN41_DP_RBR; }      /* reached_minimum_link_rate */
static bool at_min_lanes(uint8_t lanes) { return lanes <= 1; }               /* reached_minimum_lane_count */
static bool at_floor(struct dcn41_dp_link_settings s) { return at_min_rate(s.rate) && at_min_lanes(s.lanes); }

/* reduce_link_rate (the 8b/10b rates of a DP sink; eDP's intermediate rates are not trained here) */
static uint8_t lower_rate(uint8_t rate)
{
    switch (rate) {
    case DCN41_DP_HBR3: return DCN41_DP_HBR2;
    case DCN41_DP_HBR2: return DCN41_DP_HBR;
    case DCN41_DP_HBR:  return DCN41_DP_RBR;
    default:            return 0;              /* LINK_RATE_UNKNOWN; the callers never get here */
    }
}

/* reduce_lane_count */
static uint8_t fewer_lanes(uint8_t lanes)
{
    switch (lanes) {
    case 4:  return 2;
    case 2:  return 1;
    default: return 0;                         /* LANE_COUNT_UNKNOWN; the callers never get here */
    }
}

/* An equalisation failure that lowers the rate also caps `max` there, so a later clock-recovery fallback cannot climb
 * back above it (Linux: "Reduce max link rate to avoid potential infinite loop"). */
static void lower_rate_and_cap(struct dcn41_dp_link_settings *max, struct dcn41_dp_link_settings *cur)
{
    cur->rate = lower_rate(cur->rate);
    max->rate = cur->rate;
    cur->lanes = max->lanes;
}

bool dcn41_dp_fallback(struct dcn41_dp_link_settings *max, struct dcn41_dp_link_settings *cur,
                       enum dcn41_dp_lt_result result)
{
    switch (result) {
    case DCN41_LT_CR_FAIL_LANE0:
    case DCN41_LT_CR_FAIL_LANE1:
    case DCN41_LT_CR_FAIL_LANE23:
    case DCN41_LT_LQA_FAIL:
        if (!at_min_rate(cur->rate)) {
            cur->rate = lower_rate(cur->rate);
        } else if (!at_min_lanes(cur->lanes)) {
            cur->rate = max->rate;             /* Linux sets this before giving up on lane 0, so it stays set */
            if (result == DCN41_LT_CR_FAIL_LANE0)
                return false;
            if (result == DCN41_LT_CR_FAIL_LANE1)
                cur->lanes = 1;
            else if (result == DCN41_LT_CR_FAIL_LANE23)
                cur->lanes = 2;
            else
                cur->lanes = fewer_lanes(cur->lanes);
        } else {
            return false;
        }
        return true;
    case DCN41_LT_EQ_FAIL_EQ:
    case DCN41_LT_EQ_FAIL_CR_PARTIAL:
        if (!at_min_lanes(cur->lanes))
            cur->lanes = fewer_lanes(cur->lanes);
        else if (!at_min_rate(cur->rate))
            lower_rate_and_cap(max, cur);
        else
            return false;
        return true;
    case DCN41_LT_EQ_FAIL_CR:
        if (at_min_rate(cur->rate))
            return false;
        lower_rate_and_cap(max, cur);
        return true;
    default:
        return false;
    }
}

uint32_t dcn41_dp_link_bandwidth_kbps(struct dcn41_dp_link_settings s)
{
    uint32_t per_lane = (uint32_t)s.rate * BW_REF_KHZ * BW_BITS_PER_DP_BYTE;
    return per_lane * s.lanes / 10000u * BW_EFFICIENCY_X10000;
}

/* perform_link_training_with_retries for a DP (not eDP, not MST, not DPIA) sink on the DIO, do_fallback on */
bool dcn41_dp_link_train_with_retries(const struct dcn41_dp_train_io *io, const struct dcn41_dp_sink_caps *sink,
                                      const struct dcn41_dp_src_caps *src, struct dcn41_dp_link_settings max,
                                      uint32_t req_kbps, uint32_t attempts, struct dcn41_dp_link_settings *trained,
                                      enum dcn41_dp_lt_result *last)
{
    struct dcn41_dp_link_settings cur = max, cap = max;   /* Linux: cur_link_settings, max_link_settings */
    enum dcn41_dp_lt_result status = DCN41_LT_CR_FAIL_LANE0;
    uint32_t j = 0, fail_count = 0, delay_ms = LT_RETRY_DELAY_MS;
    bool bw_low = false, bw_min = at_floor(max);
    uint8_t b;

    if (!io || !sink || !src || !io->phy_on || !io->phy_off || !io->stream_encoder_setup) {
        if (last)
            *last = DCN41_LT_BAD_ARGS;
        return false;
    }
    io->stream_encoder_setup(io->ctx);                  /* link_hwss->setup_stream_encoder, 8b/10b: before the loop */
    while (j < attempts && fail_count < attempts * 10u) {
        /* dp_enable_link_phy: the output on, then the sink to D0 (dpcd_write_rx_power_ctrl) */
        io->phy_on(io->ctx, cur.lanes, cur.rate);
        b = DP_POWER_D0;
        (void)io->aux_write(io->ctx, DCN41_DPCD_SET_POWER, &b, 1);
        /* dp_set_panel_mode(DP_PANEL_MODE_DEFAULT): a sink that reports PANEL_MODE_EDP has it cleared */
        b = 0;
        if (io->aux_read(io->ctx, DCN41_DPCD_EDP_CONFIGURATION_SET, &b, 1) == 0 && (b & EDP_CFG_PANEL_MODE_EDP)) {
            b = (uint8_t)(b & ~EDP_CFG_PANEL_MODE_EDP);
            (void)io->aux_write(io->ctx, DCN41_DPCD_EDP_CONFIGURATION_SET, &b, 1);
        }
        status = dcn41_dp_link_train(io, sink, src, cur.rate, cur.lanes, NULL);
        if (status == DCN41_LT_SUCCESS && !bw_low) {
            if (trained)
                *trained = cur;
            if (last)
                *last = status;
            return true;
        }
        fail_count++;
        if (j == attempts - 1)                          /* the last attempt failed: keep the PHY on, give up */
            break;
        /* dp_disable_link_phy: the sink to D3, then the output off */
        b = DP_POWER_D3;
        (void)io->aux_write(io->ctx, DCN41_DPCD_SET_POWER, &b, 1);
        io->phy_off(io->ctx);
        if ((status == DCN41_LT_SUCCESS && bw_low) || bw_min) {
            /* trained, but too slow for the stream, or already at the floor: the next attempt starts from the top */
            j++;
            cur = max;
            delay_ms += LT_RETRY_DELAY_MS;
            bw_low = false;
            bw_min = at_floor(max);
        } else {
            (void)dcn41_dp_fallback(&cap, &cur, status);  /* Linux ignores the verdict: a floor it cannot leave trains again */
            bw_low = req_kbps > dcn41_dp_link_bandwidth_kbps(cur);
            bw_min = at_floor(cur);
        }
        io->delay_us(io->ctx, delay_ms * 1000u);
    }
    if (trained)
        *trained = cur;
    if (last)
        *last = status;
    return false;
}
