// shims/title_hud.h: LEONIDA_TITLE_HUD's counters reach the title bar of a window owned by another thread, and a
// window that stops pumping messages does not hold the HUD thread. The memory figures: VK_EXT_memory_budget's heaps
// summed, DXGI's as the fallback, "n/a" where neither has one, and the private bytes Wine reports. A real Win32
// window and made-up budgets; no GPU needed (d3d12_vram_budget_test reads a real device's).
#include "title_hud.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#define CHECK(condition) do {                                        \
    if (!(condition)) {                                              \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                    \
    }                                                                \
} while (0)

static std::wstring title_of(HWND window) {
    wchar_t text[512] = {};
    InternalGetWindowText(window, text, 512);  // no message: the window's own thread may be busy
    return text;
}

// Pumps this thread's messages for `ms`, presenting every 16 ms when `present` is set.
static void pump(DWORD ms, rt::d3d12_xs::TitleHud* present) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (present) present->note_present();
        Sleep(16);
    }
}

constexpr std::uint64_t kGiB = 1ull << 30;

// The memory figures without a window: VK_EXT_memory_budget's heaps summed, DXGI's answer, the private bytes, and
// the text for each, "n/a" where there is no figure. Made-up values, so it is the same on every host.
static int check_figures() {
    using rt::d3d12_xs::TitleHud;
    wchar_t text[96];

    // Two device-local heaps (VRAM and a BAR window) and system memory.
    VkPhysicalDeviceMemoryProperties heaps{};
    heaps.memoryHeapCount = 3;
    heaps.memoryHeaps[0] = {16 * kGiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    heaps.memoryHeaps[1] = {32 * kGiB, 0};
    heaps.memoryHeaps[2] = {kGiB / 4, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    budget.heapUsage[0] = 3 * kGiB + kGiB / 4;
    budget.heapBudget[0] = 15 * kGiB + kGiB * 3 / 4;
    budget.heapUsage[1] = kGiB / 2;
    budget.heapBudget[1] = 24 * kGiB;
    budget.heapUsage[2] = kGiB / 4;
    budget.heapBudget[2] = kGiB / 4;
    budget.heapUsage[3] = 7 * kGiB;  // past memoryHeapCount: not counted
    budget.heapBudget[3] = 7 * kGiB;
    const leonida::VramBudget summed = leonida::sum_vram_budget(heaps, budget);
    CHECK(summed.available);
    CHECK(summed.local_usage == 3 * kGiB + kGiB / 2 && summed.local_budget == 16 * kGiB);
    CHECK(summed.shared_usage == kGiB / 2);
    TitleHud::format_vram(text, 96, summed);
    CHECK(std::wstring(text) == L"VRAM 3.5/16.0 GB +0.5 shared");

    // A device that reports no budget for its device-local heaps has no figure, not a 0.
    const VkPhysicalDeviceMemoryBudgetPropertiesEXT empty{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    CHECK(!leonida::sum_vram_budget(heaps, empty).available);
    TitleHud::format_vram(text, 96, leonida::VramBudget{});
    CHECK(std::wstring(text) == L"VRAM n/a");

    // DXGI: Wine's QueryVideoMemoryInfo answers S_OK with no usage, which is not a figure; Windows' is.
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, non_local{};
    local.Budget = 24 * kGiB;
    CHECK(!TitleHud::dxgi_budget(local, non_local).available);
    local.CurrentUsage = 2 * kGiB;
    non_local.CurrentUsage = kGiB;
    const leonida::VramBudget dxgi = TitleHud::dxgi_budget(local, non_local);
    CHECK(dxgi.available && dxgi.local_usage == 2 * kGiB && dxgi.local_budget == 24 * kGiB &&
          dxgi.shared_usage == kGiB);

    // The private bytes: Wine fills PagefileUsage and leaves PrivateUsage 0; Windows fills both with the same value.
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.WorkingSetSize = kGiB;
    TitleHud::format_ram(text, 96, &counters);
    CHECK(std::wstring(text) == L"RAM n/a (ws 1.0)");
    counters.PagefileUsage = 5 * kGiB / 2;
    CHECK(TitleHud::private_bytes(counters) == 5 * kGiB / 2);
    TitleHud::format_ram(text, 96, &counters);
    CHECK(std::wstring(text) == L"RAM 2.5 GB (ws 1.0)");
    counters.PrivateUsage = 5 * kGiB / 2;
    CHECK(TitleHud::private_bytes(counters) == 5 * kGiB / 2);
    TitleHud::format_ram(text, 96, nullptr);
    CHECK(std::wstring(text) == L"RAM n/a");

    // This process's own counters: its private bytes are never 0, on Windows or under Wine.
    PROCESS_MEMORY_COUNTERS_EX own{};
    own.cb = sizeof own;
    CHECK(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&own), sizeof own));
    std::fprintf(stderr, "own counters: PrivateUsage %llu, PagefileUsage %llu, WorkingSetSize %llu\n",
                 (unsigned long long)own.PrivateUsage, (unsigned long long)own.PagefileUsage,
                 (unsigned long long)own.WorkingSetSize);
    CHECK(TitleHud::private_bytes(own) > 0);
    return 0;
}

int main() {
    if (const int failed = check_figures()) return failed;
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"LeonidaTitleHudTest";
    CHECK(RegisterClassW(&cls));
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE, cls.lpszClassName, L"before", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                  nullptr, nullptr, cls.hInstance, nullptr);
    CHECK(window);
    {
        rt::d3d12_xs::TitleHud hud(window);
        // Presents at ~60 Hz for 1.5 s: the title shows a frame rate and the counters.
        pump(1500, &hud);
        const std::wstring text = title_of(window);
        std::fwprintf(stderr, L"title: %ls\n", text.c_str());
        CHECK(text.rfind(L"Leonida | ", 0) == 0);
        CHECK(text.find(L" fps ") != std::wstring::npos && text.find(L" ms (worst ") != std::wstring::npos);
        CHECK(text.find(L"RAM ") != std::wstring::npos && text.find(L"VRAM ") != std::wstring::npos);
        // The private bytes are read under Wine as well ("VRAM n/a" may show: no device behind this HUD).
        CHECK(text.find(L"| RAM n/a") == std::wstring::npos);
        CHECK(text.find(L"PSO -") != std::wstring::npos);  // no LEONIDA_MEM_PROBE: no pipeline counters
        const int fps = _wtoi(text.c_str() + 10);
        CHECK(fps >= 10 && fps <= 120);  // a loose band: Sleep(16) on a loaded machine is not 60 Hz

        // Presents stop while the window still pumps: the title says how long since the last frame.
        pump(1300, nullptr);
        const std::wstring stalled = title_of(window);
        std::fwprintf(stderr, L"title: %ls\n", stalled.c_str());
        CHECK(stalled.find(L"0 fps, no frame for ") != std::wstring::npos);

        // The window's thread stops pumping: the HUD's SendMessageTimeout gives up and the HUD thread is not stuck,
        // so the destructor below returns.
        Sleep(1200);
    }
    {
        // A VRAM source (the device's VK_EXT_memory_budget in the runtime) is what the title shows, and it is read
        // at most once per refresh, on the HUD's thread.
        std::atomic<unsigned> reads{0};
        rt::d3d12_xs::TitleHud hud(window, [&reads] {
            ++reads;
            leonida::VramBudget fake;
            fake.available = true;
            fake.local_usage = 3 * kGiB + kGiB / 2;
            fake.local_budget = 20 * kGiB;
            fake.shared_usage = kGiB / 2;
            return fake;
        });
        pump(1300, &hud);
        const std::wstring text = title_of(window);
        std::fwprintf(stderr, L"title: %ls\n", text.c_str());
        CHECK(text.find(L"| VRAM 3.5/20.0 GB +0.5 shared |") != std::wstring::npos);
        CHECK(reads.load() >= 1 && reads.load() <= 3);  // 1.3 s at one refresh per 500 ms
    }
    {
        // A source with no figure (a device without VK_EXT_memory_budget): DXGI's figure where it has one (Windows),
        // else "n/a" (Wine, whose DXGI answers 0; check_figures covers that choice).
        rt::d3d12_xs::TitleHud hud(window, [] { return leonida::VramBudget{}; });
        pump(700, &hud);
        const std::wstring text = title_of(window);
        std::fwprintf(stderr, L"title: %ls\n", text.c_str());
        CHECK(text.find(L"| VRAM ") != std::wstring::npos && text.find(L"VRAM 3.5/20.0") == std::wstring::npos);
    }
    const ULONGLONG before = GetTickCount64();
    {
        rt::d3d12_xs::TitleHud quick(window);
    }  // stops at once, without waiting a whole interval
    CHECK(GetTickCount64() - before < 1000);
    DestroyWindow(window);
    std::printf("title_hud: counters in the title bar of another thread's window; a busy window does not hold it\n");
    return 0;
}
