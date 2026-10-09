/* dcn41_dp_train.h - DisplayPort 8b/10b link training for a sink connected directly to the DIO (no LTTPR in non-transparent
 * mode), ported from Linux 238650ef6c7c dc/link/protocols/link_dp_training.c and link_dp_training_8b_10b.c (MIT).
 *
 * Pure: no register, no AUX engine, no clock. Every effect goes through dcn41_dp_train_io, so the same code runs in the
 * kext and in tools/dcn41/dptrain, which drives Linux's own dp_perform_link_training and this file against one DPCD sink
 * model and requires the two traces (every AUX read and write, PHY setting, pattern and delay) to be identical.
 *
 * What it does, in Linux's order: decide the settings (TPS4 > TPS3 > TPS2 for EQ, AUX_RD_INTERVAL from DPCD 0x00E, spread
 * unless the source turns it off), leave any previous training, write MAIN_LINK_CHANNEL_CODING_SET, write DOWNSPREAD /
 * LANE_COUNT / LINK_BW, clock recovery (TPS1, up to 100 rounds, five identical voltage-swing requests or max swing end
 * it), channel equalisation (up to 6 rounds), leave training, switch the PHY to video, then either check the link status
 * once (TPS4 or no POST_LT_ADJ_REQ) or run the post-LT adjust-request sequence and clear POST_LT_ADJ_REQ_GRANTED.
 * Not here: 128b/132b (UHBR), LTTPR non-transparent mode, FEC (only needed with DSC), DPIA, eDP and the retry / fallback
 * policy around a failed training.
 */
#ifndef N48_DCN41_DP_TRAIN_H
#define N48_DCN41_DP_TRAIN_H

#include <stdbool.h>
#include <stdint.h>

/* DPCD addresses (Linux include/drm/display/drm_dp.h) */
#define DCN41_DPCD_REV                       0x000u
#define DCN41_DPCD_TRAINING_AUX_RD_INTERVAL  0x00Eu
#define DCN41_DPCD_LINK_BW_SET               0x100u
#define DCN41_DPCD_LANE_COUNT_SET            0x101u
#define DCN41_DPCD_TRAINING_PATTERN_SET      0x102u
#define DCN41_DPCD_TRAINING_LANE0_SET        0x103u
#define DCN41_DPCD_DOWNSPREAD_CTRL           0x107u
#define DCN41_DPCD_MAIN_LINK_CHANNEL_CODING_SET 0x108u
#define DCN41_DPCD_SINK_COUNT                0x200u
#define DCN41_DPCD_LANE0_1_STATUS            0x202u

/* DPCD link-rate codes for 8b/10b (x 0.27 Gbps per lane) */
#define DCN41_DP_RBR  0x06u
#define DCN41_DP_HBR  0x0Au
#define DCN41_DP_HBR2 0x14u
#define DCN41_DP_HBR3 0x1Eu

/* The result, numbered as Linux's enum link_training_result (dc_dp_types.h). */
enum dcn41_dp_lt_result {
    DCN41_LT_SUCCESS = 0,
    DCN41_LT_CR_FAIL_LANE0 = 1,
    DCN41_LT_CR_FAIL_LANE1 = 2,
    DCN41_LT_CR_FAIL_LANE23 = 3,
    DCN41_LT_EQ_FAIL_CR = 4,
    DCN41_LT_EQ_FAIL_CR_PARTIAL = 5,
    DCN41_LT_EQ_FAIL_EQ = 6,
    DCN41_LT_LQA_FAIL = 7,
    DCN41_LT_LINK_LOSS = 8,
    DCN41_LT_ABORT = 9,
    DCN41_LT_BAD_ARGS = 100,    /* not Linux's: a rate or lane count this file does not train */
};

/* What the PHY is asked to send. */
enum dcn41_dp_phy_pattern {
    DCN41_DP_PHY_TPS1 = 1,
    DCN41_DP_PHY_TPS2 = 2,
    DCN41_DP_PHY_TPS3 = 3,
    DCN41_DP_PHY_TPS4 = 4,
    DCN41_DP_PHY_VIDEO = 5,
};

/* The sink's capabilities as read at detection (DC: link->dpcd_caps). */
struct dcn41_dp_sink_caps {
    uint8_t dpcd_rev;        /* DPCD 0x000 */
    bool tps3;               /* 0x002 bit 6 TPS3_SUPPORTED */
    bool post_lt_adj_req;    /* 0x002 bit 5 POST_LT_ADJ_REQ_SUPPORTED */
    bool tps4;               /* 0x003 bit 7 TPS4_SUPPORTED */
};

/* The source side: DCN 4.01's DIO encoders can send TPS3 and TPS4. */
struct dcn41_dp_src_caps {
    bool tps3;
    bool tps4;
    bool spread_off;         /* DC: link->dp_ss_off; spread is 0.5 % down at 30 kHz otherwise */
};

struct dcn41_dp_train_io {
    void *ctx;
    /* 0 = acknowledged; anything else is a failed transaction (a read leaves buf untouched). */
    int (*aux_read)(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len);
    int (*aux_write)(void *ctx, uint32_t addr, const uint8_t *buf, uint32_t len);
    /* Drive the lanes: the same voltage swing / pre-emphasis level (0..3) on every lane, at link_rate (a DCN41_DP_* code). */
    void (*phy_lanes)(void *ctx, uint8_t lane_count, uint8_t link_rate, uint8_t vs, uint8_t pe);
    void (*phy_pattern)(void *ctx, enum dcn41_dp_phy_pattern pattern);
    void (*delay_us)(void *ctx, uint32_t us);
};

/* The drive levels training ended with (meaningful on DCN41_LT_SUCCESS). */
struct dcn41_dp_lt_out {
    uint8_t vs, pe;
};

/* Train lane_count lanes (1, 2 or 4) at link_rate (DCN41_DP_RBR .. DCN41_DP_HBR3). */
enum dcn41_dp_lt_result dcn41_dp_link_train(const struct dcn41_dp_train_io *io, const struct dcn41_dp_sink_caps *sink,
                                            const struct dcn41_dp_src_caps *src, uint8_t link_rate, uint8_t lane_count,
                                            struct dcn41_dp_lt_out *out);

#endif
