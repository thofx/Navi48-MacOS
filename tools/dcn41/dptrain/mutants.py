#!/usr/bin/env python3
"""mutants.py - planted breaks for the dptrain harness (run by mutants.sh after run.sh built the harness into OUT).

For each mutant: copy src/dcn41/dcn41_dp_train.c with ONE plausible mistake, relink it against the same Linux objects and
require the harness to FAIL. A mutant it lets through is a hole in the scenarios.
Usage: mutants.py <repo root> <out dir> <compiler command...>
"""
import pathlib, subprocess, sys

# (id, exact text in dcn41_dp_train.c, replacement, the mistake)
MUTANTS = [
    ("same-vs-limit", "#define LT_MAX_SAME_VS         5u", "#define LT_MAX_SAME_VS         6u",
     "six identical swing requests before CR gives up"),
    ("eq-one-round-short", "round <= LT_MAX_EQ_ROUND", "round < LT_MAX_EQ_ROUND", "five EQ rounds instead of six"),
    ("tps3-scrambled", "case DCN41_DP_PHY_TPS3: return 0x23u;", "case DCN41_DP_PHY_TPS3: return 0x03u;",
     "TPS3 sent with scrambling on"),
    ("tps4-unscrambled", "case DCN41_DP_PHY_TPS4: return 0x07u;", "case DCN41_DP_PHY_TPS4: return 0x27u;",
     "TPS4 sent with scrambling off"),
    ("no-max-swing-flag", "vs | ((vs == LT_MAX_LEVEL) ? 0x04u : 0u)", "vs | 0u", "MAX_SWING_REACHED never set"),
    ("no-max-pe-flag", "((pe == LT_MAX_LEVEL) ? 0x20u : 0u)", "0u", "MAX_PRE_EMPHASIS_REACHED never set"),
    ("no-pe-clamp", "if (pe > LT_MAX_LEVEL - vs)", "if (0)", "pre-emphasis not limited by the swing level"),
    ("failed-read-clears", "        return;\n    for (uint8_t l = 0; l < t->lanes; l++) {\n        t->status[l]",
     "        for (uint32_t z = 0; z < sizeof buf; z++) buf[z] = 0;\n    for (uint8_t l = 0; l < t->lanes; l++) {\n        t->status[l]",
     "a failed status read is taken as all-zero status"),
    ("eq-wait-table", "400, 4000, 8000, 12000, 16000", "400, 4000, 8000, 12000, 15000", "the 16 ms EQ interval is 15 ms"),
    ("cr-wait-unit", "(raw & 0x7Fu) * 4000u", "(raw & 0x7Fu) * 4096u", "CR interval in 4.096 ms units"),
    ("cr-wait-ext-bit", "        t.cr_wait_us = 400;\n", "", "the EXT_RECEIVER_CAP bit alone leaves the CR wait at 100 us"),
    ("eq-fail-swap", "? DCN41_LT_EQ_FAIL_CR_PARTIAL : DCN41_LT_EQ_FAIL_CR",
     "? DCN41_LT_EQ_FAIL_CR : DCN41_LT_EQ_FAIL_CR_PARTIAL", "partial and full CR loss swapped"),
    ("grant-with-tps4", "t.pattern_eq != DCN41_DP_PHY_TPS4 && sink->post_lt_adj_req", "sink->post_lt_adj_req",
     "POST_LT_ADJ_REQ granted with TPS4"),
    ("loss-delay", "io->delay_us(io->ctx, 5000);", "io->delay_us(io->ctx, 1000);", "1 ms before the link-loss check"),
    ("eq-ignores-align", " && (t->align & AL_INTERLANE_ALIGN_DONE))\n            return DCN41_LT_SUCCESS;",
     ")\n            return DCN41_LT_SUCCESS;", "EQ done without inter-lane alignment"),
    ("spread-33khz", "#define LT_SPREAD_05_30KHZ     0x10u", "#define LT_SPREAD_05_30KHZ     0x11u", "down-spread at 33 kHz"),
    ("no-enhanced-framing", "b = (uint8_t)(lane_count | 0x80u | (t.post_lt_adj_granted",
     "b = (uint8_t)(lane_count | (t.post_lt_adj_granted", "ENHANCED_FRAMING not set"),
    ("lane0-only", "    for (uint8_t l = 1; l < t->lanes; l++) {\n        if ((t->adjust[l] & 0x3u) > vs)",
     "    for (uint8_t l = t->lanes; l < t->lanes; l++) {\n        if ((t->adjust[l] & 0x3u) > vs)",
     "only lane 0's request is honoured"),
    ("no-max-vs-stop", "        if (t->vs == LT_MAX_LEVEL)          /* dp_is_max_vs_reached */\n            break;", "",
     "CR keeps going at maximum swing"),
    ("post-adjust-gives-up", "        if (!changed)\n            return true;", "        if (!changed)\n            return false;",
     "post-LT adjust fails when the sink stops asking"),
    ("post-adjust-no-wait", "            t->io->delay_us(t->io->ctx, 1000);\n        }", "        }",
     "post-LT adjust polls without the 1 ms wait"),
    ("coding-zero", "    b = LT_ENCODING_8B10B;", "    b = 0;", "channel coding written as 0"),
    ("tps3-before-tps4", "(src->tps4 && sink->tps4) ? DCN41_DP_PHY_TPS4 : (src->tps3 && sink->tps3) ? DCN41_DP_PHY_TPS3",
     "(src->tps3 && sink->tps3) ? DCN41_DP_PHY_TPS3 : (src->tps4 && sink->tps4) ? DCN41_DP_PHY_TPS4",
     "TPS3 preferred over TPS4"),
    ("old-sink-reads-interval", "if (sink->dpcd_rev >= 0x12u)\n        (void)io->aux_read(io->ctx, DCN41_DPCD_TRAINING_AUX_RD_INTERVAL, &raw, 1);\n    t.eq_wait_us",
     "(void)io->aux_read(io->ctx, DCN41_DPCD_TRAINING_AUX_RD_INTERVAL, &raw, 1);\n    t.eq_wait_us",
     "a DPCD 1.1 sink is asked for TRAINING_AUX_RD_INTERVAL"),
]


def main():
    root, out, cc = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3:]
    src = (root / "src/dcn41/dcn41_dp_train.c").read_text()
    objs = [str(out / f) for f in ("link_dp_training.o", "link_dp_training_8b_10b.o", "link_dp_phy.o", "linux_side.o",
                                   "sink.o", "main.o", "stubs.o")]
    escaped = 0
    for mid, old, new, what in MUTANTS:
        if src.count(old) != 1:
            print(f"MUTANT {mid}: text found {src.count(old)} times, not once")
            escaped += 1
            continue
        (out / "mutant.c").write_text(src.replace(old, new))
        built = subprocess.run(cc + ["-std=c11", "-O1", "-w", f"-I{root}/src/dcn41", "-c", str(out / "mutant.c"), "-o",
                                     str(out / "mutant.o")], capture_output=True, text=True)
        if built.returncode != 0:
            print(f"MUTANT {mid}: does not compile, so it plants nothing: {built.stderr.strip()[:120]}")
            escaped += 1
            continue
        subprocess.run(cc + objs + [str(out / "mutant.o"), "-o", str(out / "dptrain-mutant")], check=True)
        r = subprocess.run([str(out / "dptrain-mutant")], capture_output=True, text=True)
        fails = sum(1 for line in r.stdout.splitlines() if line.startswith("FAIL"))
        if r.returncode == 0:
            print(f"MUTANT {mid}: ESCAPED ({what})")
            escaped += 1
        else:
            print(f"MUTANT {mid}: caught by {fails} scenario(s) ({what})")
    print(f"dptrain mutants: {len(MUTANTS)}, {escaped} escaped or did not apply")
    sys.exit(1 if escaped else 0)


if __name__ == "__main__":
    main()
