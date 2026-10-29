/*
 * SwanStation Xbox - frame generation (AMD FSR 3's interpolation, third_party/framegen) is not built into the Xbox
 * package yet: these stand in for it, so the presenter turns the setting off ("can't start") instead of failing.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fg_bridge.h"

#include <stdio.h>
#include <string.h>

FgInterpolator *ssfg_create(PFN_vkVoidFunction (*get_instance_proc)(VkInstance, const char *), VkInstance instance,
                            VkPhysicalDevice gpu, VkDevice device, uint32_t width, uint32_t height, VkFormat format,
                            char *error, size_t error_size)
{
    (void)get_instance_proc, (void)instance, (void)gpu, (void)device, (void)width, (void)height, (void)format;
    snprintf(error, error_size, "frame generation is not in the Xbox build");
    return NULL;
}
void ssfg_destroy(FgInterpolator *fg) { (void)fg; }
void ssfg_prepare(FgInterpolator *fg, VkCommandBuffer cmd) { (void)fg, (void)cmd; }
VkImage ssfg_frame_image(FgInterpolator *fg) { (void)fg; return VK_NULL_HANDLE; }
VkImageView ssfg_frame_view(FgInterpolator *fg) { (void)fg; return VK_NULL_HANDLE; }
bool ssfg_record(FgInterpolator *fg, VkCommandBuffer cmd, bool reset) { (void)fg, (void)cmd, (void)reset; return false; }
VkImageView ssfg_output_view(FgInterpolator *fg) { (void)fg; return VK_NULL_HANDLE; }
bool ssfg_record_n(FgInterpolator *fg, VkCommandBuffer cmd, bool reset, uint32_t generated)
{
    (void)fg, (void)cmd, (void)reset, (void)generated;
    return false;
}
VkImageView ssfg_generated_view(FgInterpolator *fg, uint32_t i) { (void)fg, (void)i; return VK_NULL_HANDLE; }
void ssfg_advance(FgInterpolator *fg) { (void)fg; }

FgPacing *ssfg_pacing_create(void) { return NULL; }
void ssfg_pacing_destroy(FgPacing *pacing) { (void)pacing; }
FgDecision ssfg_pacing_frame(FgPacing *pacing, uint32_t vsyncs, uint32_t already, double core_hz, double display_hz,
                             double now, double speed, bool nominal, uint32_t max_generated)
{
    FgDecision d;
    (void)pacing, (void)vsyncs, (void)already, (void)core_hz, (void)display_hz, (void)now, (void)speed, (void)nominal,
        (void)max_generated;
    memset(&d, 0, sizeof(d));
    return d;
}
const char *ssfg_pacing_state_name(int state) { (void)state; return "off"; }
const char *ssfg_pacing_event(FgPacing *pacing) { (void)pacing; return NULL; }
