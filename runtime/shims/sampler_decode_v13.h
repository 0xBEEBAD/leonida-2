#pragma once
// Portable translation of the separately documented console sampler layout.
// The 0x54-byte layout and field interpretation are an ABI hypothesis described
// in public Leonida PR #8134. This adapter does not establish hardware parity.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace rt::d3d12_xs::v13 {
inline constexpr std::size_t sampler_bytes = 0x54;
struct Sampler {
    std::uint32_t filter = 0, u = 1, v = 1, w = 1, anisotropy = 1, comparison = 1;
    float bias = 0, min_lod = 0, max_lod = (std::numeric_limits<float>::max)();
    std::array<float, 4> border{};
    bool normalized = false;
};
inline bool decode(std::span<const std::uint8_t> bytes, Sampler& out) {
    if (bytes.size() != sampler_bytes) return false;
    static_assert(sizeof(float) == 4);
    Sampler result{};
    auto word = [&](std::size_t at) {
        // Explicit little-endian input. No alignment assumption.
        return std::uint32_t(bytes[at]) | (std::uint32_t(bytes[at+1]) << 8) |
               (std::uint32_t(bytes[at+2]) << 16) | (std::uint32_t(bytes[at+3]) << 24);
    };
    auto fp = [&](std::size_t at) {
        auto bits = word(at); float value;
        std::memcpy(&value, &bits, sizeof(value)); return value;
    };
    auto linear = [&](std::uint32_t field) {
        if (field > 2) result.normalized = true;
        return field >= 2 ? 1u : 0u;
    };
    auto mode = [&](std::uint32_t field) {
        if (field >= 1 && field <= 5) return field;
        result.normalized = true;
        return 1u;
    };
    auto finite = [&](float value, float fallback) {
        if (std::isfinite(value)) return value;
        result.normalized = true;
        return fallback;
    };
    const auto red = word(0x14);
    const auto reduction = red <= 3 ? red : 0u;
    result.normalized |= red > 3;
    const auto aniso_word = word(0x34);
    result.anisotropy = std::clamp(aniso_word, 1u, 16u);
    result.normalized |= aniso_word != result.anisotropy;
    const auto min_filter = linear(word(0x04));
    const auto mag_filter = linear(word(0x08));
    const auto mip_filter = linear(word(0x0c));
    // D3D12 basic filter bit encoding; anisotropic requires all linear bits.
    result.filter = (reduction << 7) | (result.anisotropy > 1 ? 0x55u :
                    ((min_filter << 4) | (mag_filter << 2) | mip_filter));
    result.u = mode(word(0x18)); result.v = mode(word(0x1c)); result.w = mode(word(0x20));
    const auto comparison = word(0x40);
    result.comparison = comparison >= 1 && comparison <= 8 ? comparison : 1u;
    result.normalized |= comparison != result.comparison;
    const auto bias = finite(fp(0x30), 0.0f);
    result.bias = std::clamp(bias, -16.0f, 15.99f);
    result.normalized |= bias != result.bias;
    result.min_lod = finite(fp(0x24), 0.0f);
    result.max_lod = finite(fp(0x28), (std::numeric_limits<float>::max)());
    if (result.max_lod < result.min_lod) {
        result.max_lod = result.min_lod;
        result.normalized = true;
    }
    for (std::size_t i=0; i<result.border.size(); ++i)
        result.border[i] = finite(fp(0x44 + 4*i), 0.0f);
    out = result;
    return true;
}
} // namespace rt::d3d12_xs::v13
