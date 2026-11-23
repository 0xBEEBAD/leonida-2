
#pragma once

#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace v29_packed16 {

inline bool enabled(const char* name="LEONIDA_V29_PACKED16") {
    const char* value=std::getenv(name);
    return value && value[0]=='1' && value[1]=='\0';
}

struct Entry {
    VkDevice device;
    VkImage image;
    VkImageUsageFlags usage;
};

inline std::mutex mutex;
inline std::vector<Entry> images;

inline bool configuration(const VkImageCreateInfo& info) {
    constexpr VkImageCreateFlags required =
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
        VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;

    return info.format==VK_FORMAT_A1R5G5B5_UNORM_PACK16 &&
           (info.flags & required)==required &&
           (info.usage & VK_IMAGE_USAGE_STORAGE_BIT)!=0;
}

inline bool remember(VkDevice device,VkImage image,
                     const VkImageCreateInfo& info) {
    if(!configuration(info)) return true;

    try {
        std::lock_guard lock(mutex);
        images.push_back({device,image,info.usage});
    } catch(...) {
        return false;
    }

    std::fprintf(stderr,
        "[v29 IMAGE] packed16 mutable image registered\n");

    return true;
}

inline VkImageUsageFlags usage(VkDevice device,VkImage image) {
    std::lock_guard lock(mutex);

    for(const auto& entry:images)
        if(entry.device==device && entry.image==image)
            return entry.usage;

    return 0;
}

inline void forget(VkDevice device,VkImage image) {
    std::lock_guard lock(mutex);

    images.erase(std::remove_if(images.begin(),images.end(),
        [&](const Entry& e) {
            return e.device==device && e.image==image;
        }),images.end());
}

template<class Fn>
VkResult create_view(Fn fn,VkDevice device,
                     const VkImageViewCreateInfo* input,
                     const VkAllocationCallbacks* allocator,
                     VkImageView* output) {

    if(!input || !enabled())
        return fn(device,input,allocator,output);

    const auto available=usage(device,input->image);

    if(!available)
        return fn(device,input,allocator,output);

    VkImageViewCreateInfo view=*input;

    VkImageViewUsageCreateInfo restriction{
        VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};

    bool has_restriction=false;

    for(auto* node=static_cast<const VkBaseInStructure*>(view.pNext);
        node;node=node->pNext) {
        if(node->sType==VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO) {
            has_restriction=true;
            break;
        }
    }

    if(!has_restriction) {
        if(view.format==VK_FORMAT_A1R5G5B5_UNORM_PACK16) {
            restriction.usage=available &
                (VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        } else if(view.format==VK_FORMAT_R16_UINT) {
            restriction.usage=available &
                (VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_STORAGE_BIT);
        } else {
            return fn(device,input,allocator,output);
        }

        if(!restriction.usage)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;

        restriction.pNext=view.pNext;
        view.pNext=&restriction;
    }

    const auto result=fn(device,&view,allocator,output);

    std::fprintf(stderr,
        "[v29 VIEW] format=%u result=%d\n",
        unsigned(view.format),int(result));

    return result;
}

template<class Fn>
void destroy_image(Fn fn,VkDevice device,VkImage image,
                   const VkAllocationCallbacks* allocator) {
    forget(device,image);
    fn(device,image,allocator);
}

} // namespace v29_packed16
