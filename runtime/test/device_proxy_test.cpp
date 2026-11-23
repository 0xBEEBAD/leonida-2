// Exercises the Xbox-ABI device proxy against a real WARP PC device, with no game executable.
//
// It creates the proxy through the registered d3d12_xs!D3D12XboxCreateDevice shim, then calls the measured Xbox
// slots exactly as the guest would (raw function pointers, not the C++ interface):
//   slot 14  CreateDescriptorHeap (returns a heap proxy)
//   heap 9/10 GetCPU/GetGPUDescriptorHandleForHeapStart (Xbox: 8-byte handle in RAX; the PC heap uses the
//             hidden return pointer in RDX, so slot 14 wraps it)
//   slot 62  CreateComputePipelineState
//   slot 76  resource creation with an Xbox layout word (0x101)
//   slot 8/36 CreateCommandQueue/CreateFence (return queue and fence proxies that run GPU work)
// Slot 9 on the device is CreateCommandAllocator on the PC vtable and must still forward, which proves the handle
// getters were not wedged into the device slots. Console output also shows the proxy's first-call slot log.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <vector>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <cstddef>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <wrl/client.h>

#include "d3d12_trace.h"
#include "d3d12_xs_device.h"
#include "shims.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!condition) ++g_failures;
}

using Microsoft::WRL::ComPtr;

using XboxCreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, const void*, const IID*, void**);
using XboxCreateResourceFn = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                              const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE*, REFIID, void**);
// Xbox slot 62: (desc, two words, iid, out), as XboxBvhBuilder.dll calls it (zeros for the two words).
using XboxCreateComputePsoFn = HRESULT(WINAPI*)(void*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, std::uint64_t,
                                                std::uint64_t, REFIID, void**);
using GetCpuHandleFn = D3D12_CPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*);
using GetGpuHandleFn = D3D12_GPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*);

D3D12_RESOURCE_DESC buffer_desc() {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 4096;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);  // measured Xbox tiled/buffer word
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

D3D12_RESOURCE_DESC texture_desc() {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 4;
    desc.Height = 4;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

D3D12_HEAP_PROPERTIES default_heap() {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    return heap;
}

bool load_compute_shader(ComPtr<ID3DBlob>& bytecode) {
    HMODULE compiler = LoadLibraryExA("d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(compiler != nullptr, "d3dcompiler_47.dll loads");
    auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    check(compile != nullptr, "d3dcompiler_47.dll exports D3DCompile");
    if (!compile) return false;

    static const char kSource[] =
        "[numthreads(1, 1, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) { }\n";
    ComPtr<ID3DBlob> errors;
    HRESULT hr = compile(kSource, sizeof(kSource) - 1, "device_proxy_test.hlsl", nullptr, nullptr, "main",
                         "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, bytecode.GetAddressOf(),
                         errors.GetAddressOf());
    if (FAILED(hr) && errors) {
        std::printf("shader compile error: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
    }
    check(SUCCEEDED(hr) && bytecode, "trivial compute shader compiles (cs_5_0)");
    return SUCCEEDED(hr) && bytecode;
}

}  // namespace


static bool v16_check_slot108(ID3D12Device* real) {
    if (!real) return false;

    ID3D12Device8* native = nullptr;

    if (FAILED(real->QueryInterface(
            __uuidof(ID3D12Device8),
            reinterpret_cast<void**>(&native))) ||
        !native) return false;

    void* proxy = nullptr;

    real->AddRef();

    const HRESULT hr =
        rt::d3d12_xs::create_device_proxy(
            real, __uuidof(ID3D12Device), &proxy, true);

    if (FAILED(hr) || !proxy) {
        native->Release();
        return false;
    }

    using XboxAllocation =
        D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(
            D3D12_RESOURCE_ALLOCATION_INFO*,
            void*, UINT, UINT,
            const D3D12_RESOURCE_DESC*,
            D3D12_RESOURCE_ALLOCATION_INFO1*);

    auto* table = *reinterpret_cast<void***>(proxy);

    const auto call =
        reinterpret_cast<XboxAllocation>(table[108]);

    bool ok = true;

    for (unsigned kind = 0; kind < 2; ++kind) {
        D3D12_RESOURCE_DESC desc{};

        desc.Dimension = kind == 0
            ? D3D12_RESOURCE_DIMENSION_BUFFER
            : D3D12_RESOURCE_DIMENSION_TEXTURE2D;

        desc.Width = kind == 0 ? 65536 : 32;
        desc.Height = kind == 0 ? 1 : 32;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;

        desc.Format = kind == 0
            ? DXGI_FORMAT_UNKNOWN
            : DXGI_FORMAT_R8G8B8A8_UNORM;

        desc.Layout = kind == 0
            ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
            : D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_RESOURCE_ALLOCATION_INFO1 expected_each{};
        D3D12_RESOURCE_ALLOCATION_INFO1 actual_each{};

        D3D12_RESOURCE_DESC1 native_desc{};

        std::memcpy(&native_desc, &desc, sizeof(desc));
        native_desc.SamplerFeedbackMipRegion = {};

        if (kind == 1) {
            // Confirm console-only descriptor words are
            // normalized on a copy.
            desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x105);
            desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20000);
        }

        const auto expected =
            native->GetResourceAllocationInfo2(
                1, 1, &native_desc, &expected_each);

        D3D12_RESOURCE_ALLOCATION_INFO actual{};

        const auto returned = call(
            &actual, proxy, 1, 1, &desc, &actual_each);

        ok = ok &&
            returned == &actual &&
            expected.SizeInBytes != UINT64_MAX &&
            actual.SizeInBytes == expected.SizeInBytes &&
            actual.Alignment == expected.Alignment &&
            actual_each.Offset == expected_each.Offset &&
            actual_each.Alignment == expected_each.Alignment &&
            actual_each.SizeInBytes == expected_each.SizeInBytes;
    }

    reinterpret_cast<IUnknown*>(proxy)->Release();
    native->Release();

    return ok;
}

int main(int argc, char** argv) {
    const bool v14_debug_enabled =
        GetEnvironmentVariableW(
            L"LEONIDA_DIAG_DEBUG", nullptr, 0) != 0;

    if (v14_debug_enabled) {
        HMODULE dll = LoadLibraryW(L"d3d12.dll");
        using DebugFn = HRESULT(WINAPI*)(REFIID, void**);

        auto fn = dll ? reinterpret_cast<DebugFn>(
            GetProcAddress(dll, "D3D12GetDebugInterface"))
            : nullptr;

        ID3D12Debug* debug = nullptr;

        if (!fn || FAILED(fn(IID_PPV_ARGS(&debug))) || !debug) {
            std::printf("FAIL v14 debug layer unavailable\n");
            return 1;
        }

        debug->EnableDebugLayer();
        debug->Release();
        std::printf("v14 D3D12 debug layer enabled\n");
    }

    // --trace-d3d12, as gta6rt takes it: device_proxy_trace checks that the parameter words still print then.
    if (argc == 2 && std::strcmp(argv[1], "--trace-d3d12") == 0) rt::d3d12_trace::set_enabled(true);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("device_proxy_test: Xbox-ABI device proxy over WARP\n");

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    check(SUCCEEDED(hr) && factory, "DXGI factory created");
    ComPtr<IDXGIAdapter> warp;
    if (factory) hr = factory->EnumWarpAdapter(IID_PPV_ARGS(warp.GetAddressOf()));
    if (!factory) {
        std::printf("device_proxy_test: FAILED (no DXGI factory)\n");
        return 1;
    }
    if (!warp) {
        // Windows always has WARP; Wine doesn't. CTest reads exit code 77 as a skip (SKIP_RETURN_CODE).
        std::printf("SKIP device_proxy_test: no WARP adapter (0x%08lX)\n", static_cast<unsigned long>(hr));
        return 77;
    }
    check(true, "WARP adapter enumerated");

    auto create_device = reinterpret_cast<XboxCreateDeviceFn>(
        rt::shims::find("d3d12_xs.dll", "D3D12XboxCreateDevice"));
    check(create_device != nullptr, "shim registry exposes d3d12_xs!D3D12XboxCreateDevice");
    if (!create_device) return 1;

    std::uint32_t params[24] = {0x1234abcd};  // logged only, not decoded; word 0 is device_proxy_trace's marker
    const IID iid_device = __uuidof(ID3D12Device);
    void* created = nullptr;
    hr = create_device(warp.Get(), params, &iid_device, &created);
    check(SUCCEEDED(hr) && created != nullptr, "D3D12XboxCreateDevice returns a device");
    if (!created) {
        std::printf("device_proxy_test: FAILED (no device)\n");
        return 1;
    }

    ComPtr<ID3D12Device> device;
    device.Attach(static_cast<ID3D12Device*>(created));
    check(rt::d3d12_xs::is_device_proxy(device.Get()), "returned pointer is an Xbox-ABI proxy");

    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device.Get());
    check(real != nullptr, "proxy exposes its PC device");
    check(v16_check_slot108(real),
          "v16 slot108 Xbox allocation ABI matches native D3D12");
    unsigned slots = rt::d3d12_xs::device_proxy_slot_count(device.Get());
    std::printf("proxy forwards %u PC vtable slots (ID3D12Device..Device14 max is 83)\n", slots);
    check(slots >= 44, "proxy forwards at least the base ID3D12Device vtable");
    check(rt::d3d12_xs::device_slot_overridden(9), "device slot 9 is CreateCommandAllocator (allocator proxies)");
    check(!rt::d3d12_xs::device_slot_overridden(10), "device slot 10 is forwarded (PC CreateGraphicsPipelineState)");
    check(rt::d3d12_xs::device_slot_overridden(14), "device slot 14 is the descriptor-heap override");
    check(rt::d3d12_xs::device_slot_overridden(62), "device slot 62 is the compute-PSO override");
    check(rt::d3d12_xs::device_slot_overridden(76), "device slot 76 is the resource override");
    check(rt::d3d12_xs::device_slot_overridden(67),
          "device slot 67 is the DeserializeComputePipelineStateX override (console native-code compute pipeline, not PC CreateProtectedResourceSession1)");

    void** proxy_vtbl = *reinterpret_cast<void***>(device.Get());
    void** real_vtbl = real ? *reinterpret_cast<void***>(real) : nullptr;
    check(proxy_vtbl != nullptr && proxy_vtbl[14] != real_vtbl[14], "slot 14 thunk differs from the PC entry");
    if (slots > 62) check(proxy_vtbl[62] != real_vtbl[62], "slot 62 thunk differs from PC CreateStateObject");
    if (slots > 67) check(proxy_vtbl[67] != real_vtbl[67], "slot 67 thunk differs from PC CreateProtectedResourceSession1");
    if (slots > 76) check(proxy_vtbl[76] != real_vtbl[76], "slot 76 thunk differs from PC CreateCommittedResource3");

    // Xbox struct-return ABI: RCX is the hidden result buffer, RDX is the
    // device, R8/R9 carry the ordinary arguments (PC reverses RCX/RDX).
    // The real WARP device is the oracle for the *values* of both methods.
    check(rt::d3d12_xs::device_slot_overridden(25) && rt::d3d12_xs::device_slot_overridden(26),
          "device slots 25 and 26 have struct-return adapters");
    using XboxHeapPropsFn = std::uintptr_t(WINAPI*)(D3D12_HEAP_PROPERTIES*, void*, UINT, D3D12_HEAP_TYPE);
    auto xbox_heap_props = reinterpret_cast<XboxHeapPropsFn>(proxy_vtbl[26]);
    D3D12_HEAP_PROPERTIES xbox_heap{};
    const auto xbox_heap_pointer = xbox_heap_props(&xbox_heap, device.Get(), 0, D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES real_heap_props = real->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_DEFAULT);
    check(xbox_heap_pointer == reinterpret_cast<std::uintptr_t>(&xbox_heap) &&
              xbox_heap.Type == real_heap_props.Type &&
              xbox_heap.CPUPageProperty == real_heap_props.CPUPageProperty &&
              xbox_heap.MemoryPoolPreference == real_heap_props.MemoryPoolPreference &&
              xbox_heap.CreationNodeMask == real_heap_props.CreationNodeMask &&
              xbox_heap.VisibleNodeMask == real_heap_props.VisibleNodeMask,
          "slot 26 Xbox sret order returns the actual host heap properties");
    const D3D12_HEAP_PROPERTIES pc_heap_props = device->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_DEFAULT);
    check(std::memcmp(&pc_heap_props, &real_heap_props, sizeof pc_heap_props) == 0,
          "slot 26 also retains native PC C++ sret order");

    D3D12_RESOURCE_DESC info_desc = buffer_desc();
    info_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    using XboxAllocationInfoFn = std::uintptr_t(WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT,
                                                         const D3D12_RESOURCE_DESC*);
    auto xbox_allocation_info = reinterpret_cast<XboxAllocationInfoFn>(proxy_vtbl[25]);
    D3D12_RESOURCE_ALLOCATION_INFO xbox_info{};
    const auto xbox_info_pointer = xbox_allocation_info(&xbox_info, device.Get(), 0, 1, &info_desc);
    const D3D12_RESOURCE_ALLOCATION_INFO real_info = real->GetResourceAllocationInfo(0, 1, &info_desc);
    check(xbox_info_pointer == reinterpret_cast<std::uintptr_t>(&xbox_info) &&
              xbox_info.SizeInBytes == real_info.SizeInBytes && xbox_info.Alignment == real_info.Alignment,
          "slot 25 Xbox sret order returns the actual host allocation info");
    const D3D12_RESOURCE_ALLOCATION_INFO pc_info = device->GetResourceAllocationInfo(0, 1, &info_desc);
    check(pc_info.SizeInBytes == real_info.SizeInBytes && pc_info.Alignment == real_info.Alignment,
          "slot 25 also retains native PC C++ sret order");

    // Avoid an out-of-bounds vtable read if startup reaches Xbox slot 133.
    check(rt::d3d12_xs::device_slot_overridden(133) && proxy_vtbl[133] != nullptr,
          "slot 133 has a guarded entry instead of a null vtable pointer");
    using Slot133Fn = HRESULT(WINAPI*)(void*, UINT64, const D3D12_RESOURCE_DESC*,
                                         D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                                         const IID*, void**);
    const IID iid_placed = __uuidof(ID3D12Resource);
    void* missing_resource = reinterpret_cast<void*>(0x1234);
    auto make_placed = reinterpret_cast<Slot133Fn>(proxy_vtbl[133]);
    const HRESULT invalid_placed = make_placed(device.Get(), 0, nullptr,
        D3D12_RESOURCE_STATE_COMMON, nullptr, &iid_placed, &missing_resource);
    check(invalid_placed == E_POINTER && missing_resource == nullptr,
          "slot 133 refuses missing desc and initializes the output pointer");
    D3D12_RESOURCE_DESC placed_desc = buffer_desc();
    placed_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x100000u);
    ComPtr<ID3D12Resource> placed_buffer;
    const HRESULT placed_hr = make_placed(device.Get(), 0x10000000ull, &placed_desc,
        static_cast<D3D12_RESOURCE_STATES>(0xffffffffu), nullptr, &iid_placed,
        reinterpret_cast<void**>(placed_buffer.GetAddressOf()));
    check(SUCCEEDED(placed_hr) && placed_buffer &&
          placed_buffer->GetDesc().Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR &&
          placed_buffer->GetDesc().Flags == D3D12_RESOURCE_FLAG_NONE,
          "slot 133 creates a separate PC buffer with normalised Xbox flags and layout");

    // The hit counters are for tests and count from the first read or reset, so that a title's calls write no
    // shared counter. The QueryInterface of the creation above was before that: its flag is set, its hit is not.
    check(rt::d3d12_xs::device_slot_first_call_seen(0), "slot 0 first-call flag is set by the creation");
    check(rt::d3d12_xs::device_slot_hits(0) == 0, "slot 0 has no hit before the first read of a counter");
    device->AddRef();
    device->Release();
    check(rt::d3d12_xs::device_slot_hits(1) == 1 && rt::d3d12_xs::device_slot_hits(2) == 1,
          "slots 1 and 2 count the calls after the first read");

    rt::d3d12_xs::reset_device_proxy_stats();

    // QI/refcount behaviour: device interfaces resolve to the same proxy, non-device IIDs do not.
    ID3D12Device* same = nullptr;
    hr = device->QueryInterface(__uuidof(ID3D12Device), reinterpret_cast<void**>(&same));
    check(SUCCEEDED(hr) && same == device.Get(), "QueryInterface(IID_ID3D12Device) returns the same proxy");
    if (same) same->Release();
    ComPtr<ID3D12Device1> device1;
    hr = device->QueryInterface(IID_PPV_ARGS(device1.GetAddressOf()));
    check(SUCCEEDED(hr) && device1 != nullptr, "QueryInterface(IID_ID3D12Device1) succeeds");
    ComPtr<ID3D12CommandQueue> not_a_device;
    hr = device->QueryInterface(IID_PPV_ARGS(not_a_device.GetAddressOf()));
    check(FAILED(hr) && !not_a_device, "QueryInterface rejects a non-device IID");
    check(rt::d3d12_xs::device_slot_hits(0) >= 3, "slot 0 hit counter moves");

    // Generic-forwarder coverage: slot 43 returns an 8-byte struct through the hidden pointer, and slot 30 has
    // two stack arguments. Both must reach the PC device unchanged (they are not measured overrides).
    const LUID proxy_luid = device->GetAdapterLuid();
    const LUID real_luid = real ? real->GetAdapterLuid() : LUID{};
    check(proxy_luid.LowPart == real_luid.LowPart && proxy_luid.HighPart == real_luid.HighPart,
          "slot 43 forwards GetAdapterLuid (struct return)");
    D3D12_RESOURCE_DESC reserved_desc = buffer_desc();
    reserved_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> reserved;
    hr = device->CreateReservedResource(&reserved_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        IID_PPV_ARGS(reserved.GetAddressOf()));
    check(SUCCEEDED(hr) && reserved != nullptr, "slot 30 forwards CreateReservedResource (stack arguments)");

    // Slot 14: descriptor heap through the proxy. The proxy wraps the real PC heap for the handle ABI.
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 8;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    const unsigned long refs_before_heap = rt::d3d12_xs::device_proxy_ref_count(device.Get());
    hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(heap.GetAddressOf()));
    check(SUCCEEDED(hr) && heap != nullptr, "slot 14 creates a descriptor heap");
    check(rt::d3d12_xs::device_slot_hits(14) == 1, "slot 14 hit counter is 1");
    check(heap && rt::d3d12_xs::device_proxy_ref_count(device.Get()) == refs_before_heap + 1,
          "heap holds one device proxy reference");

    if (heap) {
        check(rt::d3d12_xs::is_heap_proxy(heap.Get()), "slot 14 returns a heap proxy");
        ID3D12DescriptorHeap* real_heap = rt::d3d12_xs::heap_proxy_real_heap(heap.Get());
        check(real_heap != nullptr, "heap proxy exposes the PC heap");
        check(rt::d3d12_xs::heap_slot_overridden(9), "heap slot 9 is the CPU-handle override");
        check(rt::d3d12_xs::heap_slot_overridden(10), "heap slot 10 is the GPU-handle override");
        ID3D12DescriptorHeap* same_heap = nullptr;
        hr = heap->QueryInterface(__uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&same_heap));
        check(SUCCEEDED(hr) && same_heap == heap.Get(), "heap QueryInterface returns the same heap proxy");
        if (same_heap) same_heap->Release();

        // ID3D12DeviceChild::GetDevice must return the Xbox-ABI device, not the raw PC device, and AddRef it.
        check(rt::d3d12_xs::heap_slot_overridden(7), "heap slot 7 is the GetDevice override");
        ID3D12Device* heap_device = nullptr;
        const unsigned long refs_before_get = rt::d3d12_xs::device_proxy_ref_count(device.Get());
        hr = heap->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&heap_device));
        check(SUCCEEDED(hr) && heap_device == device.Get() && rt::d3d12_xs::is_device_proxy(heap_device),
              "heap GetDevice returns the device proxy");
        check(rt::d3d12_xs::device_proxy_ref_count(device.Get()) == refs_before_get + 1,
              "GetDevice AddRefs the device proxy");
        check(rt::d3d12_xs::heap_slot_hits(7) == 1, "heap slot 7 hit counter is 1");
        if (heap_device) heap_device->Release();
        check(rt::d3d12_xs::device_proxy_ref_count(device.Get()) == refs_before_get,
              "GetDevice release balances");

        // The Xbox call shape: `this` only, 8-byte handle in RAX. The proxy converts from the PC heap's
        // hidden return pointer (RDX). A PC C++ call through the proxy uses the PC convention, so it is not
        // used here; the real heap below shows that convention and the conversion is compared against it.
        void** proxy_heap_vtbl = *reinterpret_cast<void***>(heap.Get());
        auto get_cpu = reinterpret_cast<GetCpuHandleFn>(proxy_heap_vtbl[9]);
        auto get_gpu = reinterpret_cast<GetGpuHandleFn>(proxy_heap_vtbl[10]);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = get_cpu(heap.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = get_gpu(heap.Get());
        check(cpu.ptr != 0, "heap proxy slot 9 returns a CPU handle in RAX");
        check(gpu.ptr != 0, "heap proxy slot 10 returns a GPU handle in RAX");
        if (real_heap) {
            void** real_heap_vtbl = *reinterpret_cast<void***>(real_heap);
            check(proxy_heap_vtbl[9] != real_heap_vtbl[9], "heap slot 9 thunk differs from the PC heap entry");
            check(proxy_heap_vtbl[10] != real_heap_vtbl[10], "heap slot 10 thunk differs from the PC heap entry");
        }

        // PC reality check: the real heap takes a hidden return pointer in RDX. This is the deviation that
        // makes the wrapper necessary; a one-argument call on the real heap is invalid and is not made.
        if (real_heap) {
            void** real_heap_vtbl = *reinterpret_cast<void***>(real_heap);
            using GetCpuOutFn = void(WINAPI*)(ID3D12DescriptorHeap*, D3D12_CPU_DESCRIPTOR_HANDLE*);
            using GetGpuOutFn = void(WINAPI*)(ID3D12DescriptorHeap*, D3D12_GPU_DESCRIPTOR_HANDLE*);
            constexpr SIZE_T kSentinel = 0x1111111111111111ull;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu_pc{};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu_pc{};
            cpu_pc.ptr = kSentinel;
            gpu_pc.ptr = kSentinel;
            reinterpret_cast<GetCpuOutFn>(real_heap_vtbl[9])(real_heap, &cpu_pc);
            reinterpret_cast<GetGpuOutFn>(real_heap_vtbl[10])(real_heap, &gpu_pc);
            check(cpu_pc.ptr != kSentinel, "PC heap slot 9 writes through the RDX return pointer");
            check(gpu_pc.ptr != kSentinel, "PC heap slot 10 writes through the RDX return pointer");
            check(cpu_pc.ptr == cpu.ptr, "proxy CPU handle equals the PC heap's hidden-pointer result");
            check(gpu_pc.ptr == gpu.ptr, "proxy GPU handle equals the PC heap's hidden-pointer result");
        }

        const D3D12_DESCRIPTOR_HEAP_DESC got_desc = heap->GetDesc();
        check(got_desc.NumDescriptors == heap_desc.NumDescriptors, "heap slot 8 forwards GetDesc");
        check(rt::d3d12_xs::heap_slot_hits(9) >= 1, "heap slot 9 hit counter moves");
        check(rt::d3d12_xs::heap_slot_hits(10) >= 1, "heap slot 10 hit counter moves");
        check(rt::d3d12_xs::heap_slot_first_call_seen(9), "heap slot 9 first-call flag is set");
    }

    // Slot 9 on the device must remain CreateCommandAllocator, not a handle getter.
    ComPtr<ID3D12CommandAllocator> allocator;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()));
    check(SUCCEEDED(hr) && allocator != nullptr, "device slot 9 forwards CreateCommandAllocator");
    // A PC runtime allocator is not wrapped: the PC runtime's lists could not resolve an allocator proxy.
    check(!rt::d3d12_xs::is_allocator_proxy(allocator.Get()), "slot 9 hands out the PC allocator unwrapped");
    check(rt::d3d12_xs::device_slot_hits(9) == 1, "slot 9 hit counter is 1");

    // Slot 76: buffer resource with the measured Xbox layout word 0x101; PC committed resources take ROW_MAJOR.
    const IID iid_resource = __uuidof(ID3D12Resource);
    auto create_resource = reinterpret_cast<XboxCreateResourceFn>(proxy_vtbl[76]);
    const D3D12_HEAP_PROPERTIES heap_properties = default_heap();
    D3D12_RESOURCE_DESC xbox_buffer = buffer_desc();
    ComPtr<ID3D12Resource> buffer;
    hr = create_resource(device.Get(), &heap_properties, D3D12_HEAP_FLAG_NONE, &xbox_buffer,
                         D3D12_RESOURCE_STATE_COMMON, nullptr, iid_resource,
                         reinterpret_cast<void**>(buffer.GetAddressOf()));
    check(SUCCEEDED(hr) && buffer != nullptr, "slot 76 creates a committed buffer");
    if (buffer) {
        const D3D12_RESOURCE_DESC got = buffer->GetDesc();
        check(got.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR, "slot 76 maps 0x101 to ROW_MAJOR for a buffer");
        check(got.Width == 4096, "slot 76 preserves the resource width");
    }

    // The same Xbox word on a texture maps to UNKNOWN, which is what PC committed textures require.
    D3D12_RESOURCE_DESC xbox_texture = texture_desc();
    ComPtr<ID3D12Resource> texture;
    hr = create_resource(device.Get(), &heap_properties, D3D12_HEAP_FLAG_NONE, &xbox_texture,
                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, iid_resource,
                         reinterpret_cast<void**>(texture.GetAddressOf()));
    check(SUCCEEDED(hr) && texture != nullptr, "slot 76 creates a committed texture");
    if (texture) {
        check(texture->GetDesc().Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN,
              "slot 76 maps 0x101 to UNKNOWN for a texture");
    }
    check(rt::d3d12_xs::device_slot_hits(76) == 2, "slot 76 hit counter is 2");
    check(rt::d3d12_xs::device_slot_first_call_seen(76), "slot 76 first-call flag is set");

    // Xbox device slots from the public HLE. On the Vulkan device the PC entries at 78/79 are
    // CreateReservedResource2/CreateSampler2 and 80 is past the 80-slot table, so success here can only come from
    // the routes: frame events return S_OK and a fresh token, CreateCommandQueueX returns a queue proxy and
    // CreateCommandListX (desc, allocator, initialState, riid, out; no node mask) reaches PC CreateCommandList.
    {
        using SetFrameIntervalFn = HRESULT(WINAPI*)(void*, void*, UINT, UINT, UINT);
        using ScheduleFrameEventFn = HRESULT(WINAPI*)(void*, UINT, UINT, void*, UINT);
        using WaitFrameEventFn = HRESULT(WINAPI*)(void*, UINT, UINT, void*, UINT, UINT64*);
        using CreateQueueXFn = HRESULT(WINAPI*)(void*, const D3D12_COMMAND_QUEUE_DESC*, const IID*, void**);
        using CreateListXFn = HRESULT(WINAPI*)(void*, const void*, ID3D12CommandAllocator*, ID3D12PipelineState*,
                                               const IID*, void**);

        hr = reinterpret_cast<SetFrameIntervalFn>(proxy_vtbl[78])(device.Get(), nullptr, 16667, 1, 0);
        check(hr == S_OK, "slot 78 SetFrameIntervalX returns S_OK");
        hr = reinterpret_cast<ScheduleFrameEventFn>(proxy_vtbl[79])(device.Get(), 0, 0, nullptr, 0);
        check(hr == S_OK, "slot 79 ScheduleFrameEventX returns S_OK");

        std::uint64_t token = 0, token2 = 0;
        hr = reinterpret_cast<WaitFrameEventFn>(proxy_vtbl[80])(device.Get(), 0, INFINITE, nullptr, 0, &token);
        check(hr == S_OK && token != 0, "slot 80 WaitFrameEventX returns a non-zero token");
        hr = reinterpret_cast<WaitFrameEventFn>(proxy_vtbl[80])(device.Get(), 0, 0, nullptr, 0, &token2);
        check(hr == S_OK && token2 != 0 && token2 != token, "slot 80 returns a fresh token on the next wait");
        hr = reinterpret_cast<WaitFrameEventFn>(proxy_vtbl[80])(device.Get(), 0, 0, nullptr, 0, nullptr);
        check(hr == E_POINTER, "slot 80 refuses a null token with E_POINTER");
        check(rt::d3d12_xs::device_slot_hits(78) == 1 && rt::d3d12_xs::device_slot_hits(79) == 1 &&
                  rt::d3d12_xs::device_slot_hits(80) == 3,
              "slots 78/79/80 hit counters are 1/1/3");

        D3D12_COMMAND_QUEUE_DESC xbox_queue{};
        xbox_queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        // The guest can supply console-only bits. The Xbox slot 48 adapter
        // strips those bits before the host D3D12/Vulkan queue sees them.
        xbox_queue.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(0x40000000u);
        const IID iid_queue = __uuidof(ID3D12CommandQueue);
        ComPtr<ID3D12CommandQueue> xbox_queue_proxy;
        hr = reinterpret_cast<CreateQueueXFn>(proxy_vtbl[48])(device.Get(), &xbox_queue, &iid_queue,
                                                               reinterpret_cast<void**>(xbox_queue_proxy.GetAddressOf()));
        check(hr == S_OK && rt::d3d12_xs::is_queue_proxy(xbox_queue_proxy.Get()),
              "slot 48 CreateCommandQueueX returns a queue proxy");
        if (xbox_queue_proxy) {
            const D3D12_COMMAND_QUEUE_DESC got = rt::d3d12_xs::queue_proxy_real_queue(xbox_queue_proxy.Get())->GetDesc();
            check(got.Flags == D3D12_COMMAND_QUEUE_FLAG_NONE,
                  "slot 48 removes console-only flags from the actual queue description");
        }
        check(rt::d3d12_xs::device_slot_hits(48) == 1, "slot 48 hit counter is 1");

        ComPtr<ID3D12CommandAllocator> xbox_allocator;
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(xbox_allocator.GetAddressOf()));
        struct XboxCommandListDesc {
            UINT Type;
            UINT NodeMask;
        } list_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 7};  // the node mask is ignored by the console call
        const IID iid_list = __uuidof(ID3D12GraphicsCommandList);
        ComPtr<ID3D12GraphicsCommandList> xbox_list;
        hr = SUCCEEDED(hr) ? reinterpret_cast<CreateListXFn>(proxy_vtbl[73])(
                                 device.Get(), &list_desc, xbox_allocator.Get(), nullptr, &iid_list,
                                 reinterpret_cast<void**>(xbox_list.GetAddressOf()))
                           : hr;
        check(hr == S_OK && xbox_list != nullptr, "slot 73 CreateCommandListX returns a command list");
        check(rt::d3d12_xs::device_slot_hits(73) == 1, "slot 73 hit counter is 1");
    }

    // Slot 62: an empty root signature and a trivial compute shader through the Xbox signature. The PC entry at
    // slot 62 is CreateStateObject, so success here proves the override reached CreateComputePipelineState.
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> root_error;
    hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, root_blob.GetAddressOf(),
                                     root_error.GetAddressOf());
    check(SUCCEEDED(hr) && root_blob != nullptr, "empty root signature serializes");
    ComPtr<ID3D12RootSignature> root_signature;
    if (root_blob) {
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(root_signature.GetAddressOf()));
    }
    check(SUCCEEDED(hr) && root_signature != nullptr, "device slot 16 forwards CreateRootSignature");

    ComPtr<ID3DBlob> bytecode;
    if (load_compute_shader(bytecode) && root_signature) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root_signature.Get();
        pso_desc.CS.pShaderBytecode = bytecode->GetBufferPointer();
        pso_desc.CS.BytecodeLength = bytecode->GetBufferSize();
        pso_desc.NodeMask = 0;
        pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

        auto create_pso = reinterpret_cast<XboxCreateComputePsoFn>(proxy_vtbl[62]);
        const IID iid_pso = __uuidof(ID3D12PipelineState);
        ComPtr<ID3D12PipelineState> pso;
        hr = create_pso(device.Get(), &pso_desc, 0, 0, iid_pso, reinterpret_cast<void**>(pso.GetAddressOf()));
        check(SUCCEEDED(hr) && pso != nullptr, "slot 62 creates a compute PSO via CreateComputePipelineState");
        check(rt::d3d12_xs::device_slot_hits(62) == 1, "slot 62 hit counter is 1");
        check(rt::d3d12_xs::device_slot_first_call_seen(62), "slot 62 first-call flag is set");

        // Slot 67: Xbox native compute pipeline creation. Maps the console descriptor (+0x00 root signature,
        // +0x28 kind 6 code blob, +0x30 code size) to D3D12_COMPUTE_PIPELINE_STATE_DESC and calls CreateComputePipelineState.
        struct XboxNativeComputeDesc {
            ID3D12RootSignature* root_signature;
            const void* kind0;
            std::uint64_t kind0_size;
            const void* kind1;
            std::uint64_t kind1_size;
            const void* kind6;
            std::uint64_t kind6_size;
            const void* kind7;
            std::uint64_t kind7_size;
        } xbox_desc{};
        xbox_desc.root_signature = root_signature.Get();
        xbox_desc.kind6 = bytecode->GetBufferPointer();
        xbox_desc.kind6_size = bytecode->GetBufferSize();

        using XboxCreateNativeComputePsoFn = HRESULT(WINAPI*)(void*, const void*, const IID*, void**);
        auto create_native_pso = reinterpret_cast<XboxCreateNativeComputePsoFn>(proxy_vtbl[67]);
        ComPtr<ID3D12PipelineState> pso67;
        hr = create_native_pso(device.Get(), &xbox_desc, &iid_pso, reinterpret_cast<void**>(pso67.GetAddressOf()));
        check(SUCCEEDED(hr) && pso67 != nullptr, "slot 67 creates a compute PSO via CreateComputePipelineState");
        check(rt::d3d12_xs::device_slot_hits(67) == 1, "slot 67 hit counter is 1");
        check(rt::d3d12_xs::device_slot_first_call_seen(67), "slot 67 first-call flag is set");
    }

    // _COM_Outptr_ (d3d12.h): a failed create leaves NULL in the output. WARP accepts invalid heap descriptions,
    // so the PC-failure case for slot 14 is in d3d12_vulkan_device_test; these are the proxy's own early returns.
    {
        void* proxy_out = reinterpret_cast<void*>(0x1111);
        using Slot14Fn = HRESULT(WINAPI*)(void*, const D3D12_DESCRIPTOR_HEAP_DESC*, const IID*, void**);
        hr = reinterpret_cast<Slot14Fn>(proxy_vtbl[14])(device.Get(), &heap_desc, nullptr, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 14 nulls its output for a null IID");
        using Slot62Fn = HRESULT(WINAPI*)(void*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, std::uint64_t,
                                          std::uint64_t, const IID*, void**);
        proxy_out = reinterpret_cast<void*>(0x1111);
        D3D12_COMPUTE_PIPELINE_STATE_DESC empty_pso{};
        hr = reinterpret_cast<Slot62Fn>(proxy_vtbl[62])(device.Get(), &empty_pso, 0, 0, nullptr, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 62 nulls its output for a null IID");
        using Slot67Fn = HRESULT(WINAPI*)(void*, const void*, const IID*, void**);
        proxy_out = reinterpret_cast<void*>(0x1111);
        const IID iid_pso = __uuidof(ID3D12PipelineState);
        hr = reinterpret_cast<Slot67Fn>(proxy_vtbl[67])(device.Get(), nullptr, &iid_pso, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 67 nulls its output for a null description");
        proxy_out = reinterpret_cast<void*>(0x1111);
        hr = reinterpret_cast<Slot67Fn>(proxy_vtbl[67])(device.Get(), &root_desc, nullptr, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 67 nulls its output for a null IID");
        using Slot76Fn = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                          const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                          const D3D12_CLEAR_VALUE*, const IID*, void**);
        proxy_out = reinterpret_cast<void*>(0x1111);
        hr = reinterpret_cast<Slot76Fn>(proxy_vtbl[76])(device.Get(), &heap_properties, D3D12_HEAP_FLAG_NONE,
                                                         nullptr, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         &iid_resource, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 76 nulls its output for a null description");
    }

    // Slots 8 and 36 on the device the shim made: a title gets queue and fence proxies, and they run GPU work.
    {
        using CreateQueueFn = HRESULT(WINAPI*)(void*, const D3D12_COMMAND_QUEUE_DESC*, const IID*, void**);
        using CreateFenceFn = HRESULT(WINAPI*)(void*, UINT64, D3D12_FENCE_FLAGS, const IID*, void**);
        void** vtable = *reinterpret_cast<void***>(device.Get());
        const D3D12_COMMAND_QUEUE_DESC queue_desc{};
        const IID iid_queue = __uuidof(ID3D12CommandQueue);
        const IID iid_fence = __uuidof(ID3D12Fence);
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> fence;
        hr = reinterpret_cast<CreateQueueFn>(vtable[8])(device.Get(), &queue_desc, &iid_queue,
                                                        reinterpret_cast<void**>(queue.GetAddressOf()));
        check(SUCCEEDED(hr) && rt::d3d12_xs::is_queue_proxy(queue.Get()), "slot 8 returns a queue proxy");
        hr = reinterpret_cast<CreateFenceFn>(vtable[36])(device.Get(), 0, D3D12_FENCE_FLAG_NONE, &iid_fence,
                                                         reinterpret_cast<void**>(fence.GetAddressOf()));
        check(SUCCEEDED(hr) && rt::d3d12_xs::is_fence_proxy(fence.Get()), "slot 36 returns a fence proxy");
        if (queue && fence) {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            check(event && SUCCEEDED(queue->Signal(fence.Get(), 1)) &&
                      SUCCEEDED(fence->SetEventOnCompletion(1, event)) && WaitForSingleObject(event, 5000) == 0,
                  "the queue proxy signals the fence proxy on the GPU");
            if (event) CloseHandle(event);
            ComPtr<ID3D12Device> owner;
            check(SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(owner.GetAddressOf()))) && owner.Get() == device.Get(),
                  "the queue proxy's GetDevice is the device the shim returned");
        }
    }

    // Title-exported __XMemAllocationHooks__ backing for CreateDescriptorHeap (slot 14) + CPU handle translation
    // against the real WARP D3D12 runtime.
    {
        struct HookState {
            alignas(64) std::uint8_t backing[4096]{};
            std::size_t alloc_size = 0;
            std::uint64_t alloc_attr = 0;
            void* freed_ptr = nullptr;
            std::uint64_t freed_attr = 0;
            int alloc_count = 0;
            int free_count = 0;
        };
        static HookState hook_state;
        hook_state = {};
        struct HookCallbacks {
            static void* __stdcall alloc(std::size_t size, std::uint64_t attr) {
                ++hook_state.alloc_count;
                hook_state.alloc_size = size;
                hook_state.alloc_attr = attr;
                if ((attr & 0x3FFFull) == 4 && size <= sizeof(hook_state.backing)) return hook_state.backing;
                return nullptr;
            }
            static void __stdcall free(void* ptr, std::uint64_t attr) {
                ++hook_state.free_count;
                hook_state.freed_ptr = ptr;
                hook_state.freed_attr = attr;
            }
        };
        const rt::shims::XMemAllocationHooks title_hooks{&HookCallbacks::alloc, &HookCallbacks::free};
        rt::shims::add_xmem_allocation_hooks(&title_hooks, &title_hooks);

        D3D12_DESCRIPTOR_HEAP_DESC hooked_desc{};
        hooked_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hooked_desc.NumDescriptors = 4;
        hooked_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const UINT inc = device->GetDescriptorHandleIncrementSize(hooked_desc.Type);
        ComPtr<ID3D12DescriptorHeap> hooked_heap;
        hr = device->CreateDescriptorHeap(&hooked_desc, IID_PPV_ARGS(hooked_heap.GetAddressOf()));
        check(SUCCEEDED(hr) && hooked_heap != nullptr, "hooked slot 14 creates a descriptor heap");
        check(hook_state.alloc_count == 1 && hook_state.alloc_size == hooked_desc.NumDescriptors * inc &&
                  hook_state.alloc_attr == rt::d3d12_xs::kXmemDescriptorHeapAttributes,
              "slot 14 invokes __XMemAllocationHooks__ with heap byte size and D3D descriptor-heap attributes");
        if (hooked_heap) {
            void** hooked_vtbl = *reinterpret_cast<void***>(hooked_heap.Get());
            const auto hooked_cpu = reinterpret_cast<GetCpuHandleFn>(hooked_vtbl[9])(hooked_heap.Get());
            const auto hooked_gpu = reinterpret_cast<GetGpuHandleFn>(hooked_vtbl[10])(hooked_heap.Get());
            const auto expected_base = reinterpret_cast<SIZE_T>(hook_state.backing);
            check(hooked_cpu.ptr == expected_base,
                  "hooked heap slot 9 returns the title-supplied CPU descriptor base");
            check(hooked_gpu.ptr != 0, "hooked heap slot 10 returns a valid GPU descriptor start");

            // Create a CBV at slot 1 of the title-backed heap; WARP validates the translated real handle.
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
            cbv.BufferLocation = 0;
            cbv.SizeInBytes = 256;
            device->CreateConstantBufferView(&cbv, D3D12_CPU_DESCRIPTOR_HANDLE{expected_base + inc});
            const auto real_cpu = rt::d3d12_xs::heap_proxy_real_heap(hooked_heap.Get())->GetCPUDescriptorHandleForHeapStart();
            const auto translated = rt::d3d12_xs::translate_cpu_descriptor_handle(
                device.Get(), D3D12_CPU_DESCRIPTOR_HANDLE{expected_base + inc});
            check(translated.ptr == real_cpu.ptr + inc,
                  "title-backed CPU descriptor handle translates to the actual host heap slot");
        }
        hooked_heap.Reset();
        check(hook_state.free_count == 1 && hook_state.freed_ptr == hook_state.backing &&
                  hook_state.freed_attr == rt::d3d12_xs::kXmemDescriptorHeapAttributes,
              "releasing the hooked heap frees its backing through __XMemAllocationHooks__");
        rt::shims::remove_xmem_allocation_hooks(&title_hooks);
    }

    // Xbox device slot 127: invalid descriptor/arguments must fail without touching GPU memory.
    {
        using XboxCreateResource3Fn = HRESULT(WINAPI*)(void*, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t, std::uint64_t);
        auto call127 = reinterpret_cast<XboxCreateResource3Fn>(proxy_vtbl[127]);
        check(rt::d3d12_xs::device_slot_overridden(127), "slot 127 routes Xbox CreateCommittedResource3");
        const HRESULT missing = call127(device.Get(), 0,0,0,0,0,0,0,0,0,0);
        check(missing == E_POINTER, "slot 127 rejects missing output and descriptor");
    }

    // Exercise slot 70 in console mode, while the original PC-shaped tests
    // continue to use the native slots. No game executable is required.
    {
        ComPtr<ID3D12Device> console_device;
        real->AddRef(); // create_device_proxy consumes this reference on success.
        const HRESULT setup = rt::d3d12_xs::create_device_proxy(real, __uuidof(ID3D12Device),
            reinterpret_cast<void**>(console_device.GetAddressOf()), true);
        if (FAILED(setup)) real->Release();
        check(SUCCEEDED(setup) && console_device, "v10 console device creation");
        if (console_device) {
            void** cv = *reinterpret_cast<void***>(console_device.Get());
            D3D12_INDIRECT_ARGUMENT_DESC indirect{};
            indirect.Type = static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(105u);
            D3D12_COMMAND_SIGNATURE_DESC signature_desc{12, 1, &indirect, 0};
            ComPtr<ID3D12CommandSignature> signature;
            const auto signature_hr = console_device->CreateCommandSignature(&signature_desc, nullptr,
                                                         IID_PPV_ARGS(signature.GetAddressOf()));
            check(SUCCEEDED(signature_hr) && signature, "v12 console indirect signature maps to actual DISPATCH");
            check(static_cast<UINT>(indirect.Type) == 105u, "v12 leaves caller signature unchanged");
            signature.Reset();
            signature_desc.ByteStride = 8;
            check(FAILED(console_device->CreateCommandSignature(&signature_desc, nullptr,
                      IID_PPV_ARGS(signature.GetAddressOf()))) && !signature, "v12 refuses invalid console stride");
            const IID resource_iid = __uuidof(ID3D12Resource);
            using Place = HRESULT(WINAPI*)(void*, UINT64, const D3D12_RESOURCE_DESC*,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, const IID*, void**);
            using RawSrv = HRESULT(WINAPI*)(void*, const D3D12_RESOURCE_DESC*, const void*, SIZE_T);
            constexpr UINT64 guest_base = 0x30000000ull;
            auto desc = buffer_desc();
            ComPtr<ID3D12Resource> placed;
            const HRESULT placed_result = reinterpret_cast<Place>(cv[133])(console_device.Get(), guest_base,
                &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, &resource_iid,
                reinterpret_cast<void**>(placed.GetAddressOf()));
            check(SUCCEEDED(placed_result) && placed, "v10 tracks console slot-133 buffer");
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = 4;
            ComPtr<ID3D12DescriptorHeap> test_heap;
            const HRESULT heap_result = console_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(test_heap.GetAddressOf()));
            check(SUCCEEDED(heap_result) && test_heap, "v10 creates tracked descriptor heap");
            if (placed && test_heap) {
                void** hv = *reinterpret_cast<void***>(test_heap.Get());
                const auto handle = reinterpret_cast<GetCpuHandleFn>(hv[9])(test_heap.Get()).ptr;
                const UINT stride = console_device->GetDescriptorHandleIncrementSize(hd.Type);
                struct View { D3D12_SHADER_RESOURCE_VIEW_DESC srv; UINT64 va; } view{};
                static_assert(offsetof(View, va) == 0x28);
                view.srv.Format = DXGI_FORMAT_R32_TYPELESS;
                view.srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                view.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                view.srv.Buffer.FirstElement = 4;
                view.srv.Buffer.NumElements = 16;
                view.srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                view.va = guest_base + 64;
                auto create_srv = reinterpret_cast<RawSrv>(cv[70]);
                check(create_srv(console_device.Get(), &desc, &view, handle) == S_OK,
                      "v10 Xbox slot 70 resolves a buffer address and writes a real SRV");
                check(view.srv.Buffer.FirstElement == 4 &&
                      view.va == guest_base + 64,
                      "v10 preserves the caller's SRV description");

                // Translated FirstElement=20: valid 80-byte RAW offset.
                check(real->GetDeviceRemovedReason() == S_OK,
                      "v14 aligned RAW view preserves device health");

                // Translated FirstElement=18: invalid 72-byte RAW offset.
                view.srv.Buffer.FirstElement = 2;
                check(create_srv(console_device.Get(), &desc, &view,
                                 handle + stride) == S_FALSE,
                      "v14 rejects unaligned translated RAW view with null fallback");
                check(view.srv.Buffer.FirstElement == 2 &&
                      view.va == guest_base + 64,
                      "v14 preserves rejected caller description");
                check(real->GetDeviceRemovedReason() == S_OK,
                      "v14 unaligned RAW view preserves device health");

                view.srv.Buffer.FirstElement = 4;

                view.srv.Buffer.NumElements = 0;
                check(create_srv(console_device.Get(), &desc, &view,
                                 handle + 2 * stride) == S_FALSE,
                      "v14 rejects zero-element RAW view");
                view.srv.Buffer.NumElements = 16;

                view.srv.Format = DXGI_FORMAT_R32_FLOAT;
                check(create_srv(console_device.Get(), &desc, &view,
                                 handle + 2 * stride) == S_FALSE,
                      "v14 rejects invalid RAW format");
                view.srv.Format = DXGI_FORMAT_R32_TYPELESS;

                view.srv.Shader4ComponentMapping = 0;
                check(create_srv(console_device.Get(), &desc, &view,
                                 handle + 2 * stride) == S_FALSE,
                      "v14 rejects invalid RAW component mapping");
                view.srv.Shader4ComponentMapping =
                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

                check(real->GetDeviceRemovedReason() == S_OK,
                      "v14 malformed RAW views preserve device health");
                view.va = guest_base + desc.Width - 4;
                check(create_srv(console_device.Get(), &desc, &view, handle + stride) == S_FALSE,
                      "v10 bounds-checks an SRV that extends outside its placed buffer");
                check(create_srv(console_device.Get(), &desc, &view, handle + 1) == E_INVALIDARG,
                      "v10 rejects misaligned descriptor destination");
                check(create_srv(console_device.Get(), &desc, &view, 1) == E_INVALIDARG,
                      "v10 rejects untracked descriptor destination");
                check(create_srv(console_device.Get(), nullptr, &view, handle + 2 * stride) == E_INVALIDARG,
                      "v10 rejects missing description");
                check(real->GetDeviceRemovedReason() == S_OK,
                      "v14 device remains healthy after all slot 70 tests");
            }
        }
    }

    // Synthetic console sampler ABI test, with no guest executable or shader assets.
    {
        ComPtr<ID3D12Device> console_device;
        real->AddRef();
        const HRESULT setup = rt::d3d12_xs::create_device_proxy(real, __uuidof(ID3D12Device),
            reinterpret_cast<void**>(console_device.GetAddressOf()), true);
        if (FAILED(setup)) real->Release();
        check(SUCCEEDED(setup) && console_device, "v13 creates console device for sampler test");
        if (console_device) {
            auto** cv = *reinterpret_cast<void***>(console_device.Get());
            using SamplerX = HRESULT(WINAPI*)(void*, const void*, SIZE_T);
            const auto create = reinterpret_cast<SamplerX>(cv[61]);
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            hd.NumDescriptors = 2;

            ComPtr<ID3D12DescriptorHeap> native_probe;
            const HRESULT native_cpu = real->CreateDescriptorHeap(
                &hd, IID_PPV_ARGS(native_probe.GetAddressOf()));
            std::printf("Native CPU sampler heap: 0x%08lX\n",
                static_cast<unsigned long>(native_cpu));
            check(SUCCEEDED(native_cpu) && native_probe,
                  "v14 native CPU sampler heap succeeds");
            native_probe.Reset();

            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

            const HRESULT native_visible = real->CreateDescriptorHeap(
                &hd, IID_PPV_ARGS(native_probe.GetAddressOf()));
            std::printf("Native visible sampler heap: 0x%08lX\n",
                static_cast<unsigned long>(native_visible));
            check(SUCCEEDED(native_visible) && native_probe,
                  "v14 native visible sampler heap succeeds");
            native_probe.Reset();

            ComPtr<ID3D12DescriptorHeap> sampler_heap;
            using HeapFn = HRESULT(WINAPI*)(void*,
                const D3D12_DESCRIPTOR_HEAP_DESC*, const IID*, void**);
            const IID heap_iid = __uuidof(ID3D12DescriptorHeap);
            const HRESULT made = reinterpret_cast<HeapFn>(cv[14])(
                console_device.Get(), &hd, &heap_iid,
                reinterpret_cast<void**>(sampler_heap.GetAddressOf()));
            std::printf("Proxy visible sampler heap: 0x%08lX\n",
                static_cast<unsigned long>(made));
            check(SUCCEEDED(made) && sampler_heap, "v13 creates tracked sampler heap");
            if (sampler_heap) {
                auto** hv = *reinterpret_cast<void***>(sampler_heap.Get());
                const auto base = reinterpret_cast<GetCpuHandleFn>(hv[9])(sampler_heap.Get()).ptr;
                const UINT inc = console_device->GetDescriptorHandleIncrementSize(hd.Type);
                std::array<std::uint32_t, 21> synthetic{};
                synthetic[1] = synthetic[2] = synthetic[3] = 2;
                synthetic[6] = synthetic[7] = synthetic[8] = 1;
                synthetic[10] = 0x447a0000u;  // finite MaxLOD = 1000.0f
                synthetic[13] = 1;
                synthetic[16] = 1;
                check(create(console_device.Get(), synthetic.data(), base) == S_OK,
                      "v13 translates slot 61 to native CreateSampler");
                check(create(console_device.Get(), synthetic.data(), base + inc) == S_OK,
                      "v13 accepts second sampler slot");
                check(create(console_device.Get(), synthetic.data(), base + 1) == E_INVALIDARG,
                      "v13 refuses misaligned handle");
                check(create(console_device.Get(), synthetic.data(), 0) == E_INVALIDARG,
                      "v13 refuses null handle");
                check(create(console_device.Get(), nullptr, base) == E_INVALIDARG,
                      "v13 refuses null description");
                D3D12_DESCRIPTOR_HEAP_DESC other = hd;
                other.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                ComPtr<ID3D12DescriptorHeap> other_heap;
                check(SUCCEEDED(console_device->CreateDescriptorHeap(&other, IID_PPV_ARGS(other_heap.GetAddressOf()))) && other_heap,
                      "v13 creates non-sampler heap for negative control");
                if (other_heap) {
                    auto** ov = *reinterpret_cast<void***>(other_heap.Get());
                    const auto wrong = reinterpret_cast<GetCpuHandleFn>(ov[9])(other_heap.Get()).ptr;
                    check(create(console_device.Get(), synthetic.data(), wrong) == E_INVALIDARG,
                          "v13 refuses non-sampler heap destination");
                }
            }
        }
    }

    check(real->GetDeviceRemovedReason() == S_OK,
          "v14 device remains healthy after all sampler tests");

    if (v14_debug_enabled) {
        ComPtr<ID3D12InfoQueue> info;
        const HRESULT info_hr = real->QueryInterface(
            IID_PPV_ARGS(info.GetAddressOf()));

        check(SUCCEEDED(info_hr) && info,
              "v14 D3D12 debug info queue available");

        unsigned errors = 0;

        if (info) {
            const UINT64 count =
                info->GetNumStoredMessagesAllowedByRetrievalFilter();

            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T size = 0;

                if (FAILED(info->GetMessage(i, nullptr, &size))) {
                    ++errors;
                    continue;
                }

                std::vector<unsigned char> bytes(size);
                auto* msg = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());

                if (FAILED(info->GetMessage(i, msg, &size))) {
                    ++errors;
                    continue;
                }

                if (msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    ++errors;
                    std::printf("[D3D12 ERROR %u] %s\n",
                        static_cast<unsigned>(msg->ID),
                        msg->pDescription);
                }
            }
        }

        check(errors == 0, "v14 no D3D12 validation errors");
    }

    // Hold a reference to the PC device so the final check can prove the proxy's Release balances its own.
    if (real) real->AddRef();
    // Drop every device child before the proxy so the PC device is not released first.
    root_signature.Reset();
    placed_buffer.Reset();
    reserved.Reset();
    texture.Reset();
    buffer.Reset();
    allocator.Reset();
    device1.Reset();
    const unsigned long refs_before_heap_release = rt::d3d12_xs::device_proxy_ref_count(device.Get());
    heap.Reset();
    check(rt::d3d12_xs::device_proxy_ref_count(device.Get()) + 1 == refs_before_heap_release,
          "heap release drops its device reference");
    device.Reset();
    if (real) {
        check(real->GetNodeCount() > 0, "PC device survives the proxy's final Release");
        real->Release();
    }

    std::printf("device_proxy_test: %s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
