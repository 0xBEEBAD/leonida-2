// Console Xbox CreateDescriptorHeap (issue #313 comment 7107265058) passes a 16-byte PC
// D3D12_DESCRIPTOR_HEAP_DESC followed by address at +0x10 and size at +0x18. The title asserts
// GetCPUDescriptorHandleForHeapStart() == address. This tests the explicit Leonida console-heap
// creation path used by the Xbox device proxy slot 14. It does not cover title-memory aliasing /
// section 3.3 backing.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"

#include <cstdint>
#include <cstdio>
#include <wrl/client.h>

namespace {

using Microsoft::WRL::ComPtr;

bool check_console_addressed_heap(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 8;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NodeMask = 0;
    const D3D12_GPU_VIRTUAL_ADDRESS address = 0x112233440000ull;
    const std::uint64_t size = 8ull * 32ull;

    ComPtr<ID3D12DescriptorHeap> heap;
    const HRESULT hr = rt::d3d12::create_console_descriptor_heap(
        device, &desc, address, size, IID_PPV_ARGS(heap.GetAddressOf()));
    if (FAILED(hr) || !heap) {
        std::fprintf(stderr, "create_console_descriptor_heap failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    const auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    if (cpu.ptr != static_cast<SIZE_T>(address)) {
        std::fprintf(stderr, "CPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(cpu.ptr), static_cast<unsigned long long>(address));
        return false;
    }
    const auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
    if (gpu.ptr != address) {
        std::fprintf(stderr, "GPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(gpu.ptr), static_cast<unsigned long long>(address));
        return false;
    }
    const D3D12_DESCRIPTOR_HEAP_DESC got = heap->GetDesc();
    if (got.Type != desc.Type || got.NumDescriptors != desc.NumDescriptors || got.Flags != desc.Flags ||
        got.NodeMask != 1) {
        std::fprintf(stderr, "GetDesc mismatch\n");
        return false;
    }
    return true;
}

bool check_slot14_heap(ID3D12Device* proxy, std::uint64_t address, bool addressed) {
    rt::d3d12_xs::ConsoleDescriptorHeapDesc console{};
    console.desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    console.desc.NumDescriptors = 4;
    console.desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    console.address = address;
    console.size = 4ull * 32ull;

    ComPtr<ID3D12DescriptorHeap> heap;
    const HRESULT hr = proxy->CreateDescriptorHeap(&console.desc, IID_PPV_ARGS(heap.GetAddressOf()));
    if (FAILED(hr) || !heap) {
        std::fprintf(stderr, "proxy CreateDescriptorHeap(console) failed: 0x%08lx\n",
                     static_cast<unsigned long>(hr));
        return false;
    }
    // The Xbox heap proxy returns these handles in RAX (slots 9 and 10).
    void** vtable = *reinterpret_cast<void***>(heap.Get());
    using GetHandle = std::uint64_t(WINAPI*)(ID3D12DescriptorHeap*);
    const auto cpu = reinterpret_cast<GetHandle>(vtable[9])(heap.Get());
    const auto gpu = reinterpret_cast<GetHandle>(vtable[10])(heap.Get());
    if (addressed && (cpu != address || gpu != address)) {
        std::fprintf(stderr, "proxy CPU/GPU heap starts 0x%llx/0x%llx, expected 0x%llx\n",
                     static_cast<unsigned long long>(cpu), static_cast<unsigned long long>(gpu),
                     static_cast<unsigned long long>(address));
        return false;
    }
    if (!addressed && (!cpu || (address && (cpu == address || gpu == address)))) {
        std::fprintf(stderr, "ordinary heap did not get its own descriptor storage\n");
        return false;
    }
    return true;
}

bool check_xs_proxy_slot14(ComPtr<ID3D12Device> device, bool xbox_layout) {
    ComPtr<ID3D12Device> proxy;
    auto provider = xbox_layout ? &rt::d3d12::create_console_descriptor_heap : nullptr;
    if (FAILED(rt::d3d12_xs::create_device_proxy(device.Get(), IID_PPV_ARGS(proxy.GetAddressOf()), provider))) {
        std::fprintf(stderr, "create_device_proxy failed\n");
        return false;
    }
    // The proxy takes the inner reference on success, just as in D3D12XboxCreateDevice.
    ID3D12Device* inner = device.Detach();
    rt::d3d12::set_device_outer(inner, proxy.Get());
    // Identical trailing words on a PC-layout proxy must leave the ordinary heap's storage intact.
    const bool ok = check_slot14_heap(proxy.Get(), 0x556677880000ull, xbox_layout) &&
                    check_slot14_heap(proxy.Get(), 0, false);
    rt::d3d12::set_device_outer(inner, nullptr);
    return ok;
}

bool check_normal_heap_unchanged(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 4;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.GetAddressOf()));
    if (FAILED(hr) || !heap) {
        std::fprintf(stderr, "CreateDescriptorHeap(normal) failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    const auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    if (cpu.ptr == 0) {
        std::fprintf(stderr, "normal heap CPU start is null\n");
        return false;
    }
    return true;
}

}  // namespace

int main() {
    vktest::Device vk;
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;

    ComPtr<ID3D12Device> device;
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = vk.instance_proc;
    info.instance = vk.instance;
    info.physical = vk.physical;
    info.device = vk.device;
    info.queue = vk.queue;
    info.queue_family = vk.family;
    if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(device.GetAddressOf()))) || !device) {
        std::fprintf(stderr, "create_vulkan_device failed\n");
        return 1;
    }

    if (!check_normal_heap_unchanged(device.Get())) return 1;
    if (!check_console_addressed_heap(device.Get())) return 1;
    if (!check_xs_proxy_slot14(device, true) || !check_xs_proxy_slot14(device, false)) return 1;
    std::fprintf(stderr, "ok\n");
    return 0;
}
