/* dml_modes.c - run Linux's DML2.1 (DCN 4.01) on the modes of src/dcn41/dcn41_modes.tsv, on the build host.
 *
 * Prototype for moving mode planning into user space (the kext may not use the FPU; DML2.1 is floating point). Linux's
 * DML2.1 sources are compiled unmodified (run.sh); this file only fills a dml2_display_cfg the way DC does
 * (dml21_translation_helper.c: populate_dml21_timing_config_from_stream_state, _output_config_, _dummy_surface_cfg,
 * _dummy_plane_cfg, dml21_map_dc_state_into_dml_display_cfg) and reads what dml2_build_mode_programming returns.
 *
 * Deliberate differences from DC, because they describe what this driver does:
 *   - one pipe per display: ODM forced to bypass, dynamic ODM and SubVP off;
 *   - no firmware-assisted memory-clock switching: FAMS2, SubVP and both DRR p-state methods are off in the PMO options;
 *   - the static DCN 4.01 SoC bounding box and IP caps (dcn4_soc_bb.h), not the SMU's clock table DC reads at boot.
 *
 * Input, one mode per line on stdin (run.sh cuts it from the TSV):
 *   name signal pix_clk_100hz h_active h_front h_sync h_total v_active v_front v_sync v_total max_vstartup
 * Output, one line per mode: supported?, min clocks, DML's VSTARTUP against the TSV's bound, DET size, urgent watermark.
 * Exit status: 1 if a mode the TSV carries is unsupported, DML's VSTARTUP exceeds the TSV's max_vstartup, or DML asserted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dml_top.h"
#include "dml2_internal_shared_types.h"
#include "dcn4_soc_bb.h"

/* The shim routes Linux's ASSERT / WARN_ON here. DML asserting means it got an input it does not expect: counted per mode
 * and reported, never ignored. */
static unsigned n_warn;
static char first_warn[160];
void dcn41_shim_warn(const char *file, int line)
{
    if (!n_warn++) {
        const char *b = strrchr(file, '/');
        snprintf(first_warn, sizeof first_warn, "%s:%d", b ? b + 1 : file, line);
    }
}

static void fill(struct dml2_display_cfg *cfg, const char *signal, unsigned long pix_100hz, unsigned long ha,
                 unsigned long hf, unsigned long hs, unsigned long ht, unsigned long va, unsigned long vf,
                 unsigned long vs, unsigned long vt, int gpuvm, const struct dml2_soc_bb *bb)
{
    struct dml2_stream_parameters *st = &cfg->stream_descriptors[0];
    struct dml2_plane_parameters *pl = &cfg->plane_descriptors[0];
    unsigned long w = ha > 3840 ? 3840 : ha, h = va > 2160 ? 2160 : va;   /* the dummy plane's 4K cap */

    memset(cfg, 0, sizeof *cfg);
    cfg->gpuvm_enable = gpuvm;
    cfg->gpuvm_max_page_table_levels = bb->gpuvm_max_page_table_levels ? bb->gpuvm_max_page_table_levels : 4;
    cfg->hostvm_max_non_cached_page_table_levels = bb->hostvm_max_non_cached_page_table_levels;
    cfg->minimize_det_reallocation = true;
    cfg->num_streams = 1;
    cfg->num_planes = 1;
    (void)vs;

    /* timing: populate_dml21_timing_config_from_stream_state, progressive, no borders, no DSC, 8 bpc */
    st->timing.h_active = ha;
    st->timing.v_active = va;
    st->timing.h_front_porch = hf;
    st->timing.v_front_porch = vf > 1 ? vf : 1;
    st->timing.pixel_clock_khz = pix_100hz / 10;
    st->timing.h_total = ht;
    st->timing.v_total = vt;
    st->timing.h_sync_width = hs;
    st->timing.h_blank_end = (ht - hf) - ha;
    st->timing.v_blank_end = (vt - st->timing.v_front_porch) - va;
    st->timing.drr_config.disallowed = true;
    st->timing.dsc.enable = dml2_dsc_disable;
    st->timing.bpc = 8;
    st->timing.vblank_nom = vt - va;

    /* output: populate_dml21_output_config_from_stream_state */
    st->output.output_dp_lane_count = 4;
    st->output.output_encoder = strcmp(signal, "HDMI") ? dml2_dp : dml2_hdmi;
    st->output.output_format = dml2_444;
    st->output.output_dp_link_rate = dml2_dp_rate_na;
    st->output.output_disabled = true;

    /* overrides: one pipe, no SubVP (see the header) */
    st->overrides.odm_mode = dml2_odm_mode_bypass;
    st->overrides.disable_dynamic_odm = true;
    st->overrides.disable_subvp = true;
    st->overrides.hw.twait_budgeting.fclk_pstate = dml2_twait_budgeting_setting_if_needed;
    st->overrides.hw.twait_budgeting.uclk_pstate = dml2_twait_budgeting_setting_if_needed;
    st->overrides.hw.twait_budgeting.stutter_enter_exit = dml2_twait_budgeting_setting_if_needed;

    /* surface: populate_dml21_dummy_surface_cfg */
    pl->surface.plane0.width = ha;
    pl->surface.plane0.height = va;
    pl->surface.plane1.width = ha;
    pl->surface.plane1.height = va;
    pl->surface.plane0.pitch = ((ha + 127) / 128) * 128;
    pl->surface.dcc.informative.dcc_rate_plane0 = 2.0;
    pl->surface.dcc.informative.dcc_rate_plane1 = 2.0;
    pl->surface.tiling = dml2_sw_64kb_2d;

    /* plane: populate_dml21_dummy_plane_cfg */
    pl->stream_index = 0;
    pl->cursor.cursor_bpp = 32;
    pl->cursor.cursor_width = 256;
    pl->cursor.num_cursors = 1;
    pl->composition.viewport.plane0.width = w;
    pl->composition.viewport.plane0.height = h;
    pl->composition.rotation_angle = dml2_rotation_0;
    pl->composition.scaler_info.plane0.h_ratio = 1.0;
    pl->composition.scaler_info.plane0.v_ratio = 1.0;
    pl->composition.scaler_info.plane0.h_taps = 1;
    pl->composition.scaler_info.plane0.v_taps = 1;
    pl->composition.scaler_info.rect_out_width = w;
    pl->pixel_format = dml2_444_32;
    pl->overrides.gpuvm_min_page_size_kbytes = bb->gpuvm_min_page_size_kbytes;
    pl->overrides.hostvm_min_page_size_kbytes = bb->hostvm_min_page_size_kbytes;
}

int main(int argc, char **argv)
{
    int gpuvm = argc > 1 && !strcmp(argv[1], "--gpuvm");
    struct dml2_initialize_instance_in_out *init = calloc(1, sizeof *init);
    struct dml2_instance *inst = calloc(1, sizeof *inst);
    struct dml2_display_cfg *cfg = calloc(1, sizeof *cfg);
    struct dml2_display_cfg_programming *prog = calloc(1, sizeof *prog);
    char name[64], signal[16], line[512];
    unsigned long pix, ha, hf, hs, ht, va, vf, vs, vt, maxvs;
    int bad = 0;

    if (!init || !inst || !cfg || !prog) {
        fprintf(stderr, "dml_modes: out of memory\n");
        return 2;
    }
    /* dml21_populate_dml_init_params for DCN_VERSION_4_01, with this driver's PMO options */
    init->dml2_instance = inst;
    init->options.project_id = dml2_project_dcn4x_stage2_auto_drr_svp;
    init->soc_bb = dml2_socbb_dcn401;
    init->ip_caps = dml2_dcn401_max_ip_caps;
    init->options.pmo_options.disable_dyn_odm = true;
    init->options.pmo_options.disable_dyn_odm_for_multi_stream = true;
    init->options.pmo_options.disable_dyn_odm_for_stream_with_svp = true;
    init->options.pmo_options.disable_svp = true;
    init->options.pmo_options.disable_drr_clamped = true;
    init->options.pmo_options.disable_drr_var = true;
    init->options.pmo_options.disable_fams2 = true;
    if (!dml2_initialize_instance(init)) {
        fprintf(stderr, "dml_modes: dml2_initialize_instance failed\n");
        return 2;
    }
    printf("%-22s %-4s %3s %8s %8s %8s %8s %8s %5s %5s %6s %7s %s  %s\n", "mode", "sig", "ok", "dispclk", "dppclk",
           "dcfclk", "fclk", "uclk", "vsu", "vsmax", "det_kb", "urgent", "asserts", gpuvm ? "(gpuvm on)" : "(gpuvm off)");
    while (fgets(line, sizeof line, stdin)) {
        struct dml2_check_mode_supported_in_out ms = { .dml2_instance = inst, .display_config = cfg };
        struct dml2_build_mode_programming_in_out mp = { .dml2_instance = inst, .display_config = cfg, .programming = prog };
        bool ok;
        unsigned int vsu = 0, det = 0;
        unsigned long urgent = 0;

        if (sscanf(line, "%63s %15s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu", name, signal, &pix, &ha, &hf, &hs, &ht,
                   &va, &vf, &vs, &vt, &maxvs) != 12)
            continue;
        fill(cfg, signal, pix, ha, hf, hs, ht, va, vf, vs, vt, gpuvm, &init->soc_bb);
        n_warn = 0;
        first_warn[0] = 0;
        memset(prog, 0, sizeof *prog);
        /* dml21_check_mode_support / dml21_mode_check_and_programming */
        memset(&inst->scratch.check_mode_supported_locals.mode_support_params, 0,
               sizeof inst->scratch.check_mode_supported_locals.mode_support_params);
        inst->scratch.build_mode_programming_locals.mode_programming_params.programming = prog;
        ok = dml2_check_mode_supported(&ms);
        if (ok) {
            memset(&inst->scratch.build_mode_programming_locals.mode_programming_params, 0,
                   sizeof inst->scratch.build_mode_programming_locals.mode_programming_params);
            ok = dml2_build_mode_programming(&mp);
        }
        if (ok) {
            vsu = prog->stream_programming[0].global_sync.dcn4x.vstartup_lines;
            if (prog->plane_programming[0].pipe_regs[0])
                det = prog->plane_programming[0].pipe_regs[0]->det_size;
            urgent = prog->global_regs.wm_regs[0].urgent;
        }
        printf("%-22s %-4s %3s %8lu %8lu %8lu %8lu %8lu %5u %5lu %6u %7lu %u %s\n", name, signal, ok ? "yes" : "NO",
               prog->min_clocks.dcn4x.dispclk_khz, prog->plane_programming[0].min_clocks.dcn4x.dppclk_khz,
               prog->min_clocks.dcn4x.active.dcfclk_khz, prog->min_clocks.dcn4x.active.fclk_khz,
               prog->min_clocks.dcn4x.active.uclk_khz, vsu, maxvs, det * 64, urgent, n_warn, first_warn);
        if (!ok || vsu > maxvs || n_warn)
            bad = 1;
    }
    return bad;
}
