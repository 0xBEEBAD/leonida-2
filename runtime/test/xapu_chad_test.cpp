// Synthetic unit tests for the xapu.dll and chad.dll audio processor shims.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

using XApuConnectFn = HRESULT(WINAPI*)(void**, void*, void*);
using XApuDisconnectFn = HRESULT(WINAPI*)(void*, uint64_t);
using XApuEnqueueCommandFn = HRESULT(WINAPI*)(void*, const void*, uint32_t);
using XApuGetResultCountFn = uint32_t(WINAPI*)(void*, uint32_t);
using XApuDequeueResultFn = HRESULT(WINAPI*)(void*, void*);

using XDspConnectFn = HRESULT(WINAPI*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, void**);
using XDspActivateFn = HRESULT(WINAPI*)(void*, uint32_t, uint32_t, void*, void*);
using XDspDeactivateFn = HRESULT(WINAPI*)(void*, uint32_t, uint32_t, void*);
using XDspDisconnectFn = HRESULT(WINAPI*)(void*, uint64_t);
using XDspSubmitCommandFn = HRESULT(WINAPI*)(void*, const void*);

}  // namespace

int main() {
    auto* xapu_connect = reinterpret_cast<XApuConnectFn>(rt::shims::find("xapu.dll", "XApuConnect"));
    auto* xapu_disconnect = reinterpret_cast<XApuDisconnectFn>(rt::shims::find("xapu.dll", "XApuDisconnect"));
    auto* xapu_enqueue = reinterpret_cast<XApuEnqueueCommandFn>(rt::shims::find("xapu.dll", "XApuEnqueueCommand"));
    auto* xapu_count = reinterpret_cast<XApuGetResultCountFn>(rt::shims::find("xapu.dll", "XApuGetResultCount"));
    auto* xapu_dequeue = reinterpret_cast<XApuDequeueResultFn>(rt::shims::find("xapu.dll", "XApuDequeueResult"));

    check(xapu_connect && xapu_disconnect && xapu_enqueue && xapu_count && xapu_dequeue, "all 5 xapu exports resolve");
    check(rt::shims::owns_dll("xapu.dll"), "owns_dll(xapu.dll)");

    auto* xdsp_connect =
        reinterpret_cast<XDspConnectFn>(rt::shims::find("chad.dll", "XDspConnectWithMaximumStreamLimit"));
    auto* xdsp_activate = reinterpret_cast<XDspActivateFn>(rt::shims::find("chad.dll", "XDspActivate"));
    auto* xdsp_deactivate = reinterpret_cast<XDspDeactivateFn>(rt::shims::find("chad.dll", "XDspDeactivate"));
    auto* xdsp_disconnect = reinterpret_cast<XDspDisconnectFn>(rt::shims::find("chad.dll", "XDspDisconnect"));
    auto* xdsp_submit = reinterpret_cast<XDspSubmitCommandFn>(rt::shims::find("chad.dll", "XDspSubmitCommand"));

    check(xdsp_connect && xdsp_activate && xdsp_deactivate && xdsp_disconnect && xdsp_submit,
          "all 5 chad exports resolve");
    check(rt::shims::owns_dll("chad.dll"), "owns_dll(chad.dll)");
    if (g_failures) return 1;

    // ---- XAPU queue lifecycle & FIFO acknowledgement --------------------------------------------------------
    void* apu = nullptr;
    check(xapu_connect(nullptr, nullptr, nullptr) == E_POINTER, "XApuConnect(nullptr) -> E_POINTER");
    check(SUCCEEDED(xapu_connect(&apu, nullptr, nullptr)) && apu != nullptr, "XApuConnect allocates queue handle");
    check(xapu_count(apu, 0) == 0, "fresh queue has 0 pending results");

    uint8_t empty_res[0x50]{};
    check(FAILED(xapu_dequeue(apu, empty_res)), "XApuDequeueResult on empty queue returns failure HRESULT");
    check(xapu_dequeue(apu, nullptr) == E_POINTER, "XApuDequeueResult(nullptr) -> E_POINTER");

    uint8_t cmd1[0x40]{};
    const uint32_t op_create = 1;
    const uint32_t ctx_7 = 7;
    std::memcpy(cmd1 + 0x00, &op_create, 4);
    std::memcpy(cmd1 + 0x04, &ctx_7, 4);
    check(SUCCEEDED(xapu_enqueue(apu, cmd1, 0)), "XApuEnqueueCommand(create) succeeds");

    uint8_t cmd3[0x40]{};
    const uint32_t op_decode = 3;
    const uint32_t out_bytes = 3840;
    std::memcpy(cmd3 + 0x00, &op_decode, 4);
    std::memcpy(cmd3 + 0x04, &ctx_7, 4);
    std::memcpy(cmd3 + 0x34, &out_bytes, 4);
    check(SUCCEEDED(xapu_enqueue(apu, cmd3, 0)), "XApuEnqueueCommand(decode) succeeds");

    check(xapu_count(apu, 0) == 2, "result count matches queued commands");

    uint8_t res1[0x50]{};
    uint32_t got_op = 0, got_ctx = 0, got_bytes = 0;
    check(SUCCEEDED(xapu_dequeue(apu, res1)), "dequeue first result");
    std::memcpy(&got_op, res1 + 0x00, 4);
    std::memcpy(&got_ctx, res1 + 0x04, 4);
    check(got_op == 1 && got_ctx == 7, "first dequeued result echoes opcode=1 ctx=7");

    uint8_t res2[0x50]{};
    check(SUCCEEDED(xapu_dequeue(apu, res2)), "dequeue second result");
    std::memcpy(&got_op, res2 + 0x00, 4);
    std::memcpy(&got_ctx, res2 + 0x04, 4);
    std::memcpy(&got_bytes, res2 + 0x34, 4);
    check(got_op == 3 && got_ctx == 7 && got_bytes == 3840, "second result echoes opcode=3 ctx=7 output_bytes=3840");
    check(xapu_count(apu, 0) == 0, "result count returns to 0 after draining");

    check(SUCCEEDED(xapu_disconnect(apu, 0x20)), "XApuDisconnect succeeds");
    check(FAILED(xapu_disconnect(apu, 0)), "double XApuDisconnect rejects stale handle");
    check(FAILED(xapu_enqueue(apu, cmd1, 0)), "enqueue after disconnect fails closed");
    check(xapu_count(apu, 0) == 0, "get_result_count after disconnect returns 0 safely");

    // ---- CHAD DSP lifecycle ----------------------------------------------------------------------------------
    void* dsp = reinterpret_cast<void*>(1);
    check(xdsp_connect(nullptr, 0, 0, 0x80, 1, nullptr) == E_POINTER, "XDspConnect null out_handle -> E_POINTER");
    check(xdsp_connect(nullptr, 0, 0, 0, 1, &dsp) == E_INVALIDARG && dsp == nullptr,
          "XDspConnect max_streams=0 -> E_INVALIDARG & clears handle");
    check(xdsp_connect(nullptr, 0, 0, 0x80, 1, &dsp) == S_OK && dsp != nullptr,
          "XDspConnect(0x80 streams) returns S_OK");
    check(xdsp_activate(dsp, 0, 0, nullptr, nullptr) == S_OK, "XDspActivate returns S_OK");
    uint8_t dsp_cmd[16]{};
    check(xdsp_submit(dsp, nullptr) == E_INVALIDARG, "XDspSubmitCommand(nullptr) -> E_INVALIDARG");
    check(xdsp_submit(dsp, dsp_cmd) == S_OK, "XDspSubmitCommand returns S_OK");
    check(xdsp_deactivate(dsp, 0, 0, nullptr) == S_OK, "XDspDeactivate returns S_OK");
    check(xdsp_disconnect(dsp, 0) == S_OK, "XDspDisconnect returns S_OK");
    check(FAILED(xdsp_activate(dsp, 0, 0, nullptr, nullptr)), "XDspActivate after disconnect fails");
    check(FAILED(xdsp_submit(dsp, dsp_cmd)), "XDspSubmitCommand after disconnect fails");

    if (g_failures == 0) std::printf("xapu_chad_test: OK\n");
    return g_failures == 0 ? 0 : 1;
}
