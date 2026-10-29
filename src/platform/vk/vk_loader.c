/*
 * SwanStationUWP - Vulkan without the Khronos loader: Mesa's Dozen (vulkan_dzn.dll, Vulkan on Direct3D 12) ships in
 * the package and exports only the ICD entry points, so every command is looked up through vk_icdGetInstanceProcAddr.
 * (A UWP app has no system vulkan-1.dll to use.)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "../../SwanStationPS5.h"

typedef PFN_vkVoidFunction(VKAPI_PTR *PFN_icdGetInstanceProcAddr)(VkInstance, const char *);
typedef VkResult(VKAPI_PTR *PFN_icdNegotiate)(uint32_t *);

static PFN_icdGetInstanceProcAddr icd_get;
static int load_tried;

static void load_driver(void)
{
    load_tried = 1;
    /* LoadPackagedLibrary: the package's own DLLs only; Dozen's dxil.dll, spirv_to_dxil.dll and z-1.dll are found beside it */
    HMODULE dll = LoadPackagedLibrary(L"vulkan_dzn.dll", 0);
    if (!dll)
    {
        SwanStationPS5_log("vulkan: vulkan_dzn.dll could not be loaded (error %lu)", GetLastError());
        return;
    }
    PFN_icdNegotiate negotiate = (PFN_icdNegotiate)GetProcAddress(dll, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (negotiate)
    {
        uint32_t version = 5;
        negotiate(&version);
    }
    icd_get = (PFN_icdGetInstanceProcAddr)GetProcAddress(dll, "vk_icdGetInstanceProcAddr");
    if (!icd_get)
        SwanStationPS5_log("vulkan: vulkan_dzn.dll has no vk_icdGetInstanceProcAddr");
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!load_tried)
        load_driver();
    return icd_get ? icd_get(instance, name) : NULL;
}
