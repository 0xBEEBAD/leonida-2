// Hardware audio processor (xapu.dll) and DSP (chad.dll) shims for Xbox Series builds.
//
// Xbox titles connect an XAPU command/result queue (opcodes to create, decode, flush, and release voice
// contexts) and a CHAD DSP session capped at a configured stream limit. There is no console ACP/DSP
// coprocessor on a PC host, so this shim validates handles/arguments cleanly, tracks active sessions under
// SRWLOCKs (constant-initialized, zero _Init_thread_header TLS guards), acknowledges XAPU commands in FIFO
// order so voice-pool state machines advance and recycle slots without stalling, and reports S_OK for valid
// CHAD DSP control operations.
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <new>
#include <vector>

#include "shims.h"

namespace rt::xapu_chad {
namespace {

struct SrwLockGuard {
    SRWLOCK* lock;
    explicit SrwLockGuard(SRWLOCK& l) : lock(&l) { ::AcquireSRWLockExclusive(lock); }
    ~SrwLockGuard() { ::ReleaseSRWLockExclusive(lock); }
    SrwLockGuard(const SrwLockGuard&) = delete;
    SrwLockGuard& operator=(const SrwLockGuard&) = delete;
};

// Fixed 56-byte (0x38-byte) result packet matching callers that zero 0x50 bytes on stack and read
// opcode (+0x00), context index (+0x04), status (+0x08), and decoded payload size (+0x34).
struct ApuResult {
    uint32_t opcode = 0;
    uint32_t context_index = 0;
    uint32_t status = 0;
    uint32_t reserved0[10]{};
    uint32_t output_bytes = 0;
};
static_assert(sizeof(ApuResult) == 0x38, "ApuResult must be 0x38 bytes");
static_assert(offsetof(ApuResult, context_index) == 0x04, "context_index offset mismatch");
static_assert(offsetof(ApuResult, output_bytes) == 0x34, "output_bytes offset mismatch");

struct ApuSession {
    SRWLOCK lock = SRWLOCK_INIT;
    std::deque<ApuResult> results;
    uint64_t commands_submitted = 0;
};

struct DspSession {
    SRWLOCK lock = SRWLOCK_INIT;
    uint32_t max_streams = 0;
    bool active = false;
    uint64_t commands_submitted = 0;
};

SRWLOCK g_registry_lock = SRWLOCK_INIT;
std::vector<ApuSession*> g_apu_sessions;
std::vector<DspSession*> g_dsp_sessions;

bool contains_apu(const ApuSession* target) {
    return target && std::find(g_apu_sessions.begin(), g_apu_sessions.end(), target) != g_apu_sessions.end();
}

bool contains_dsp(const DspSession* target) {
    return target && std::find(g_dsp_sessions.begin(), g_dsp_sessions.end(), target) != g_dsp_sessions.end();
}

// ---- xapu.dll ------------------------------------------------------------------------------------------------

HRESULT WINAPI xapu_connect(void** out_handle, void*, void*) {
    if (!out_handle) return E_POINTER;
    *out_handle = nullptr;

    ApuSession* session = new (std::nothrow) ApuSession{};
    if (!session) return E_OUTOFMEMORY;

    {
        SrwLockGuard guard(g_registry_lock);
        g_apu_sessions.push_back(session);
    }
    *out_handle = session;
    return S_OK;
}

HRESULT WINAPI xapu_disconnect(void* handle, uint64_t) {
    ApuSession* session = static_cast<ApuSession*>(handle);
    if (!session) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);

    {
        SrwLockGuard guard(g_registry_lock);
        auto it = std::find(g_apu_sessions.begin(), g_apu_sessions.end(), session);
        if (it == g_apu_sessions.end()) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        g_apu_sessions.erase(it);
    }
    delete session;
    return S_OK;
}

HRESULT WINAPI xapu_enqueue_command(void* handle, const void* cmd, uint32_t) {
    if (!cmd) return E_INVALIDARG;
    const auto* bytes = static_cast<const uint8_t*>(cmd);

    ApuResult res{};
    std::memcpy(&res.opcode, bytes + 0x00, sizeof(res.opcode));
    std::memcpy(&res.context_index, bytes + 0x04, sizeof(res.context_index));
    if (res.opcode == 3) {
        std::memcpy(&res.output_bytes, bytes + 0x34, sizeof(res.output_bytes));
    }

    SrwLockGuard reg_guard(g_registry_lock);
    auto* session = static_cast<ApuSession*>(handle);
    if (!contains_apu(session)) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);

    SrwLockGuard session_guard(session->lock);
    ++session->commands_submitted;
    session->results.push_back(res);
    return S_OK;
}

uint32_t WINAPI xapu_get_result_count(void* handle, uint32_t) {
    SrwLockGuard reg_guard(g_registry_lock);
    auto* session = static_cast<ApuSession*>(handle);
    if (!contains_apu(session)) return 0;

    SrwLockGuard session_guard(session->lock);
    return static_cast<uint32_t>(session->results.size());
}

HRESULT WINAPI xapu_dequeue_result(void* handle, void* out_result) {
    if (!out_result) return E_POINTER;

    SrwLockGuard reg_guard(g_registry_lock);
    auto* session = static_cast<ApuSession*>(handle);
    if (!contains_apu(session)) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);

    SrwLockGuard session_guard(session->lock);
    if (session->results.empty()) return HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS);

    const ApuResult res = session->results.front();
    session->results.pop_front();
    std::memcpy(out_result, &res, sizeof(res));
    return S_OK;
}

// ---- chad.dll ------------------------------------------------------------------------------------------------

HRESULT WINAPI xdsp_connect_with_max_stream_limit(void*, uint32_t, uint32_t, uint32_t max_streams, uint32_t,
                                                  void** out_handle) {
    if (!out_handle) return E_POINTER;
    *out_handle = nullptr;
    if (max_streams == 0) return E_INVALIDARG;

    DspSession* session = new (std::nothrow) DspSession{};
    if (!session) return E_OUTOFMEMORY;
    session->max_streams = max_streams;

    {
        SrwLockGuard guard(g_registry_lock);
        g_dsp_sessions.push_back(session);
    }
    *out_handle = session;
    return S_OK;
}

HRESULT WINAPI xdsp_activate(void* handle, uint32_t, uint32_t, void*, void*) {
    SrwLockGuard reg_guard(g_registry_lock);
    auto* session = static_cast<DspSession*>(handle);
    if (!contains_dsp(session)) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);

    SrwLockGuard session_guard(session->lock);
    session->active = true;
    return S_OK;
}

HRESULT WINAPI xdsp_deactivate(void* handle, uint32_t, uint32_t, void*) {
    SrwLockGuard reg_guard(g_registry_lock);
    auto* session = static_cast<DspSession*>(handle);
    if (!contains_dsp(session)) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);

    SrwLockGuard session_guard(session->lock);
    session->active = false;
    return S_OK;
}

HRESULT WINAPI xdsp_submit_command(void* handle, const void* cmd) {
    if (!cmd) return E_INVALIDARG;

    SrwLockGuard reg_guard(g_registry_lock);
    auto* session = static_cast<DspSession*>(handle);
    if (!contains_dsp(session)) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);

    SrwLockGuard session_guard(session->lock);
    ++session->commands_submitted;
    return S_OK;
}

HRESULT WINAPI xdsp_disconnect(void* handle, uint64_t) {
    DspSession* session = static_cast<DspSession*>(handle);
    if (!session) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);

    {
        SrwLockGuard guard(g_registry_lock);
        auto it = std::find(g_dsp_sessions.begin(), g_dsp_sessions.end(), session);
        if (it == g_dsp_sessions.end()) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        g_dsp_sessions.erase(it);
    }
    delete session;
    return S_OK;
}

}  // namespace
}  // namespace rt::xapu_chad

SHIM_TABLE(xapu_chad,
           {"xapu.dll", "XApuConnect", reinterpret_cast<void*>(&rt::xapu_chad::xapu_connect)},
           {"xapu.dll", "XApuDisconnect", reinterpret_cast<void*>(&rt::xapu_chad::xapu_disconnect)},
           {"xapu.dll", "XApuEnqueueCommand", reinterpret_cast<void*>(&rt::xapu_chad::xapu_enqueue_command)},
           {"xapu.dll", "XApuGetResultCount", reinterpret_cast<void*>(&rt::xapu_chad::xapu_get_result_count)},
           {"xapu.dll", "XApuDequeueResult", reinterpret_cast<void*>(&rt::xapu_chad::xapu_dequeue_result)},
           {"chad.dll", "XDspConnectWithMaximumStreamLimit",
            reinterpret_cast<void*>(&rt::xapu_chad::xdsp_connect_with_max_stream_limit)},
           {"chad.dll", "XDspActivate", reinterpret_cast<void*>(&rt::xapu_chad::xdsp_activate)},
           {"chad.dll", "XDspDeactivate", reinterpret_cast<void*>(&rt::xapu_chad::xdsp_deactivate)},
           {"chad.dll", "XDspDisconnect", reinterpret_cast<void*>(&rt::xapu_chad::xdsp_disconnect)},
           {"chad.dll", "XDspSubmitCommand", reinterpret_cast<void*>(&rt::xapu_chad::xdsp_submit_command)});
