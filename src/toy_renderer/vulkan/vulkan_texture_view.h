#pragma once

#include <toy_renderer/api/api_texture_view.h>
#include <toy_renderer/handle.h>

#include <vulkan/vulkan.h>

namespace ToyRenderer {

struct Texture_t;
struct Device_t;

struct VulkanTextureView : public ApiTextureView {
    explicit VulkanTextureView(VkImageView _imageView,
                               const Handle<Texture_t> &_textureHandle,
                               const Handle<Device_t> &_deviceHandle);

    VkImageView imageView{ VK_NULL_HANDLE };
    Handle<Texture_t> textureHandle;
    Handle<Device_t> deviceHandle;
};

} // namespace ToyRenderer
