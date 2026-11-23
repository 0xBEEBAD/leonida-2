// Xbox slot 76, a compute dispatch through the Vulkan backend, and separate depth/stencil readbacks.
// A synthetic compute shader looks up the T#s that the device writes. No game data.
#include "d3d12_copy_compare.h"
#include "d3d12_test_shaders.h"
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "vk_test_device.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/shader_library.h"

#include <cstdio>
#include <cstring>
#include <fstream>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {
using d3d12test::Ref;

void enhanced_transition(ID3D12GraphicsCommandList7* list, ID3D12Resource* texture,
                         D3D12_BARRIER_SYNC before_sync, D3D12_BARRIER_ACCESS before_access,
                         D3D12_BARRIER_LAYOUT before_layout, D3D12_BARRIER_SYNC after_sync,
                         D3D12_BARRIER_ACCESS after_access, D3D12_BARRIER_LAYOUT after_layout) {
    D3D12_TEXTURE_BARRIER barrier{};
    barrier.SyncBefore = before_sync;
    barrier.AccessBefore = before_access;
    barrier.LayoutBefore = before_layout;
    barrier.SyncAfter = after_sync;
    barrier.AccessAfter = after_access;
    barrier.LayoutAfter = after_layout;
    barrier.Subresources = {D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, 0, 0, 0, 0, 0};
    barrier.pResource = texture;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &barrier;
    list->Barrier(1, &group);
}

int run(vktest::Device& vk, UINT width, UINT height, bool enhanced) {
    const auto program = d3d12test::fssm_program({0x44535541, 0xbf810000});
    const auto shader = d3d12test::shader_container(program);
    const testfiles::ScratchDir library("leonida_depth_stencil_uav");
    CHECK(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_DEPTH_UAV_SPV), library.path() / "write.spv"));
    {
        std::ofstream index(library.path() / "index.json", std::ios::binary);
        index << "{\"write\":{\"stage\":\"cs\",\"fssm_sha256\":\"" << leonida::sha256_hex(program)
              << "\",\"bindings\":[2,8,9],\"push_constant_bytes\":80,\"local_size\":[1,1,1],\"wave64\":false}}";
        CHECK(index.good());
    }

    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = vk.instance_proc;
    info.instance = vk.instance;
    info.physical = vk.physical;
    info.device = vk.device;
    info.queue = vk.queue;
    info.queue_family = vk.family;
    info.bindless = vk.bindless;
    info.bindless_partially_bound = vk.partially_bound;
    info.buffer_device_address = vk.buffer_device_address;
    info.shader_dir = library.path().string();
    ID3D12Device* device = nullptr;
    CHECK(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device))));
    Ref<ID3D12Device> xbox;
    CHECK(SUCCEEDED(rt::d3d12_xs::create_device_proxy(device, IID_PPV_ARGS(&xbox.p)))); // takes ownership
    rt::d3d12::set_device_outer(device, xbox.p);

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.DepthOrArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    using CreateFn = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                      const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                                      REFIID, void**);
    // The Xbox slot-25 sizing path from #7110/#7679 is covered on the Forge integration branch.
    // Use the inner PC device's native depth description for allocation size and readback footprints.
    auto native_depth = desc;
    native_depth.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    native_depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const auto size = device->GetResourceAllocationInfo(0, 1, &native_depth);
    CHECK(size.SizeInBytes > 0 && size.SizeInBytes != UINT64_MAX);
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    Ref<ID3D12Resource> texture;
    const auto create_texture = reinterpret_cast<CreateFn>(d3d12test::slot(xbox.p, 76));
    CHECK(SUCCEEDED(create_texture(xbox.p, &props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                   nullptr, IID_PPV_ARGS(&texture.p))));
    CHECK(texture->GetDesc().Flags == desc.Flags);

    const D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    const D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    Ref<ID3D12DescriptorHeap> dsv, views;
    CHECK(SUCCEEDED(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv.p))));
    CHECK(SUCCEEDED(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views.p))));
    D3D12_DEPTH_STENCIL_VIEW_DESC depth_view{};
    depth_view.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    depth_view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    const auto depth_handle = dsv->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(texture.p, &depth_view, depth_handle);
    const SIZE_T base = views->GetCPUDescriptorHandleForHeapStart().ptr;
    const SIZE_T step = device->GetDescriptorHandleIncrementSize(views_desc.Type);
    CHECK(base && step >= 32);
    std::uint32_t keys[2][8]{};
    for (UINT plane = 0; plane < 2; ++plane) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Format = plane == 0 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8_UINT;
        view.Texture2D.PlaneSlice = plane;
        const D3D12_CPU_DESCRIPTOR_HANDLE handle{base + plane * step};
        device->CreateUnorderedAccessView(texture.p, nullptr, &view, handle);
        std::memcpy(keys[plane], reinterpret_cast<const void*>(handle.ptr), sizeof(keys[plane]));
        CHECK(rt::d3d12::bindless_hash_lookup(device, keys[plane]) != 0);
    }
    CHECK(std::memcmp(keys[0], keys[1], sizeof(keys[0])) != 0);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    const D3D12_CPU_DESCRIPTOR_HANDLE sampled{base + 2 * step};
    device->CreateShaderResourceView(texture.p, &srv, sampled);
    std::uint32_t sampled_key[8]{};
    std::memcpy(sampled_key, reinterpret_cast<const void*>(sampled.ptr), sizeof(sampled_key));
    CHECK(rt::d3d12::bindless_hash_lookup(device, sampled_key) != 0);
    CHECK(std::memcmp(sampled_key, keys[0], sizeof(sampled_key)) != 0);
    const D3D12_CPU_DESCRIPTOR_HANDLE invalid{base + 3 * step};
    std::uint32_t zero[8]{};
    for (UINT plane = 0; plane < 2; ++plane) {
        std::memset(reinterpret_cast<void*>(invalid.ptr), 0x55, sizeof(zero));
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Texture2D.PlaneSlice = plane;
        view.Format = plane == 0 ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R32_FLOAT;
        device->CreateUnorderedAccessView(texture.p, nullptr, &view, invalid);
        CHECK(std::memcmp(reinterpret_cast<const void*>(invalid.ptr), zero, sizeof(zero)) == 0);
    }

    // ClearUAV takes matching handles to two copies of the descriptor and the original Xbox resource.
    const D3D12_DESCRIPTOR_HEAP_DESC visible_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> visible;
    CHECK(SUCCEEDED(device->CreateDescriptorHeap(&visible_desc, IID_PPV_ARGS(&visible.p))));
    device->CopyDescriptorsSimple(2, visible->GetCPUDescriptorHandleForHeapStart(), {base}, visible_desc.Type);
    const auto gpu = visible->GetGPUDescriptorHandleForHeapStart();
    const FLOAT depth_clear[] = {0.75f, 0, 0, 0};
    const UINT stencil_clear[] = {0x31, 0, 0, 0};
    {
        // Both parents have a real R32_FLOAT storage plane, but this T# belongs to the first parent only.
        auto other_desc = desc;
        other_desc.Width = other_desc.Height = 2;
        Ref<ID3D12Resource> other;
        CHECK(SUCCEEDED(create_texture(xbox.p, &props, D3D12_HEAP_FLAG_NONE, &other_desc,
                                       D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&other.p))));
        copycmp::Run rejected;
        CHECK(rejected.init(device, true));
        rejected.begin();
        rejected.barrier(other.p, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[] = {visible.p};
        rejected.list->SetDescriptorHeaps(1, heaps);
        rejected.list->ClearUnorderedAccessViewFloat(gpu, {base}, other.p, depth_clear, 0, nullptr);
        CHECK(rejected.list->Close() == E_INVALIDARG);
    }

    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.Num32BitValues = 16;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    rs.Desc_1_0 = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    const auto root_blob = d3d12test::serialize_root_signature(rs);
    Ref<ID3D12RootSignature> root;
    CHECK(!root_blob.empty());
    CHECK(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.p;
    pipeline_desc.CS = {shader.data(), shader.size()};
    Ref<ID3D12PipelineState> pipeline;
    CHECK(SUCCEEDED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline.p))));

    copycmp::Run commands;
    CHECK(commands.init(device, true));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[2]{};
    UINT64 row_bytes[2]{}, total = 0;
    device->GetCopyableFootprints(&native_depth, 0, 2, 0, layouts, nullptr, row_bytes, &total);
    CHECK(row_bytes[0] == width * sizeof(float) && row_bytes[1] == width);
    copycmp::Ptr<ID3D12Resource> readback;
    CHECK(commands.buffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST, readback));

    commands.begin();
    commands.list->ClearDepthStencilView(depth_handle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                          0.5f, 0x5a, 0, nullptr);
    Ref<ID3D12GraphicsCommandList7> enhanced_list;
    if (enhanced) {
        CHECK(SUCCEEDED(commands.list->QueryInterface(IID_PPV_ARGS(&enhanced_list.p))));
        enhanced_transition(enhanced_list.p, texture.p, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                            D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    } else {
        commands.barrier(texture.p, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    const D3D12_RECT depth_rect{1, 3, 2, 4}, stencil_rect{3, 2, 4, 3};
    ID3D12DescriptorHeap* clear_heaps[] = {visible.p};
    commands.list->SetDescriptorHeaps(1, clear_heaps);
    commands.list->ClearUnorderedAccessViewFloat(gpu, {base}, texture.p, depth_clear, 1, &depth_rect);
    commands.list->ClearUnorderedAccessViewUint({gpu.ptr + step}, {base + step}, texture.p, stencil_clear, 1,
                                               &stencil_rect);
    D3D12_RESOURCE_BARRIER uav_order{};
    uav_order.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_order.UAV.pResource = texture.p;
    commands.list->ResourceBarrier(1, &uav_order);
    commands.list->SetComputeRootSignature(root.p);
    commands.list->SetPipelineState(pipeline.p);
    commands.list->SetComputeRoot32BitConstants(0, 16, keys, 0);
    commands.list->Dispatch(1, 1, 1);
    if (enhanced) {
        enhanced_transition(enhanced_list.p, texture.p, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                            D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COPY_SOURCE);
    } else {
        commands.barrier(texture.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    for (UINT plane = 0; plane < 2; ++plane) {
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = texture.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = plane;
        dst.pResource = readback.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = layouts[plane];
        commands.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    const auto error = commands.submit();
    if (!error.empty()) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }

    unsigned char* mapped = nullptr;
    CHECK(SUCCEEDED(readback.p->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            float depth = 0.0f;
            std::memcpy(&depth, mapped + layouts[0].Offset + y * layouts[0].Footprint.RowPitch + x * sizeof(float),
                        sizeof(depth));
            const auto stencil = mapped[layouts[1].Offset + y * layouts[1].Footprint.RowPitch + x];
            float expected_depth = x == 2 && y == 1 ? 0.125f : 0.5f;
            unsigned expected_stencil = x == 5 && y == 3 ? 0xa5 : 0x5a;
            if (x == UINT(depth_rect.left) && y == UINT(depth_rect.top)) expected_depth = depth_clear[0];
            if (x == UINT(stencil_rect.left) && y == UINT(stencil_rect.top)) expected_stencil = stencil_clear[0];
            if (depth != expected_depth || stencil != expected_stencil) {
                std::fprintf(stderr, "(%u,%u): depth %g, stencil %#x\n", x, y, depth, unsigned(stencil));
                readback.p->Unmap(0, nullptr);
                return 1;
            }
        }
    }
    readback.p->Unmap(0, nullptr);
    std::printf("depth/stencil UAV: %s barriers, %ux%u, read back all %u pixels on each plane\n",
                enhanced ? "enhanced" : "legacy", width, height, width * height);
    return 0;
}
} // namespace

int main() {
    vktest::Device vk;
    vk.request_buffer_device_address = true;
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;
    if (!vk.bindless || !vk.storage_image_extended_formats) {
        std::fprintf(stderr, "SKIP: shader requires bindless images and typed r8ui storage\n");
        return 77;
    }
    if (run(vk, 9, 7, false) || run(vk, 2560, 1440, true)) return 1;
    return vk.validation_errors() ? 1 : 0;
}
