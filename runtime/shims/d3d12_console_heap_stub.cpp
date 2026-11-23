// The standalone proxy tests (root_sig_test, device_proxy_test, xs_proxy_mock_test) compile shims/d3d12_xs_device.cpp,
// whose Xbox slot-14 console path calls rt::d3d12::create_console_descriptor_heap. The real definition lives in
// d3d12_vulkan_device.cpp, which those tests do not link (it pulls in the Vulkan device). They never create a console
// descriptor heap, so this inert definition satisfies the reference. The runtime links the real one instead.
#include <windows.h>
#include <d3d12.h>

#include <cstdint>

namespace rt::d3d12 {
HRESULT create_console_descriptor_heap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                       D3D12_GPU_VIRTUAL_ADDRESS console_address, std::uint64_t console_size,
                                       REFIID riid, void** out) {
    (void)device;
    (void)desc;
    (void)console_address;
    (void)console_size;
    (void)riid;
    if (out) *out = nullptr;
    return E_NOTIMPL;
}
}  // namespace rt::d3d12
