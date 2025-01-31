/*
  This file is part of KDGpu.

  SPDX-FileCopyrightText: 2023 Klarälvdalens Datakonsult AB, a KDAB Group company <info@kdab.com>

  SPDX-License-Identifier: MIT

  Contact KDAB at <info@kdab.com> for commercial licensing options.
*/

#pragma once

#include <KDGpu/kdgpu_export.h>

#include <vulkan/vulkan.h>

#include <array>
#include <vector>

namespace KDGpu {

//
// Instance Config
//

#if defined(NDEBUG) || defined(__arm__)
constexpr bool enableValidationLayers = false;
const std::vector<const char *> requestedInstanceLayers = {
#if defined(PLATFORM_MACOS)
    "VK_LAYER_KHRONOS_synchronization2"
#endif
};
#else
constexpr bool enableValidationLayers = true;
const std::vector<const char *> requestedInstanceLayers = {
    "VK_LAYER_KHRONOS_validation",
#if defined(PLATFORM_MACOS)
    "VK_LAYER_KHRONOS_synchronization2"
#endif
};
#endif

constexpr std::array<const char *, 1> requestedInstanceExtensions = {
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME
};

std::vector<const char *> KDGPU_EXPORT getDefaultRequestedInstanceExtensions();

//
// Device Config
//
std::vector<const char *> KDGPU_EXPORT getDefaultRequestedDeviceExtensions();

} // namespace KDGpu
