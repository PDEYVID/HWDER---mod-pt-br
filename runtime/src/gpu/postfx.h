// Optional post-processing of the game image before it reaches the window: FXAA and sharpening,
// one compute pass (settings: F1 > Video > Post-processing). Called by the presenter (display.cpp).
#pragma once
#include "gpu/vk_loader.h"
#include "hwder/runtime.h"

namespace postfx {

bool enabled();                        // any effect switched on
bool needs_rebuild(u32 w, u32 h);      // images missing or of another size
void rebuild(u32 w, u32 h);            // with the device idle
// Records src (GENERAL layout, w x h) -> processed image, left in TRANSFER_SRC_OPTIMAL for the
// presenter's blit. VK_NULL_HANDLE when unavailable (the presenter then blits src itself).
VkImage record(VkCommandBuffer cb, VkImage src, u32 w, u32 h);

}  // namespace postfx
