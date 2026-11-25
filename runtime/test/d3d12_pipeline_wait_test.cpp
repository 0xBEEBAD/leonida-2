// Bounded waits for pipelines still being built (src/pipeline_wait.h, docs/d3d12_vulkan.md "Pipelines still being
// built"). A build hook (set_pipeline_build_hook) holds one compute PSO's build in flight - claimed by the thread that
// builds it, before the driver call - so "still being built" is a state the test holds, not a race. Run as
//   async  LEONIDA_ASYNC_PIPELINES=1: the build is a compile-worker job, queued when SetPipelineState is recorded;
//   sync   asynchronous builds off: the build is the PSO's warm-up on the compile worker.
// Each case's list copies a sentinel into the output buffer, dispatches, and copies the output to a readback buffer:
// a skipped dispatch leaves the sentinel, so the rest of the list (barriers and copies) is seen to have run.
//   skip     budget 50 ms: ExecuteCommandLists returns within the budget, the dispatch is skipped and counted.
//   later    the build let go: the next submission runs the dispatch with the pipeline.
//   in time  budget 2 s, the build let go 200 ms into the wait: the submission waited, and the dispatch ran.
//   no lock  budget -1 (wait for the build): while a submission waits, another thread creates and maps a buffer at
//            once (the wait holds no lock a resource creation needs); let go, the dispatch runs.
//   zero     budget 0: no wait, skipped at once.
//   exit     (async) budget -1 with the PSO's job still queued when the compiler stops for exit: the waiting
//            submission builds the pipeline in place and runs instead of waiting for ever.
// No game data: the shader is test/glsl/cs_user_data.comp, the programs are made-up FSSM bytes.
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_compile_exit.h"
#include "../shims/d3d12_pipeline_compile.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/pipeline_wait.h"
#include "../src/shader_library.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifndef LEONIDA_TEST_CS_SPV
#error "LEONIDA_TEST_CS_SPV must name the compiled test shader"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

using XboxComputePipelineFn = HRESULT(WINAPI*)(void* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, std::uint64_t,
                                               std::uint64_t, const IID*, void**);
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slots 9/10: handle in RAX
using SerializeFn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
using Clock = std::chrono::steady_clock;

constexpr unsigned kPrograms = 5;
constexpr std::uint32_t kSentinel = 0x5e5e5e5eu;

void* slot(void* object, unsigned index) { return (*reinterpret_cast<void***>(object))[index]; }

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
}

std::vector<std::uint8_t> zlib_stored(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out = {0x78, 0x01, 0x01};
    const auto length = std::uint16_t(data.size());
    out.insert(out.end(), {std::uint8_t(length), std::uint8_t(length >> 8), std::uint8_t(~length),
                           std::uint8_t(~length >> 8)});
    out.insert(out.end(), data.begin(), data.end());
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t x : data) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    const std::uint32_t adler = (b << 16) | a;
    out.insert(out.end(), {std::uint8_t(adler >> 24), std::uint8_t(adler >> 16), std::uint8_t(adler >> 8),
                           std::uint8_t(adler)});
    return out;
}

// DXBC { DXIL { stub, XDAT { 0x0D: XBCH + zlib(fssm) } } }, as in d3d12_compute_test.
std::vector<std::uint8_t> shader_container(const std::vector<std::uint8_t>& fssm) {
    const auto stream = zlib_stored(fssm);
    std::vector<std::uint8_t> xbch = {'X', 'B', 'C', 'H'};
    for (const std::uint32_t v : {1u, std::uint32_t(stream.size()), 0u, std::uint32_t(fssm.size()), 0u}) put32(xbch, v);
    xbch.insert(xbch.end(), stream.begin(), stream.end());
    std::vector<std::uint8_t> dxil(16, 0);
    dxil.insert(dxil.end(), {'X', 'D', 'A', 'T'});
    put32(dxil, 1);
    put32(dxil, 12);
    put32(dxil, 0x0d);
    put32(dxil, 16 + 12 + 12);
    put32(dxil, std::uint32_t(xbch.size()));
    dxil.insert(dxil.end(), xbch.begin(), xbch.end());
    std::vector<std::uint8_t> out = {'D', 'X', 'B', 'C'};
    out.resize(28, 0);
    put32(out, 1);
    put32(out, 36);
    out.insert(out.end(), {'D', 'X', 'I', 'L'});
    put32(out, std::uint32_t(dxil.size()));
    out.insert(out.end(), dxil.begin(), dxil.end());
    return out;
}

// Program i: a made-up FSSM blob whose code word (dword 1, which the shader reads through its code base) differs.
std::uint32_t code_word(unsigned i) { return 9100 + i; }
std::vector<std::uint8_t> program(unsigned i) {
    std::vector<std::uint8_t> fssm = {'F', 'S', 'S', 'M'};
    put32(fssm, code_word(i));
    for (std::uint32_t w = 0; w < 14; ++w) put32(fssm, 0xbf810000u + w);
    return fssm;
}

std::vector<std::uint8_t> serialize(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc) {
    const HMODULE d3d12 = LoadLibraryExA("d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto fn = d3d12 ? reinterpret_cast<SerializeFn>(
                                reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature")))
                          : nullptr;
    std::vector<std::uint8_t> out;
    ID3DBlob* blob = nullptr;
    if (fn && SUCCEEDED(fn(&desc, &blob, nullptr)) && blob) {
        const auto* p = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
        out.assign(p, p + blob->GetBufferSize());
        blob->Release();
    }
    return out;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

struct Event {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() = default;
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    ~Event() { CloseHandle(handle); }
    void set() const { SetEvent(handle); }
    void reset() const { ResetEvent(handle); }
    bool wait(DWORD ms = 30000) const { return WaitForSingleObject(handle, ms) == WAIT_OBJECT_0; }
    bool is_set() const { return WaitForSingleObject(handle, 0) == WAIT_OBJECT_0; }
};

using rt::d3d12::PipelineCompiler;

// Waits until the worker has run everything queued so far: a last-priority marker job.
bool drain(PipelineCompiler& compiler) {
    Event done;
    static const int marker = 0;
    if (!compiler.submit(&marker, nullptr, SIZE_MAX, [&done] { done.set(); })) return false;
    return done.wait(120000);
}

// Holds the worker in a job of no device until open() (or the end of the scope).
struct Gate {
    Event entered, opened, left;
    explicit Gate(PipelineCompiler& compiler) {
        static const int gate = 0;
        compiler.submit(&gate, nullptr, 0, [this] {
            entered.set();
            opened.wait(INFINITE);
            left.set();
        });
        entered.wait();
    }
    void open() { opened.set(); }
    ~Gate() {
        opened.set();
        left.wait();
    }
};

// Holds the build of `target` (once claimed, before the driver call) until let_go().
struct Hold {
    std::atomic<ID3D12PipelineState*> target{nullptr};
    Event entered, proceed;
    Hold() {
        rt::d3d12::set_pipeline_build_hook([this](ID3D12PipelineState* pso) {
            if (pso != target.load()) return;
            entered.set();
            proceed.wait(INFINITE);
        });
    }
    void let_go() { proceed.set(); }
    ~Hold() {
        proceed.set();
        rt::d3d12::set_pipeline_build_hook(nullptr);
    }
};

struct Outcome {
    std::vector<std::uint32_t> words;
    long long execute_ms = 0;
    leonida::ReplayResult result = leonida::ReplayResult::unsupported;
};

std::uint32_t expected(unsigned program_index, std::uint32_t i) {
    static constexpr std::uint32_t table_values[4] = {1000, 2000, 3000, 4000};
    return 7 + 3 * i + table_values[i % 4] + code_word(program_index);
}

struct Session {
    ID3D12Device* device = nullptr;
    ID3D12RootSignature* root = nullptr;
    std::vector<ID3D12PipelineState*> psos;

    int open(vktest::Device& d, const std::filesystem::path& library) {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.subgroup = d.subgroup;
        info.shader_dir = library.string();
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        const HRESULT proxied = rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                                  reinterpret_cast<void**>(&device), false);
        if (FAILED(proxied)) inner->Release();
        EXPECT(SUCCEEDED(proxied));
        rt::d3d12::set_device_outer(inner, device);

        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1, 0, 0};
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 2};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor = {0, 0};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &range};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        const auto blob = serialize(root_desc);
        EXPECT(!blob.empty());
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&root))));
        return 0;
    }

    int add(unsigned program_index, ID3D12PipelineState*& pso) {
        const auto container = shader_container(program(program_index));
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root;
        desc.CS = {container.data(), container.size()};
        const IID iid = __uuidof(ID3D12PipelineState);
        pso = nullptr;
        EXPECT(SUCCEEDED(reinterpret_cast<XboxComputePipelineFn>(slot(device, 62))(
            device, &desc, 0, 0, &iid, reinterpret_cast<void**>(&pso))));
        psos.push_back(pso);
        return 0;
    }

    // A list: sentinel copy into the output, the dispatch, the output copied to readback. `before_execute` runs just
    // before ExecuteCommandLists (after the list is recorded); only that call is timed.
    int dispatch(ID3D12PipelineState* pso, Outcome& outcome, const std::function<void()>& before_execute = {}) {
        constexpr UINT elements = 64, bytes = elements * 4;
        static constexpr std::uint32_t table_values[4] = {1000, 2000, 3000, 4000};
        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        const auto uav = buffer_desc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto upload_desc = buffer_desc(512, D3D12_RESOURCE_FLAG_NONE);
        const auto readback_desc = buffer_desc(bytes, D3D12_RESOURCE_FLAG_NONE);
        ID3D12Resource *out = nullptr, *upload = nullptr, *readback = nullptr;
        ID3D12DescriptorHeap* heap = nullptr;
        ID3D12CommandQueue* queue = nullptr;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        ID3D12Fence* fence = nullptr;
        struct Releaser {
            std::vector<IUnknown**> all;
            ~Releaser() {
                for (IUnknown** p : all)
                    if (*p) (*p)->Release();
            }
        } releaser{{reinterpret_cast<IUnknown**>(&out), reinterpret_cast<IUnknown**>(&upload),
                    reinterpret_cast<IUnknown**>(&readback), reinterpret_cast<IUnknown**>(&heap),
                    reinterpret_cast<IUnknown**>(&queue), reinterpret_cast<IUnknown**>(&allocator),
                    reinterpret_cast<IUnknown**>(&list), reinterpret_cast<IUnknown**>(&fence)}};
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out))));
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&upload))));
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback))));
        // Bytes 0..15: the CBV's table values; 256..511: the sentinel the output starts as.
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(upload->Map(0, nullptr, &mapped)));
        std::memcpy(mapped, table_values, sizeof(table_values));
        for (UINT i = 0; i < elements; ++i) std::memcpy(static_cast<std::uint8_t*>(mapped) + 256 + 4 * i, &kSentinel, 4);
        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))));
        const std::uint64_t cpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap, 9))(heap);
        const std::uint64_t gpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap, 10))(heap);
        EXPECT(cpu && gpu >= 0x10000 && gpu < (1ull << 32));
        const std::uint64_t target = upload->GetGPUVirtualAddress();
        std::memcpy(reinterpret_cast<void*>(cpu), &target, sizeof(target));

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))));
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso,
                                                   IID_PPV_ARGS(&list))));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
        list->CopyBufferRegion(out, 0, upload, 256, bytes);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {out, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        list->ResourceBarrier(1, &barrier);
        list->SetDescriptorHeaps(1, &heap);
        list->SetComputeRootSignature(root);
        list->SetComputeRoot32BitConstant(0, 7, 0);
        list->SetComputeRoot32BitConstant(0, 3, 1);
        list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
        list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu});
        list->Dispatch(1, 1, 1);
        barrier.Transition = {out, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(readback, 0, out, 0, bytes);
        EXPECT(list->Close() == S_OK);
        if (before_execute) before_execute();
        ID3D12CommandList* lists[] = {list};
        const auto start = Clock::now();
        queue->ExecuteCommandLists(1, lists);
        outcome.execute_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        outcome.result = rt::d3d12::last_execute_result(queue);
        EXPECT(outcome.result == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence, 1)));
        EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, nullptr)));
        void* results = nullptr;
        EXPECT(SUCCEEDED(readback->Map(0, nullptr, &results)));
        outcome.words.assign(elements, 0);
        std::memcpy(outcome.words.data(), results, bytes);
        return 0;
    }

    ~Session() {
        if (device) device->Release();
        if (root) root->Release();
        for (ID3D12PipelineState* pso : psos) pso->Release();
    }
};

bool ran(const Outcome& o, unsigned program_index) {
    for (std::uint32_t i = 0; i < o.words.size(); ++i)
        if (o.words[i] != expected(program_index, i)) return false;
    return !o.words.empty();
}
bool skipped(const Outcome& o) {
    for (const std::uint32_t w : o.words)
        if (w != kSentinel) return false;
    return !o.words.empty();
}

std::uint64_t skips() { return leonida::pipeline_wait::stats().skipped_dispatches.load(); }

}  // namespace

int main(int argc, char** argv) {
    const bool async = argc > 1 && !std::strcmp(argv[1], "async");
    if (argc < 2 || (!async && std::strcmp(argv[1], "sync"))) {
        std::fprintf(stderr, "usage: d3d12_pipeline_wait_test async|sync\n");
        return 2;
    }
    _putenv_s("LEONIDA_LAZY_COMPUTE", "1");
    _putenv_s("LEONIDA_COMPUTE_WARMUP", async ? "0" : "1");
    _putenv_s("LEONIDA_ASYNC_PIPELINES", async ? "1" : "0");
    _putenv_s("LEONIDA_STRICT_REPLAY", "0");
    const testfiles::ScratchDir scratch(async ? "leonida_pipeline_wait_async" : "leonida_pipeline_wait_sync");
    const auto library = scratch.path() / "spirv";
    std::filesystem::create_directories(library);
    _putenv_s("LEONIDA_PIPELINE_CACHE_DIR", (scratch.path() / "cache").string().c_str());

    vktest::Device d;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) {
        std::fprintf(stderr, "SKIP: no usable Vulkan 1.3 device\n");
        return 77;
    }
    EXPECT(device_status == vktest::DeviceStatus::ready);
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{";
        for (unsigned i = 0; i < kPrograms; ++i) {
            const std::string name = "cs_wait_" + std::to_string(i);
            EXPECT(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_TEST_CS_SPV), library / (name + ".spv")));
            index << (i ? ",\n" : "") << "\"" << name << "\": {\"stage\": \"cs\", \"fssm_sha256\": \""
                  << leonida::sha256_hex(program(i)) << "\", \"bindings\": [], \"push_constant_bytes\": 144, "
                  << "\"local_size\": [64, 1, 1], \"uses_code_base\": true, \"wave64\": false}";
        }
        index << "}";
    }

    auto& compiler = PipelineCompiler::instance();
    auto& stats = leonida::pipeline_wait::stats();
    Session s;
    if (s.open(d, library)) return 1;
    const char* mode = async ? "async" : "sync";

    // A PSO whose build `hold` holds once claimed. Sync: its warm-up, queued behind a gate so the hold knows the PSO
    // before the worker reaches it, is held at once. Async: its job is queued when the list records SetPipelineState,
    // and held then (the caller waits for hold.entered before executing).
    const auto held_pso = [&](unsigned program_index, Hold& hold, ID3D12PipelineState*& pso) -> int {
        {
            Gate gate(compiler);
            if (s.add(program_index, pso)) return 1;
            hold.target = pso;
        }
        if (!async) EXPECT(hold.entered.wait());
        return 0;
    };
    const auto entered = [](Hold& hold) { return [&hold] { hold.entered.wait(); }; };

    // --- skip: budget 50 ms ---------------------------------------------------------------------------------------
    ID3D12PipelineState* first = nullptr;
    {
        Hold hold;
        if (held_pso(0, hold, first)) return 1;
        leonida::pipeline_wait::set_budgets(50, -1);
        const std::uint64_t skips_before = skips(), waited_before = stats.waited.load();
        Outcome o;
        if (s.dispatch(first, o, entered(hold))) return 1;
        std::printf("%s skip: ExecuteCommandLists returned in %lld ms with the build held; dispatch %s\n", mode,
                    o.execute_ms, skipped(o) ? "skipped (sentinel kept, the copies ran)" : "NOT skipped");
        EXPECT(skipped(o));
        EXPECT(o.execute_ms < 50 + 450);
        EXPECT(skips() == skips_before + 1);
        EXPECT(stats.waited.load() == waited_before + 1);
        EXPECT(stats.building.load() >= 1);
        hold.let_go();
        EXPECT(drain(compiler));
    }
    // --- later: the build finished in the background; the next submission uses it ---------------------------------
    {
        const std::uint64_t skips_before = skips();
        Outcome o;
        if (s.dispatch(first, o)) return 1;
        std::printf("%s later: ran with the pipeline built in the background (%lld ms)\n", mode, o.execute_ms);
        EXPECT(ran(o, 0));
        EXPECT(skips() == skips_before);
    }
    // --- in time: budget 2 s, the build let go 200 ms into the wait ------------------------------------------------
    {
        Hold hold;
        ID3D12PipelineState* pso = nullptr;
        if (held_pso(1, hold, pso)) return 1;
        leonida::pipeline_wait::set_budgets(2000, -1);
        const std::uint64_t skips_before = skips();
        Event executing;
        Outcome o;
        int status = 1;
        std::thread title([&] {
            status = s.dispatch(pso, o, [&] {
                hold.entered.wait();
                executing.set();
            });
        });
        EXPECT(executing.wait());
        Sleep(200);
        hold.let_go();
        title.join();
        EXPECT(status == 0);
        std::printf("%s in time: waited %lld ms for a build let go 200 ms in; dispatch %s\n", mode, o.execute_ms,
                    ran(o, 1) ? "ran" : "did NOT run");
        EXPECT(ran(o, 1));
        EXPECT(o.execute_ms >= 150 && o.execute_ms < 2000);
        EXPECT(skips() == skips_before);
    }
    // --- no lock: budget -1; a resource is created and mapped while a submission waits -----------------------------
    {
        Hold hold;
        ID3D12PipelineState* pso = nullptr;
        if (held_pso(2, hold, pso)) return 1;
        leonida::pipeline_wait::set_budgets(-1, -1);
        Event executing, executed;
        Outcome o;
        int status = 1;
        std::thread title([&] {
            status = s.dispatch(pso, o, [&] {
                hold.entered.wait();
                executing.set();
            });
            executed.set();
        });
        EXPECT(executing.wait());
        Sleep(200);
        const auto start = Clock::now();
        ID3D12Resource* buffer = nullptr;
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const auto desc = buffer_desc(1 << 20, D3D12_RESOURCE_FLAG_NONE);
        const HRESULT created = s.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                  IID_PPV_ARGS(&buffer));
        void* mapped = nullptr;
        const HRESULT map = SUCCEEDED(created) ? buffer->Map(0, nullptr, &mapped) : E_FAIL;
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        if (buffer) buffer->Release();
        const bool still_waiting = !executed.is_set();
        hold.let_go();
        title.join();
        EXPECT(status == 0);
        std::printf("%s no lock: a buffer was created and mapped in %lld ms while a submission waited (still waiting: "
                    "%s); the submission then ran the dispatch after %lld ms\n",
                    mode, static_cast<long long>(took), still_waiting ? "yes" : "no", o.execute_ms);
        EXPECT(SUCCEEDED(created) && SUCCEEDED(map));
        EXPECT(took < 500);
        EXPECT(still_waiting);
        EXPECT(ran(o, 2));
    }
    // --- zero: budget 0 -------------------------------------------------------------------------------------------
    {
        Hold hold;
        ID3D12PipelineState* pso = nullptr;
        if (held_pso(3, hold, pso)) return 1;
        leonida::pipeline_wait::set_budgets(0, -1);
        const std::uint64_t skips_before = skips(), waited_before = stats.waited.load();
        Outcome o;
        if (s.dispatch(pso, o, entered(hold))) return 1;
        std::printf("%s zero: returned in %lld ms, dispatch %s\n", mode, o.execute_ms,
                    skipped(o) ? "skipped" : "NOT skipped");
        EXPECT(skipped(o));
        EXPECT(o.execute_ms < 200);
        EXPECT(skips() == skips_before + 1);
        EXPECT(stats.waited.load() == waited_before);
        hold.let_go();
        EXPECT(drain(compiler));
    }
    // A PSO never held: built (in place when synchronous) and run in its first submission.
    {
        ID3D12PipelineState* pso = nullptr;
        if (s.add(4, pso)) return 1;
        leonida::pipeline_wait::set_budgets(2000, -1);
        Outcome o;
        if (s.dispatch(pso, o)) return 1;
        std::printf("%s first use: %s (%lld ms)\n", mode, ran(o, 4) ? "ran" : "did NOT run", o.execute_ms);
        EXPECT(ran(o, 4));
    }
    // --- exit (async): budget -1, the PSO's job still queued behind a gate when the compiler stops for exit (its queue
    // dropped): the waiting submission does not wait for ever, it builds the pipeline in place and runs the dispatch.
    if (async) {
        leonida::pipeline_wait::set_budgets(-1, -1);
        Event executing, executed;
        Outcome o;
        int status = 1;
        {
            Gate gate(compiler);  // the worker is held, so the job SetPipelineState queues stays queued
            ID3D12PipelineState* queued = nullptr;
            if (s.add(1, queued)) return 1;  // program 1 again: a second PSO, with its own build
            std::thread title([&] {
                status = s.dispatch(queued, o, [&] { executing.set(); });
                executed.set();
            });
            EXPECT(executing.wait());
            Sleep(200);
            const bool waiting = !executed.is_set();
            const bool idle = rt::d3d12::stop_pipeline_compiles(100);  // the gate's job is still running: false
            const auto stopped = Clock::now();
            const bool finished = executed.wait(5000);
            const auto after = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stopped).count();
            gate.open();
            title.join();
            std::printf("async exit: a submission waiting without limit for a queued build (waiting: %s) went on %lld "
                        "ms after the exit stop (worker idle: %s) and %s\n",
                        waiting ? "yes" : "no", static_cast<long long>(after), idle ? "yes" : "no",
                        ran(o, 1) ? "ran the dispatch" : "did NOT run the dispatch");
            EXPECT(waiting && !idle && finished);
            EXPECT(status == 0);
            EXPECT(ran(o, 1));
        }
    }
    EXPECT(stats.building.load() == 0);
    std::printf("%s: all cases passed\n", mode);
    return 0;
}
