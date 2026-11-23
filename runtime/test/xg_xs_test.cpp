// Synthetic test for the linear Xbox texture computer integrated in BulkCompatibility v6.
// Tests the actual registered xg_xs.dll names, a one-level texture layout, a
// subresource copy, and lifetime through the guest's observed vtable slots.
#include <windows.h>
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "shims.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
using Create = HRESULT(WINAPI*)(const D3D12_RESOURCE_DESC*, void**);
using Suggest = HRESULT(WINAPI*)(const D3D12_RESOURCE_DESC*, std::uintptr_t, std::uint32_t*);
using RawSlot = std::uintptr_t (*)(void*, std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                  std::uintptr_t, std::uintptr_t, std::uintptr_t);
std::uintptr_t invoke(void* obj, int slot, std::uintptr_t a=0, std::uintptr_t b=0,
                      std::uintptr_t c=0, std::uintptr_t d=0, std::uintptr_t e=0,
                      std::uintptr_t f=0) {
    auto** vtable = *reinterpret_cast<void***>(obj);
    return reinterpret_cast<RawSlot>(vtable[slot])(obj,a,b,c,d,e,f);
}
}
int main() {
    auto create = reinterpret_cast<Create>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    auto suggest = reinterpret_cast<Suggest>(rt::shims::find("xg_xs.dll", "XGSuggestSwizzleMode"));
    check(create && suggest, "both Xbox texture imports resolve");
    if (!create || !suggest) return 1;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = 16; d.Height = 8; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    std::uint32_t swizzle = 0xdeadbeef;
    check(suggest(&d, 0, &swizzle) == S_OK && swizzle == 0x100u,
          "XGSuggestSwizzleMode sets linear layout 0x100");
    check(suggest(&d, 0, nullptr) == E_POINTER, "swizzle rejects missing output");
    void* computer = nullptr;
    check(create(&d, &computer) == S_OK && computer != nullptr,
          "XGCreateTextureComputer creates the layout object");
    if (!computer) return 1;
    // The struct is 0x18 header + 4 * 0x6c8 planes + optional dimension.
    std::array<std::uint8_t, 0x2000> layout{};
    auto get_layout = static_cast<HRESULT>(static_cast<std::uint32_t>(invoke(
        computer, 2, reinterpret_cast<std::uintptr_t>(layout.data()))));
    check(get_layout == S_OK, "GetResourceLayout (slot 2) completes");
    std::uint64_t total = 0;
    std::memcpy(&total, layout.data(), sizeof total);
    check(total >= 16u * 8u * 4u && (total % 65536u) == 0,
          "reported linear layout has 64 KiB resource alignment");
    const size_t tiled_bytes = static_cast<size_t>(total && total <= 1024*1024 ? total : 65536);
    std::vector<std::uint8_t> tiled(tiled_bytes);
    std::array<std::uint8_t, 16*8*4> linear{};
    for (size_t i=0;i<linear.size();++i) linear[i]=static_cast<std::uint8_t>((i*19u)^0x5a);
    auto copy = static_cast<HRESULT>(static_cast<std::uint32_t>(invoke(
        computer, 8, reinterpret_cast<std::uintptr_t>(tiled.data()), 0, 0,
        reinterpret_cast<std::uintptr_t>(linear.data()), 16*4, 16*8*4)));
    check(copy == S_OK, "CopyIntoSubresource (slot 8) copies the texture");
    bool rows_match=true;
    for (size_t y=0;y<8;++y) {
        if (std::memcmp(tiled.data()+y*256, linear.data()+y*64, 64)) rows_match=false;
    }
    check(rows_match, "linear rows match 256-byte pitch");
    auto result = invoke(computer, 1);
    check(result == 0, "Release (slot 1) frees the layout object");
    std::printf("xg_xs_test: %s (%d failure(s))\n",failures ? "FAILED":"OK", failures);
    return failures ? 1:0;
}
