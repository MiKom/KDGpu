/*
  This file is part of KDGpu.

  SPDX-FileCopyrightText: 2023 Klarälvdalens Datakonsult AB, a KDAB Group company <info@kdab.com>

  SPDX-License-Identifier: MIT

  Contact KDAB at <info@kdab.com> for commercial licensing options.
*/

#pragma once

#include <KDGpu/api/api_instance.h>
#include <KDGpu/kdgpu_export.h>
#include <vulkan/vulkan.h>

#if defined(KDGPU_PLATFORM_WIN32)
struct VkMemoryGetWin32HandleInfoKHR;
#endif

namespace KDGpu {

class VulkanResourceManager;

/**
 * @brief VulkanInstance
 * \ingroup vulkan
 *
 */
struct KDGPU_EXPORT VulkanInstance : public ApiInstance {
    explicit VulkanInstance(VulkanResourceManager *_vulkanResourceManager, VkInstance _instance, bool _isOwned = true) noexcept;

    std::vector<Extension> extensions() const final;
    std::vector<Handle<Adapter_t>> queryAdapters(const Handle<Instance_t> &instanceHandle) final;
    Handle<Surface_t> createSurface(const SurfaceOptions &options) final;
    Handle<Surface_t> createSurface(VkSurfaceKHR surface);

    VulkanResourceManager *vulkanResourceManager{ nullptr };
    VkInstance instance{ VK_NULL_HANDLE };
    VkDebugUtilsMessengerEXT debugMessenger{ nullptr };
    bool isOwned{ true };

#if defined(KDGPU_PLATFORM_WIN32)
    using PFN_vkGetMemoryWin32HandleKHR = VkResult(VKAPI_PTR *)(VkDevice, const VkMemoryGetWin32HandleInfoKHR *, HANDLE *);
    PFN_vkGetMemoryWin32HandleKHR vkGetMemoryWin32HandleKHR{ nullptr };
#endif

#if defined(KDGPU_PLATFORM_LINUX)
    PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR{ nullptr };
#endif
};

} // namespace KDGpu
