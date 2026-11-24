// Checks D3D12ConfigureVirtualMemoryX in runtime/shims/d3d12_xs.cpp without needing a GPU or game data.
#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <memory>

#include "shims.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

struct XboxVirtualMemoryConfig {
    uint32_t flags;
    uint32_t page_table_pages;
    uint32_t preserved_bytes;
    uint32_t reserved;
    uint64_t lowest;
    uint64_t highest;
};
static_assert(sizeof(XboxVirtualMemoryConfig) == 0x20);

}  // namespace

int main() {
    using Fn = HRESULT(WINAPI*)(void*);
    const auto fn = reinterpret_cast<Fn>(rt::shims::find("d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX"));
    check(fn != nullptr, "D3D12ConfigureVirtualMemoryX resolves through shim registry");
    if (!fn) return 1;

    check(fn(nullptr) == E_INVALIDARG, "D3D12ConfigureVirtualMemoryX(nullptr) returns E_INVALIDARG");

    XboxVirtualMemoryConfig cfg{1u, 7u, 0x1000u, 0u, 0u, 0u};
    check(fn(&cfg) == S_OK, "D3D12ConfigureVirtualMemoryX(valid) returns S_OK");
    check(cfg.flags == 1u && cfg.page_table_pages == 7u && cfg.preserved_bytes == 0x1000u,
          "input words are preserved");
    check(cfg.lowest > 0 && cfg.lowest < cfg.highest, "output VA range is non-empty (0 < lowest < highest)");

    auto probe = std::make_unique<uint8_t>(42);
    const auto addr = reinterpret_cast<uint64_t>(probe.get());
    check(addr >= cfg.lowest && addr <= cfg.highest, "a live user-space heap address lies within [lowest, highest]");

    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
