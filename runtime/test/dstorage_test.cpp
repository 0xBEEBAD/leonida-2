// Synthetic unit tests for the console DirectStorage shims (XCRDAPI.dll / dstorage_xs.dll / dstorage.dll).
#include <windows.h>
#include <unknwn.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "file_system.h"
#include "shims.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

constexpr GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidFactoryX = {0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr GUID kIidFileX = {0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};
constexpr GUID kIidStatusArrayX = {0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
constexpr GUID kIidQueueX = {0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};
constexpr GUID kIidBogus = {0xdeadbeef, 0x0000, 0x0000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};

struct QueueDescX {
    uint64_t SourceType;
    uint16_t Capacity;
    uint8_t Priority;
    uint8_t Reserved[5];
    const char* Name;
    void* Device;
};

struct RequestX {
    uint64_t Options;
    void* DestinationPtr;
    uint64_t DestinationExtra;
    uint32_t DestinationSize;
    uint32_t Reserved0;
    const void* SourcePtr;
    uint64_t SourceOffset;
    uint32_t SourceSize;
    uint32_t Reserved1;
    const char* Name;
};

struct FactoryVtbl;
struct FactoryIface {
    const FactoryVtbl* vtbl;
};
struct FactoryVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(FactoryIface*, const GUID*, void**);
    ULONG(STDMETHODCALLTYPE* AddRef)(FactoryIface*);
    ULONG(STDMETHODCALLTYPE* Release)(FactoryIface*);
    HRESULT(STDMETHODCALLTYPE* CreateQueueSlot3)(FactoryIface*, const QueueDescX*, const GUID*, void**);
    HRESULT(STDMETHODCALLTYPE* OpenFile)(FactoryIface*, const wchar_t*, const GUID*, void**);
    HRESULT(STDMETHODCALLTYPE* CreateStatusArray)(FactoryIface*, UINT32, const char*, const GUID*, void**);
    void(STDMETHODCALLTYPE* SetDebugFlags)(FactoryIface*, UINT32);
    HRESULT(STDMETHODCALLTYPE* SetCpuAffinity)(FactoryIface*, UINT64);
    HRESULT(STDMETHODCALLTYPE* SetStagingBufferSize)(FactoryIface*, UINT32);
    HRESULT(STDMETHODCALLTYPE* SetDisableBypassIO)(FactoryIface*, BOOL);
    HRESULT(STDMETHODCALLTYPE* CreateQueue)(FactoryIface*, const QueueDescX*, const GUID*, void**);
};

struct FileVtbl;
struct FileIface {
    const FileVtbl* vtbl;
};
struct FileVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(FileIface*, const GUID*, void**);
    ULONG(STDMETHODCALLTYPE* AddRef)(FileIface*);
    ULONG(STDMETHODCALLTYPE* Release)(FileIface*);
    void(STDMETHODCALLTYPE* Close)(FileIface*);
    HRESULT(STDMETHODCALLTYPE* GetFileInformation)(FileIface*, BY_HANDLE_FILE_INFORMATION*);
};

struct StatusArrayVtbl;
struct StatusArrayIface {
    const StatusArrayVtbl* vtbl;
};
struct StatusArrayVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(StatusArrayIface*, const GUID*, void**);
    ULONG(STDMETHODCALLTYPE* AddRef)(StatusArrayIface*);
    ULONG(STDMETHODCALLTYPE* Release)(StatusArrayIface*);
    bool(STDMETHODCALLTYPE* IsComplete)(StatusArrayIface*, UINT32);
    HRESULT(STDMETHODCALLTYPE* GetHResult)(StatusArrayIface*, UINT32);
};

struct QueueVtbl;
struct QueueIface {
    const QueueVtbl* vtbl;
};
struct QueueVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(QueueIface*, const GUID*, void**);
    ULONG(STDMETHODCALLTYPE* AddRef)(QueueIface*);
    ULONG(STDMETHODCALLTYPE* Release)(QueueIface*);
    void(STDMETHODCALLTYPE* EnqueueRequest)(QueueIface*, const RequestX*);
    void(STDMETHODCALLTYPE* EnqueueStatus)(QueueIface*, StatusArrayIface*, UINT32);
    void(STDMETHODCALLTYPE* EnqueueSignal)(QueueIface*, void*, UINT64);
    void(STDMETHODCALLTYPE* Submit)(QueueIface*);
    void(STDMETHODCALLTYPE* CancelRequestsWithTag)(QueueIface*, UINT64, UINT64);
    void(STDMETHODCALLTYPE* Close)(QueueIface*);
    HANDLE(STDMETHODCALLTYPE* GetErrorEvent)(QueueIface*);
    void(STDMETHODCALLTYPE* RetrieveErrorRecord)(QueueIface*, void*, UINT32);
    void(STDMETHODCALLTYPE* Query)(QueueIface*, void*);
};

using DStorageGetFactoryFn = HRESULT(WINAPI*)(const GUID*, void**);

uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t s1 = 1;
    uint32_t s2 = 0;
    for (size_t i = 0; i < len; ++i) {
        s1 = (s1 + data[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    return (s2 << 16) | s1;
}

std::vector<uint8_t> zlib_stored_wrap(const uint8_t* payload, size_t len) {
    std::vector<uint8_t> out;
    out.push_back(0x78u);
    out.push_back(0x01u);
    out.push_back(0x01u);  // BFINAL=1, BTYPE=00 (stored)
    const uint16_t l = static_cast<uint16_t>(len);
    const uint16_t nl = static_cast<uint16_t>(~l);
    out.push_back(static_cast<uint8_t>(l & 0xFFu));
    out.push_back(static_cast<uint8_t>(l >> 8));
    out.push_back(static_cast<uint8_t>(nl & 0xFFu));
    out.push_back(static_cast<uint8_t>(nl >> 8));
    out.insert(out.end(), payload, payload + len);
    const uint32_t a = adler32(payload, len);
    out.push_back(static_cast<uint8_t>((a >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((a >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((a >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(a & 0xFFu));
    return out;
}

}  // namespace

int main() {
    auto* p_xcrdapi = reinterpret_cast<DStorageGetFactoryFn>(rt::shims::find("xcrdapi.dll", "DStorageGetFactory"));
    auto* p_dstorage_xs =
        reinterpret_cast<DStorageGetFactoryFn>(rt::shims::find("dstorage_xs.dll", "DStorageGetFactory"));
    auto* p_dstorage = reinterpret_cast<DStorageGetFactoryFn>(rt::shims::find("dstorage.dll", "DStorageGetFactory"));
    check(p_xcrdapi != nullptr, "xcrdapi.dll!DStorageGetFactory resolves");
    check(p_dstorage_xs == p_xcrdapi, "dstorage_xs.dll shares entrypoint");
    check(p_dstorage == p_xcrdapi, "dstorage.dll shares entrypoint");
    check(rt::shims::owns_dll("xcrdapi.dll"), "owns_dll(xcrdapi.dll)");
    check(rt::shims::owns_dll("dstorage_xs.dll"), "owns_dll(dstorage_xs.dll)");
    if (g_failures) return 1;

    // Invalid argument / interface checks.
    void* out_ptr = reinterpret_cast<void*>(1);
    check(p_xcrdapi(&kIidFactoryX, nullptr) == E_POINTER, "null ppv -> E_POINTER");
    check(p_xcrdapi(nullptr, &out_ptr) == E_INVALIDARG && out_ptr == nullptr, "null riid -> E_INVALIDARG & zeroes ppv");
    check(p_xcrdapi(&kIidBogus, &out_ptr) == E_NOINTERFACE && out_ptr == nullptr, "unknown iid -> E_NOINTERFACE");

    FactoryIface* factory = nullptr;
    check(SUCCEEDED(p_xcrdapi(&kIidFactoryX, reinterpret_cast<void**>(&factory))) && factory != nullptr,
          "DStorageGetFactory(IID_IDStorageFactoryX)");
    IUnknown* unk = nullptr;
    check(SUCCEEDED(factory->vtbl->QueryInterface(factory, &kIidUnknown, reinterpret_cast<void**>(&unk))) &&
              unk != nullptr,
          "factory QueryInterface IUnknown");
    unk->Release();

    check(factory->vtbl->SetStagingBufferSize(factory, 0) == E_INVALIDARG, "SetStagingBufferSize(0) -> E_INVALIDARG");
    check(factory->vtbl->SetStagingBufferSize(factory, 0x2000000u) == S_OK, "SetStagingBufferSize(32 MiB) -> S_OK");

    // Prepare temporary game/overlay folders to test GAME: resolution, raw read, zlib read, memory source, and errors.
    const auto temp_base =
        std::filesystem::temp_directory_path() / ("leonida_dstorage_" + std::to_string(::GetCurrentProcessId()));
    const auto game_dir = temp_base / "game";
    const auto overlay_dir = temp_base / "overlay";
    std::error_code ec;
    std::filesystem::remove_all(temp_base, ec);
    std::filesystem::create_directories(game_dir / "data", ec);
    std::filesystem::create_directories(overlay_dir, ec);
    rt::fs::configure(game_dir, overlay_dir);

    std::vector<uint8_t> raw_payload(128);
    for (size_t i = 0; i < raw_payload.size(); ++i) raw_payload[i] = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);

    const char kMsg[] = "Leonida DirectStorage zlib test payload!";
    const auto z_stream = zlib_stored_wrap(reinterpret_cast<const uint8_t*>(kMsg), sizeof(kMsg));

    {
        std::ofstream f(game_dir / "data" / "pack.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw_payload.data()), static_cast<std::streamsize>(raw_payload.size()));
        f.write(reinterpret_cast<const char*>(z_stream.data()), static_cast<std::streamsize>(z_stream.size()));
    }

    FileIface* bad_file = nullptr;
    check(factory->vtbl->OpenFile(factory, L"GAME:\\..\\secret.bin", &kIidFileX, reinterpret_cast<void**>(&bad_file)) ==
              HRESULT_FROM_WIN32(ERROR_INVALID_NAME),
          "OpenFile rejects .. escape out of GAME:");
    check(factory->vtbl->OpenFile(factory, L"GAME:\\data\\missing.bin", &kIidFileX,
                                  reinterpret_cast<void**>(&bad_file)) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
          "OpenFile missing file -> ERROR_FILE_NOT_FOUND");

    FileIface* file = nullptr;
    check(SUCCEEDED(factory->vtbl->OpenFile(factory, L"GAME:\\data\\pack.bin", &kIidFileX,
                                            reinterpret_cast<void**>(&file))) &&
              file != nullptr,
          "OpenFile(GAME:\\data\\pack.bin)");

    BY_HANDLE_FILE_INFORMATION finfo{};
    check(SUCCEEDED(file->vtbl->GetFileInformation(file, &finfo)) &&
              finfo.nFileSizeLow == raw_payload.size() + z_stream.size(),
          "GetFileInformation reports exact file size");

    StatusArrayIface* status = nullptr;
    check(SUCCEEDED(factory->vtbl->CreateStatusArray(factory, 2, "test", &kIidStatusArrayX,
                                                     reinterpret_cast<void**>(&status))) &&
              status != nullptr,
          "CreateStatusArray(2)");

    QueueDescX qdesc{};
    qdesc.SourceType = 0;
    qdesc.Capacity = 64;
    qdesc.Priority = 1;
    qdesc.Name = "test_queue";
    qdesc.Device = reinterpret_cast<void*>(1);
    QueueIface* queue = nullptr;
    check(SUCCEEDED(factory->vtbl->CreateQueue(factory, &qdesc, &kIidQueueX, reinterpret_cast<void**>(&queue))) &&
              queue != nullptr,
          "CreateQueue");

    // Enqueue: (1) file sub-range read at offset 16, size 48; (2) zlib inflate from file offset 128; (3) memory read.
    uint8_t dst_raw[48]{};
    char dst_zlib[sizeof(kMsg)]{};
    uint8_t src_mem[16]{10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160};
    uint8_t dst_mem[8]{};

    RequestX req_raw{};
    req_raw.Options = 0;
    req_raw.DestinationPtr = dst_raw;
    req_raw.DestinationSize = sizeof(dst_raw);
    req_raw.SourcePtr = file;
    req_raw.SourceOffset = 16;
    req_raw.SourceSize = sizeof(dst_raw);

    RequestX req_zlib{};
    req_zlib.Options = 0x200001ull;  // Zlib flag bit 0
    req_zlib.DestinationPtr = dst_zlib;
    req_zlib.DestinationSize = sizeof(kMsg);
    req_zlib.SourcePtr = file;
    req_zlib.SourceOffset = raw_payload.size();
    req_zlib.SourceSize = static_cast<uint32_t>(z_stream.size());

    RequestX req_mem{};
    req_mem.Options = 1ull << 22;  // Memory source (bit 22)
    req_mem.DestinationPtr = dst_mem;
    req_mem.DestinationSize = sizeof(dst_mem);
    req_mem.SourcePtr = src_mem;
    req_mem.SourceOffset = 4;
    req_mem.SourceSize = sizeof(dst_mem);

    queue->vtbl->EnqueueRequest(queue, &req_raw);
    queue->vtbl->EnqueueRequest(queue, &req_zlib);
    queue->vtbl->EnqueueRequest(queue, &req_mem);
    queue->vtbl->EnqueueStatus(queue, status, 0);

    check(!status->vtbl->IsComplete(status, 0), "StatusArray[0] pending before Submit");
    check(status->vtbl->GetHResult(status, 0) == E_PENDING, "GetHResult(0) is E_PENDING before Submit");

    queue->vtbl->Submit(queue);
    check(status->vtbl->IsComplete(status, 0), "StatusArray[0] complete after Submit");
    check(status->vtbl->GetHResult(status, 0) == S_OK, "GetHResult(0) is S_OK");
    check(std::memcmp(dst_raw, raw_payload.data() + 16, sizeof(dst_raw)) == 0, "file read matches payload slice");
    check(std::memcmp(dst_zlib, kMsg, sizeof(kMsg)) == 0, "file zlib request inflates cleanly");
    check(std::memcmp(dst_mem, src_mem + 4, sizeof(dst_mem)) == 0, "memory source request copies slice");

    // Out-of-bounds read fails loudly in StatusArray slot 1.
    RequestX req_oob{};
    req_oob.DestinationPtr = dst_raw;
    req_oob.DestinationSize = sizeof(dst_raw);
    req_oob.SourcePtr = file;
    req_oob.SourceOffset = raw_payload.size() + z_stream.size();
    req_oob.SourceSize = 4;
    queue->vtbl->EnqueueRequest(queue, &req_oob);
    queue->vtbl->EnqueueStatus(queue, status, 1);
    queue->vtbl->Submit(queue);
    check(status->vtbl->IsComplete(status, 1), "StatusArray[1] complete after EOF request");
    check(status->vtbl->GetHResult(status, 1) == HRESULT_FROM_WIN32(ERROR_HANDLE_EOF),
          "StatusArray[1] records ERROR_HANDLE_EOF");

    // Unsupported compression codec fails with E_NOTIMPL.
    RequestX req_unsupported{};
    req_unsupported.Options = (1ull << 22) | 0x20ull;
    req_unsupported.DestinationPtr = dst_mem;
    req_unsupported.DestinationSize = sizeof(dst_mem);
    req_unsupported.SourcePtr = src_mem;
    req_unsupported.SourceSize = sizeof(dst_mem);
    queue->vtbl->EnqueueRequest(queue, &req_unsupported);
    queue->vtbl->EnqueueStatus(queue, status, 1);
    queue->vtbl->Submit(queue);
    check(status->vtbl->GetHResult(status, 1) == E_NOTIMPL, "unsupported codec returns E_NOTIMPL");

    queue->vtbl->Close(queue);
    queue->vtbl->Release(queue);
    status->vtbl->Release(status);
    file->vtbl->Release(file);
    factory->vtbl->Release(factory);

    std::filesystem::remove_all(temp_base, ec);

    if (g_failures == 0) std::printf("dstorage_test: OK\n");
    return g_failures == 0 ? 0 : 1;
}
