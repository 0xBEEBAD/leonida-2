#pragma once
#include <cstdint>
#include <limits>

// Pure checked range calculation for an address-based buffer SRV. An address
// inside a resource may have a further FirstElement offset. Reject overflow,
// misalignment and any view extending beyond the registered resource.
namespace rt::d3d12_xs::v10 {
inline bool buffer_view_range(std::uint64_t base, std::uint64_t bytes,
                              std::uint64_t address, std::uint32_t element,
                              std::uint64_t first, std::uint32_t count,
                              std::uint64_t& translated_first) noexcept {
    translated_first = 0;
    if (!element || !count || !bytes || address < base || address - base >= bytes) return false;
    const auto offset = address - base;
    if (offset % element) return false;
    const auto start = offset / element;
    const auto total = bytes / element;
    if (first > total - start) return false;
    const auto translated = start + first;
    if (count > total - translated) return false;
    translated_first = translated;
    return true;
}
}
