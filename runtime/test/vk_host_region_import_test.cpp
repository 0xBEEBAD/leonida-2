// Capability probe for RT-TITLE-HOST-REGION: import one committed host region as a buffer-device-addressable
// Vulkan buffer with VK_EXT_external_memory_host. The optional argument is the region size in MiB (default 185).
// A successful import is checked in both directions: GPU reads CPU writes, and GPU writes are visible to the CPU.
// A driver import-budget refusal is reported as a capability result, not as a test failure; the arena fallback remains
// valid. No shaders or game data.
#include "vk_test_device.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#ifndef VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME
#define VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME "VK_EXT_external_memory_host"
#endif

namespace {

constexpr std::uint64_t kMiB = 1ull << 20;
constexpr VkDeviceSize kSampleBytes = 4096;

struct Region {
    void* address = nullptr;
    ~Region() {
        if (address) VirtualFree(address, 0, MEM_RELEASE);
    }
};

struct Imported {
    vktest::Device& vk;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    ~Imported() {
        if (buffer) vk.fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(vk.device, buffer, nullptr);
        if (memory) vk.fn<PFN_vkFreeMemory>("vkFreeMemory")(vk.device, memory, nullptr);
    }
};

void* allocate_region(std::size_t size, std::size_t alignment) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    if (alignment <= info.dwAllocationGranularity)
        return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.Alignment = alignment;
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    const auto alloc2 = reinterpret_cast<decltype(&VirtualAlloc2)>(
        GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2"));
    if (!alloc2) return nullptr;
    return alloc2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &parameter, 1);
}

int fail(const char* message) {
    std::fprintf(stderr, "host-region import: %s\n", message);
    return 1;
}

int run(vktest::Device& vk, std::uint64_t mib) {
    if (!mib || mib > (std::numeric_limits<std::size_t>::max)() / kMiB) return fail("invalid region size");
    const auto size = static_cast<VkDeviceSize>(mib * kMiB);
    const auto alignment = static_cast<std::size_t>(vk.host_pointer_alignment);
    if (!alignment || (alignment & (alignment - 1))) return fail("invalid imported-host-pointer alignment");

    Region region;
    region.address = allocate_region(static_cast<std::size_t>(size), alignment);
    if (!region.address) return fail("VirtualAlloc failed");
    if (reinterpret_cast<std::uintptr_t>(region.address) & (alignment - 1))
        return fail("host region is not aligned to minImportedHostPointerAlignment");

    Imported imported{vk};
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vk.fn<PFN_vkCreateBuffer>("vkCreateBuffer")(vk.device, &buffer_info, nullptr, &imported.buffer);
    if (result != VK_SUCCESS) {
        std::printf("host-region import: size=%llu MiB result=buffer-refused vkResult=%d\n",
                    static_cast<unsigned long long>(mib), static_cast<int>(result));
        return 77;
    }

    VkMemoryRequirements requirements{};
    vk.fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(vk.device, imported.buffer,
                                                                               &requirements);
    if (requirements.size > size) {
        std::printf("host-region import: size=%llu MiB result=unsupported requirements=%llu bytes\n",
                    static_cast<unsigned long long>(mib), static_cast<unsigned long long>(requirements.size));
        return 77;
    }

    VkMemoryHostPointerPropertiesEXT host{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    result = vk.fn<PFN_vkGetMemoryHostPointerPropertiesEXT>("vkGetMemoryHostPointerPropertiesEXT")(
        vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, region.address, &host);
    if (result != VK_SUCCESS || !host.memoryTypeBits) {
        std::printf("host-region import: size=%llu MiB result=unsupported vkResult=%d memoryTypeBits=0x%x\n",
                    static_cast<unsigned long long>(mib), static_cast<int>(result), host.memoryTypeBits);
        return 77;
    }

    VkPhysicalDeviceMemoryProperties memory_properties{};
    vk.instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(vk.physical,
                                                                                                      &memory_properties);
    std::uint32_t memory_type = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if (!(host.memoryTypeBits & requirements.memoryTypeBits & (1u << i))) continue;
        const auto flags = memory_properties.memoryTypes[i].propertyFlags;
        if ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
        std::printf("host-region import: size=%llu MiB result=unsupported no host-visible coherent type\n",
                    static_cast<unsigned long long>(mib));
        return 77;
    }

    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = region.address;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &import};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type;
    result = vk.fn<PFN_vkAllocateMemory>("vkAllocateMemory")(vk.device, &allocate, nullptr, &imported.memory);
    if (result != VK_SUCCESS) {
        const bool budget_refusal = result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY;
        std::printf("host-region import: size=%llu MiB result=%s vkResult=%d alignment=%llu requirements=%llu "
                    "memoryTypeBits=0x%x type=%u\n",
                    static_cast<unsigned long long>(mib), budget_refusal ? "refused" : "failed",
                    static_cast<int>(result), static_cast<unsigned long long>(requirements.alignment),
                    static_cast<unsigned long long>(requirements.size), host.memoryTypeBits, memory_type);
        return budget_refusal ? 0 : 1;
    }
    result = vk.fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(vk.device, imported.buffer, imported.memory, 0);
    if (result != VK_SUCCESS) return fail("vkBindBufferMemory failed");

    VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = imported.buffer;
    const auto address = vk.fn<PFN_vkGetBufferDeviceAddress>("vkGetBufferDeviceAddress")(vk.device, &address_info);
    if (!address) return fail("imported buffer has no device address");

    auto* bytes = static_cast<std::uint8_t*>(region.address);
    for (VkDeviceSize i = 0; i < kSampleBytes; ++i) {
        bytes[i] = static_cast<std::uint8_t>(i * 131u + 7u);
        bytes[size - kSampleBytes + i] = static_cast<std::uint8_t>(i * 197u + 11u);
    }

    void* readback_map = nullptr;
    const VkBuffer readback = vk.buffer(2 * kSampleBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback_map);
    if (!readback || !readback_map) return fail("readback buffer creation failed");
    const auto copy = vk.fn<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    const bool copied = vk.run([&](VkCommandBuffer command) {
        VkBufferCopy regions[2]{};
        regions[0].srcOffset = 0;
        regions[0].dstOffset = 0;
        regions[0].size = kSampleBytes;
        regions[1].srcOffset = size - kSampleBytes;
        regions[1].dstOffset = kSampleBytes;
        regions[1].size = kSampleBytes;
        copy(command, imported.buffer, readback, 2, regions);
        return true;
    });
    if (!copied) return fail("GPU copy from imported memory failed");
    const auto* readback_bytes = static_cast<const std::uint8_t*>(readback_map);
    if (std::memcmp(readback_bytes, bytes, kSampleBytes) != 0 ||
        std::memcmp(readback_bytes + kSampleBytes, bytes + size - kSampleBytes, kSampleBytes) != 0)
        return fail("GPU did not read the CPU's writes");

    const auto fill = vk.fn<PFN_vkCmdFillBuffer>("vkCmdFillBuffer");
    const bool filled = vk.run([&](VkCommandBuffer command) {
        fill(command, imported.buffer, 0, kSampleBytes, 0xa5a5a5a5u);
        fill(command, imported.buffer, size - kSampleBytes, kSampleBytes, 0x5a5a5a5au);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vk.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
            command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0,
            nullptr);
        return true;
    });
    if (!filled) return fail("GPU fill of imported memory failed");
    for (VkDeviceSize i = 0; i < kSampleBytes; ++i) {
        if (bytes[i] != 0xa5 || bytes[size - kSampleBytes + i] != 0x5a)
            return fail("CPU did not see the GPU's writes");
    }

    std::printf("host-region import: size=%llu MiB result=imported alignment=%llu requirements=%llu "
                "memoryTypeBits=0x%x type=%u bda=0x%llx cpu-gpu-coherence=ok\n",
                static_cast<unsigned long long>(mib), static_cast<unsigned long long>(requirements.alignment),
                static_cast<unsigned long long>(requirements.size), host.memoryTypeBits, memory_type,
                static_cast<unsigned long long>(address));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t mib = 185;
    if (argc > 1) {
        char* end = nullptr;
        mib = std::strtoull(argv[1], &end, 10);
        if (!end || *end) {
            std::fprintf(stderr, "usage: vk_host_region_import_test [MiB]\n");
            return 1;
        }
    }
    vktest::Device vk;
    vk.request_buffer_device_address = true;
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;
    if (!vk.host_pointer_alignment || !vk.buffer_device_address) {
        std::printf("host-region import: skipped, need VK_EXT_external_memory_host and bufferDeviceAddress\n");
        return 77;
    }
    const int result = run(vk, mib);
    if (vk.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", vk.validation_errors());
        return 1;
    }
    return result;
}
