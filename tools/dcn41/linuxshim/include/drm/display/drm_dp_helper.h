#include <linux/types.h>
/* The DPCD register map (DP_LINK_BW_SET, DP_LANE0_1_STATUS, ...) is Linux's include/drm/display/drm_dp.h, which
 * tools/check-deps.sh fetches with the rest of the Linux inputs; a tree without it keeps this header empty. */
#if __has_include(<drm/display/drm_dp.h>)
#include <drm/display/drm_dp.h>
#endif
