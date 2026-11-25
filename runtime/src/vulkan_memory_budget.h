#pragma once

// VK_EXT_memory_budget: how much GPU memory this process uses and may use, read from the Vulkan device itself. The
// title HUD (shims/title_hud.h, LEONIDA_TITLE_HUD) shows it; DXGI's QueryVideoMemoryInfo, which it read before, is 0
// under Wine. The sums depend only on the two Vulkan structures, so a test can feed them made-up heaps.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <cstdint>

namespace leonida {

struct VramBudget {
    // False when there is nothing to show: no VK_EXT_memory_budget on the device, no query, or a device that reports
    // no budget for any DEVICE_LOCAL heap. Callers show "n/a" then, not a 0.
    bool available = false;
    std::uint64_t local_usage = 0;   // heapUsage summed over the DEVICE_LOCAL heaps
    std::uint64_t local_budget = 0;  // heapBudget summed over the DEVICE_LOCAL heaps
    std::uint64_t shared_usage = 0;  // heapUsage summed over the other heaps: system memory the GPU uses
};

inline VramBudget sum_vram_budget(const VkPhysicalDeviceMemoryProperties& memory,
                                  const VkPhysicalDeviceMemoryBudgetPropertiesEXT& budget) {
    VramBudget out;
    for (std::uint32_t i = 0; i < memory.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; ++i) {
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            out.local_usage += budget.heapUsage[i];
            out.local_budget += budget.heapBudget[i];
        } else {
            out.shared_usage += budget.heapUsage[i];
        }
    }
    out.available = out.local_budget != 0;
    return out;
}

// Reads the budget now. `physical` must have VK_EXT_memory_budget (enabled on the logical device, as the runtime's
// device does). One vkGetPhysicalDeviceMemoryProperties2 call, which needs no external synchronization, so another
// thread (the HUD's) may call it while the device is in use.
inline VramBudget query_vram_budget(PFN_vkGetPhysicalDeviceMemoryProperties2 get_memory_properties2,
                                    VkPhysicalDevice physical) {
    if (!get_memory_properties2 || physical == VK_NULL_HANDLE) return {};
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
    get_memory_properties2(physical, &properties);
    return sum_vram_budget(properties.memoryProperties, budget);
}

}  // namespace leonida
