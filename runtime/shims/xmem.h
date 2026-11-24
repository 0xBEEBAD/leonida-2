#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rt::xmem {

bool commit_physical_range(const void* address, std::size_t size);
// A lifetime-pinned alias of a contiguous range of the title's writable mapped physical pages.
std::shared_ptr<std::uint8_t> pin_mapped_data(const void* address, std::size_t size);
bool commit_physical_pages(const std::uint64_t* page_array, std::size_t count);

}
