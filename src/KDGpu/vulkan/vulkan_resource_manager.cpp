/*
  This file is part of KDGpu.

  SPDX-FileCopyrightText: 2023 Klarälvdalens Datakonsult AB, a KDAB Group company <info@kdab.com>

  SPDX-License-Identifier: MIT

  Contact KDAB at <info@kdab.com> for commercial licensing options.
*/

#include "vulkan_resource_manager.h"

#if defined(KDGPU_PLATFORM_WIN32)
// Avoid having to define VK_USE_PLATFORM_WIN32_KHR which would result in windows.h being included when vulkan.h is included
#include <vulkan/vulkan_win32.h>
#endif

#include <KDGpu/bind_group_options.h>
#include <KDGpu/bind_group_layout_options.h>
#include <KDGpu/buffer_options.h>
#include <KDGpu/compute_pipeline_options.h>
#include <KDGpu/graphics_pipeline_options.h>
#include <KDGpu/instance.h>
#include <KDGpu/sampler_options.h>
#include <KDGpu/swapchain_options.h>
#include <KDGpu/texture_options.h>
#include <KDGpu/vulkan/vulkan_config.h>
#include <KDGpu/vulkan/vulkan_enums.h>
#include <KDGpu/vulkan/vulkan_formatters.h>

#include <cassert>
#include <stdexcept>

namespace {

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
        void *pUserData)
{
    // The validation layers do not cache the queried swapchain extent range and so
    // can race on X11 when resizing rapidly. See
    //
    // https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/1340
    //
    // Ignore this false positive.
    const char *ignore = "VUID-VkSwapchainCreateInfoKHR-imageExtent-01274";
    if (pCallbackData->pMessageIdName != nullptr && strcmp(ignore, pCallbackData->pMessageIdName) == 0)
        return false;

    switch (messageSeverity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
        SPDLOG_LOGGER_DEBUG(KDGpu::Logger::logger(), "validation layer: {}", pCallbackData->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
        SPDLOG_LOGGER_INFO(KDGpu::Logger::logger(), "validation layer: {}", pCallbackData->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        SPDLOG_LOGGER_WARN(KDGpu::Logger::logger(), "validation layer: {}", pCallbackData->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        SPDLOG_LOGGER_ERROR(KDGpu::Logger::logger(), "validation layer: {}", pCallbackData->pMessage);
        break;
    default:
        SPDLOG_LOGGER_TRACE(KDGpu::Logger::logger(), "validation layer: {}", pCallbackData->pMessage);
        break;
    }

    return VK_FALSE;
}

} // namespace
namespace KDGpu {

VulkanResourceManager::VulkanResourceManager()
{
}

VulkanResourceManager::~VulkanResourceManager()
{
}

Handle<Instance_t> VulkanResourceManager::createInstance(const InstanceOptions &options)
{
    // Populate some basic application and engine info
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = options.applicationName.data();
    appInfo.applicationVersion = options.applicationVersion;
    appInfo.pEngineName = "KDGpu";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    // On macOS we need to enable the VK_KHR_PORTABILITY_subset instance extension so that
    // the MoltenVK driver is allowed to be used even though it is technically non-conformant
    // at present. Also see vulkan_config.h. For more detail see the
    // Encountered VK_ERROR_INCOMPATIBLE_DRIVER section of
    // https://vulkan.lunarg.com/doc/sdk/1.3.216.0/mac/getting_started.html
#if defined(PLATFORM_MACOS)
    createInfo.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    std::vector<const char *> layers = requestedInstanceLayers;
    for (const std::string &userLayer : options.layers)
        layers.push_back(userLayer.c_str());

    if (!layers.empty()) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
        assert(requestedInstanceLayers.size() <= std::numeric_limits<uint32_t>::max());
        createInfo.ppEnabledLayerNames = layers.data();
    }

    std::vector<const char *> requestedInstanceExtensions;

    // Query the available instance extensions
    const auto availableExtensions = getInstanceExtensions();

    const auto findExtension = [&extensions = availableExtensions](const std::string_view &name) {
        const auto it = std::find_if(begin(extensions), end(extensions), [name](const Extension &ext) { return ext.name == name; });
        return it != std::end(extensions);
    };

    const auto defaultRequestedExtensions = getDefaultRequestedInstanceExtensions();
    for (const char *requestedExtension : defaultRequestedExtensions) {
        if (findExtension(requestedExtension)) {
            requestedInstanceExtensions.push_back(requestedExtension);
        } else {
            SPDLOG_LOGGER_WARN(Logger::logger(), "Unable to find default requested extension {}", requestedExtension);
        }
    }

    for (const std::string &userExtension : options.extensions) {
        if (findExtension(userExtension)) {
            requestedInstanceExtensions.push_back(userExtension.c_str());
        } else {
            SPDLOG_LOGGER_WARN(Logger::logger(), "Unable to find user requested extensions {}", userExtension);
        }
    }

    if (!requestedInstanceExtensions.empty()) {
        createInfo.enabledExtensionCount = static_cast<uint32_t>(requestedInstanceExtensions.size());
        assert(requestedInstanceExtensions.size() <= std::numeric_limits<uint32_t>::max());
        createInfo.ppEnabledExtensionNames = requestedInstanceExtensions.data();
    }

    // Try to create the instance
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string{ "Failed to create Vulkan instance: " } + getResultAsString(result));
    }

    VulkanInstance vulkanInstance(this, instance);

    // Validation Debug Logger
    {
        VkDebugUtilsMessengerCreateInfoEXT createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;
        createInfo.pUserData = nullptr; // Optional

        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(vulkanInstance.instance, "vkCreateDebugUtilsMessengerEXT");
        if (func != nullptr) {
            if (func(vulkanInstance.instance, &createInfo, nullptr, &vulkanInstance.debugMessenger) != VK_SUCCESS)
                vulkanInstance.debugMessenger = nullptr;
        }
    }

    auto h = m_instances.emplace(vulkanInstance);
    return h;
}

Handle<Instance_t> VulkanResourceManager::createInstanceFromExistingVkInstance(VkInstance vkInstance)
{
    VulkanInstance vulkanInstance(this, vkInstance, false);
    auto h = m_instances.emplace(vulkanInstance);
    return h;
}

void VulkanResourceManager::deleteInstance(const Handle<Instance_t> &handle)
{
    VulkanInstance *instance = m_instances.get(handle);

    // Only destroy instances that we have allocated
    if (instance->isOwned) {

        // Destroy debug logger if we have one
        if (instance->debugMessenger) {
            auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance->instance, "vkDestroyDebugUtilsMessengerEXT");
            if (func != nullptr)
                func(instance->instance, instance->debugMessenger, nullptr);
        }

        vkDestroyInstance(instance->instance, nullptr);
    }

    m_instances.remove(handle);
}

VulkanInstance *VulkanResourceManager::getInstance(const Handle<Instance_t> &handle) const
{
    return m_instances.get(handle);
}

std::vector<Extension> VulkanResourceManager::getInstanceExtensions() const
{
    uint32_t extensionCount{ 0 };
    if (auto result = vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr); result != VK_SUCCESS) {
        SPDLOG_LOGGER_CRITICAL(Logger::logger(), "Unable to enumerate instance extensions: {}", result);
        return {};
    }

    std::vector<VkExtensionProperties> vkExtensions(extensionCount);
    if (auto result = vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, vkExtensions.data()); result != VK_SUCCESS) {
        SPDLOG_LOGGER_CRITICAL(Logger::logger(), "Unable to query instance extensions: {}", result);
        return {};
    }

    std::vector<Extension> extensions;
    extensions.reserve(extensionCount);
    for (const auto &vkExtension : vkExtensions) {
        extensions.emplace_back(Extension{
                .name = vkExtension.extensionName,
                .version = vkExtension.specVersion });
    }

    return extensions;
}

Handle<Adapter_t> VulkanResourceManager::insertAdapter(const VulkanAdapter &physicalDevice)
{
    return m_adapters.emplace(physicalDevice);
}

void VulkanResourceManager::removeAdapter(const Handle<Adapter_t> &handle)
{
    m_adapters.remove(handle);
}

VulkanAdapter *VulkanResourceManager::getAdapter(const Handle<Adapter_t> &handle) const
{
    return m_adapters.get(handle);
}

/*
 * Create a VkDevice (logical device) from the provided adapter (physical device) and requested options.
 * If no options are specified we request a single queue from the first family (usually graphics capable).
 */
Handle<Device_t> VulkanResourceManager::createDevice(const Handle<Adapter_t> &adapterHandle, const DeviceOptions &options, std::vector<QueueRequest> &queueRequests)
{
    VulkanAdapter *vulkanAdapter = getAdapter(adapterHandle);

    queueRequests = options.queues;
    if (queueRequests.empty()) {
        QueueRequest queueRequest = {
            .queueTypeIndex = 0,
            .count = 1,
            .priorities = { 1.0f }
        };
        queueRequests.emplace_back(queueRequest);
    }

    uint32_t queueRequestCount = queueRequests.size();
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(queueRequestCount);
    for (uint32_t i = 0; i < queueRequestCount; ++i) {
        const auto &queueRequest = queueRequests[i];

        VkDeviceQueueCreateInfo queueCreateInfo = {};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueRequest.queueTypeIndex;
        queueCreateInfo.queueCount = queueRequest.count;
        queueCreateInfo.pQueuePriorities = queueRequest.priorities.data();

        queueCreateInfos.push_back(queueCreateInfo);
    }

    // Request the physical device features requested by options
    VkPhysicalDeviceFeatures deviceFeatures = {};
    {
        deviceFeatures.robustBufferAccess = options.requestedFeatures.robustBufferAccess;
        deviceFeatures.fullDrawIndexUint32 = options.requestedFeatures.fullDrawIndexUint32;
        deviceFeatures.imageCubeArray = options.requestedFeatures.imageCubeArray;
        deviceFeatures.independentBlend = options.requestedFeatures.independentBlend;
        deviceFeatures.geometryShader = options.requestedFeatures.geometryShader;
        deviceFeatures.tessellationShader = options.requestedFeatures.tessellationShader;
        deviceFeatures.sampleRateShading = options.requestedFeatures.sampleRateShading;
        deviceFeatures.dualSrcBlend = options.requestedFeatures.dualSrcBlend;
        deviceFeatures.logicOp = options.requestedFeatures.logicOp;
        deviceFeatures.multiDrawIndirect = options.requestedFeatures.multiDrawIndirect;
        deviceFeatures.drawIndirectFirstInstance = options.requestedFeatures.drawIndirectFirstInstance;
        deviceFeatures.depthClamp = options.requestedFeatures.depthClamp;
        deviceFeatures.depthBiasClamp = options.requestedFeatures.depthBiasClamp;
        deviceFeatures.fillModeNonSolid = options.requestedFeatures.fillModeNonSolid;
        deviceFeatures.depthBounds = options.requestedFeatures.depthBounds;
        deviceFeatures.wideLines = options.requestedFeatures.wideLines;
        deviceFeatures.largePoints = options.requestedFeatures.largePoints;
        deviceFeatures.alphaToOne = options.requestedFeatures.alphaToOne;
        deviceFeatures.multiViewport = options.requestedFeatures.multiViewport;
        deviceFeatures.samplerAnisotropy = options.requestedFeatures.samplerAnisotropy;
        deviceFeatures.textureCompressionETC2 = options.requestedFeatures.textureCompressionETC2;
        deviceFeatures.textureCompressionASTC_LDR = options.requestedFeatures.textureCompressionASTC_LDR;
        deviceFeatures.textureCompressionBC = options.requestedFeatures.textureCompressionBC;
        deviceFeatures.occlusionQueryPrecise = options.requestedFeatures.occlusionQueryPrecise;
        deviceFeatures.pipelineStatisticsQuery = options.requestedFeatures.pipelineStatisticsQuery;
        deviceFeatures.vertexPipelineStoresAndAtomics = options.requestedFeatures.vertexPipelineStoresAndAtomics;
        deviceFeatures.fragmentStoresAndAtomics = options.requestedFeatures.fragmentStoresAndAtomics;
        deviceFeatures.shaderTessellationAndGeometryPointSize = options.requestedFeatures.shaderTessellationAndGeometryPointSize;
        deviceFeatures.shaderImageGatherExtended = options.requestedFeatures.shaderImageGatherExtended;
        deviceFeatures.shaderStorageImageExtendedFormats = options.requestedFeatures.shaderStorageImageExtendedFormats;
        deviceFeatures.shaderStorageImageMultisample = options.requestedFeatures.shaderStorageImageMultisample;
        deviceFeatures.shaderStorageImageReadWithoutFormat = options.requestedFeatures.shaderStorageImageReadWithoutFormat;
        deviceFeatures.shaderStorageImageWriteWithoutFormat = options.requestedFeatures.shaderStorageImageWriteWithoutFormat;
        deviceFeatures.shaderUniformBufferArrayDynamicIndexing = options.requestedFeatures.shaderUniformBufferArrayDynamicIndexing;
        deviceFeatures.shaderSampledImageArrayDynamicIndexing = options.requestedFeatures.shaderSampledImageArrayDynamicIndexing;
        deviceFeatures.shaderStorageBufferArrayDynamicIndexing = options.requestedFeatures.shaderStorageBufferArrayDynamicIndexing;
        deviceFeatures.shaderStorageImageArrayDynamicIndexing = options.requestedFeatures.shaderStorageImageArrayDynamicIndexing;
        deviceFeatures.shaderClipDistance = options.requestedFeatures.shaderClipDistance;
        deviceFeatures.shaderCullDistance = options.requestedFeatures.shaderCullDistance;
        deviceFeatures.shaderFloat64 = options.requestedFeatures.shaderFloat64;
        deviceFeatures.shaderInt64 = options.requestedFeatures.shaderInt64;
        deviceFeatures.shaderInt16 = options.requestedFeatures.shaderInt16;
        deviceFeatures.shaderResourceResidency = options.requestedFeatures.shaderResourceResidency;
        deviceFeatures.shaderResourceMinLod = options.requestedFeatures.shaderResourceMinLod;
        deviceFeatures.sparseBinding = options.requestedFeatures.sparseBinding;
        deviceFeatures.sparseResidencyBuffer = options.requestedFeatures.sparseResidencyBuffer;
        deviceFeatures.sparseResidencyImage2D = options.requestedFeatures.sparseResidencyImage2D;
        deviceFeatures.sparseResidencyImage3D = options.requestedFeatures.sparseResidencyImage3D;
        deviceFeatures.sparseResidency2Samples = options.requestedFeatures.sparseResidency2Samples;
        deviceFeatures.sparseResidency4Samples = options.requestedFeatures.sparseResidency4Samples;
        deviceFeatures.sparseResidency8Samples = options.requestedFeatures.sparseResidency8Samples;
        deviceFeatures.sparseResidency16Samples = options.requestedFeatures.sparseResidency16Samples;
        deviceFeatures.sparseResidencyAliased = options.requestedFeatures.sparseResidencyAliased;
        deviceFeatures.variableMultisampleRate = options.requestedFeatures.variableMultisampleRate;
        deviceFeatures.inheritedQueries = options.requestedFeatures.inheritedQueries;
    }

    // Some newer features we have to request via VkPhysicalDeviceFeatures2
    VkPhysicalDeviceFeatures2 physicalDeviceFeatures2 = {};
    physicalDeviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    physicalDeviceFeatures2.features = deviceFeatures;

    // Allows to use std430 for uniform buffers which gives much nicer packing of data
    VkPhysicalDeviceUniformBufferStandardLayoutFeatures stdLayoutFeatures = {};
    stdLayoutFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES;
    stdLayoutFeatures.uniformBufferStandardLayout = options.requestedFeatures.uniformBufferStandardLayout;
    physicalDeviceFeatures2.pNext = &stdLayoutFeatures;

    // Enable multiview rendering if requested
    VkPhysicalDeviceMultiviewFeatures multiViewFeatures{};
    multiViewFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
    multiViewFeatures.multiview = options.requestedFeatures.multiView;
    multiViewFeatures.multiviewGeometryShader = options.requestedFeatures.multiViewGeometryShader;
    multiViewFeatures.multiviewTessellationShader = options.requestedFeatures.multiViewTessellationShader;
    stdLayoutFeatures.pNext = &multiViewFeatures;

#if defined(VK_KHR_synchronization2)
    // Enable the VK_KHR_Synchronization2 extension features by chaining this into the createInfo chain.
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2Features = {};
    sync2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;
    sync2Features.synchronization2 = vulkanAdapter->supportsSynchronization2;
    multiViewFeatures.pNext = &sync2Features;
#endif

    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &physicalDeviceFeatures2;
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pEnabledFeatures = nullptr; // we use VkPhysicalDeviceFeatures2 set on pNext
    createInfo.enabledLayerCount = 0;
    createInfo.ppEnabledLayerNames = nullptr;
    createInfo.enabledExtensionCount = 0;
    createInfo.ppEnabledExtensionNames = nullptr;

    // TODO: Obey requested adapter features (e.g. geometry shaders)
    // TODO: Merge requested device extensions and layers with our defaults
    const auto requestedDeviceExtensions = getDefaultRequestedDeviceExtensions();
    if (!requestedDeviceExtensions.empty()) {
        createInfo.enabledExtensionCount = static_cast<uint32_t>(requestedDeviceExtensions.size());
        assert(requestedDeviceExtensions.size() <= std::numeric_limits<uint32_t>::max());
        createInfo.ppEnabledExtensionNames = requestedDeviceExtensions.data();
    }

    VkDevice vkDevice{ VK_NULL_HANDLE };
    VkResult result = vkCreateDevice(vulkanAdapter->physicalDevice, &createInfo, nullptr, &vkDevice);
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string{ "Failed to create a logical device: " } + getResultAsString(result));

    const auto deviceHandle = m_devices.emplace(vkDevice, this, adapterHandle);

    return deviceHandle;
}

Handle<Device_t> VulkanResourceManager::createDeviceFromExistingVkDevice(const Handle<Adapter_t> &adapterHandle, VkDevice vkDevice)
{
    const auto deviceHandle = m_devices.emplace(vkDevice, this, adapterHandle, false);

    return deviceHandle;
}

void VulkanResourceManager::deleteDevice(const Handle<Device_t> &handle)
{
    VulkanDevice *vulkanDevice = m_devices.get(handle);

    // Destroy Render Passes
    for (const auto &[passKey, passHandle] : vulkanDevice->renderPasses) {
        VulkanRenderPass *pass = m_renderPasses.get(passHandle);
        vkDestroyRenderPass(vulkanDevice->device, pass->renderPass, nullptr);
        m_renderPasses.remove(passHandle);
    }

    // Destroy FrameBuffers
    for (const auto &[fbKey, fbHandle] : vulkanDevice->framebuffers) {
        VulkanFramebuffer *fb = m_framebuffers.get(fbHandle);
        vkDestroyFramebuffer(vulkanDevice->device, fb->framebuffer, nullptr);
        m_framebuffers.remove(fbHandle);
    }

    // Destroy Descriptor Pools
    for (VkDescriptorPool descriptorPool : vulkanDevice->descriptorSetPools)
        vkDestroyDescriptorPool(vulkanDevice->device, descriptorPool, nullptr);
    vulkanDevice->descriptorSetPools.clear();

    // Destroy Command Pool
    for (VkCommandPool commandPool : vulkanDevice->commandPools)
        vkDestroyCommandPool(vulkanDevice->device, commandPool, nullptr);

    // Destroy Memory Allocator
    vmaDestroyAllocator(vulkanDevice->allocator);

    // At last, destroy device if we allocated it
    if (vulkanDevice->isOwned)
        vkDestroyDevice(vulkanDevice->device, nullptr);

    m_devices.remove(handle);
}

VulkanDevice *VulkanResourceManager::getDevice(const Handle<Device_t> &handle) const
{
    return m_devices.get(handle);
}

Handle<Queue_t> VulkanResourceManager::insertQueue(const VulkanQueue &vulkanQueue)
{
    return m_queues.emplace(vulkanQueue);
}

void VulkanResourceManager::removeQueue(const Handle<Queue_t> &handle)
{
    m_queues.remove(handle);
}

VulkanQueue *VulkanResourceManager::getQueue(const Handle<Queue_t> &handle) const
{
    return m_queues.get(handle);
}

Handle<Swapchain_t> VulkanResourceManager::createSwapchain(const Handle<Device_t> &deviceHandle,
                                                           const SwapchainOptions &options)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);
    VulkanSurface *vulkanSurface = m_surfaces.get(options.surface);

    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = vulkanSurface->surface;
    createInfo.minImageCount = options.minImageCount;
    createInfo.imageFormat = formatToVkFormat(options.format);
    createInfo.imageColorSpace = colorSpaceToVkColorSpaceKHR(options.colorSpace);
    createInfo.imageExtent = { .width = options.imageExtent.width, .height = options.imageExtent.height };
    createInfo.imageArrayLayers = options.imageLayers;
    createInfo.imageUsage = options.imageUsageFlags.toInt();
    createInfo.imageSharingMode = sharingModeToVkSharingMode(options.imageSharingMode);
    if (!options.queueTypeIndices.empty()) {
        createInfo.queueFamilyIndexCount = options.queueTypeIndices.size();
        createInfo.pQueueFamilyIndices = options.queueTypeIndices.data();
    }
    createInfo.preTransform = surfaceTransformFlagBitsToVkSurfaceTransformFlagBitsKHR(options.transform);
    createInfo.compositeAlpha = compositeAlphaFlagBitsToVkCompositeAlphaFlagBitsKHR(options.compositeAlpha);
    createInfo.presentMode = presentModeToVkPresentModeKHR(options.presentMode);
    createInfo.clipped = options.clipped;

    VulkanSwapchain *oldSwapchain = m_swapchains.get(options.oldSwapchain);
    createInfo.oldSwapchain = oldSwapchain ? oldSwapchain->swapchain : VK_NULL_HANDLE;

    VkSwapchainKHR vkSwapchain{ VK_NULL_HANDLE };
    const VkResult result = vkCreateSwapchainKHR(vulkanDevice->device, &createInfo, nullptr, &vkSwapchain);
    if (result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating swapchain: {}", result);
        return {};
    }

    const auto swapchainHandle = m_swapchains.emplace(VulkanSwapchain{
            vkSwapchain,
            options.format,
            Extent3D{ options.imageExtent.width, options.imageExtent.height, 1 },
            options.imageLayers,
            options.imageUsageFlags,
            this,
            deviceHandle });
    return swapchainHandle;
}

void VulkanResourceManager::deleteSwapchain(const Handle<Swapchain_t> &handle)
{
    VulkanSwapchain *vulkanSwapChain = m_swapchains.get(handle);
    VulkanDevice *vulkanDevice = m_devices.get(vulkanSwapChain->deviceHandle);
    vkDestroySwapchainKHR(vulkanDevice->device, vulkanSwapChain->swapchain, nullptr);
    m_swapchains.remove(handle);
}

VulkanSwapchain *VulkanResourceManager::getSwapchain(const Handle<Swapchain_t> &handle) const
{
    return m_swapchains.get(handle);
}

Handle<Surface_t> VulkanResourceManager::insertSurface(const VulkanSurface &vulkanSurface)
{
    return m_surfaces.emplace(vulkanSurface);
}

void VulkanResourceManager::deleteSurface(const Handle<Surface_t> &handle)
{
    VulkanSurface *vulkanSurface = m_surfaces.get(handle);
    if (vulkanSurface == nullptr)
        return;
    // Only destroy vkSurfaces we have allocated ourselves
    if (vulkanSurface->isOwned)
        vkDestroySurfaceKHR(vulkanSurface->instance, vulkanSurface->surface, nullptr);
    m_surfaces.remove(handle);
}

VulkanSurface *VulkanResourceManager::getSurface(const Handle<Surface_t> &handle) const
{
    return m_surfaces.get(handle);
}

Handle<Texture_t> VulkanResourceManager::insertTexture(const VulkanTexture &vulkanTexture)
{
    return m_textures.emplace(vulkanTexture);
}

void VulkanResourceManager::removeTexture(const Handle<Texture_t> &handle)
{
    m_textures.remove(handle);
}

Handle<Texture_t> VulkanResourceManager::createTexture(const Handle<Device_t> &deviceHandle, const TextureOptions &options)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    VkImageCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    createInfo.imageType = textureTypeToVkImageType(options.type);
    createInfo.format = formatToVkFormat(options.format);
    createInfo.extent = {
        .width = options.extent.width,
        .height = options.extent.height,
        .depth = options.extent.depth
    };
    createInfo.mipLevels = options.mipLevels;
    createInfo.arrayLayers = options.arrayLayers;
    createInfo.samples = sampleCountFlagBitsToVkSampleFlagBits(options.samples);
    createInfo.tiling = textureTilingToVkImageTiling(options.tiling);
    createInfo.usage = options.usage.toInt();
    createInfo.sharingMode = sharingModeToVkSharingMode(options.sharingMode);
    if (!options.queueTypeIndices.empty()) {
        createInfo.queueFamilyIndexCount = options.queueTypeIndices.size();
        createInfo.pQueueFamilyIndices = options.queueTypeIndices.data();
    }
    createInfo.initialLayout = textureLayoutToVkImageLayout(options.initialLayout);

    if (options.type == TextureType::TextureTypeCube)
        createInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    VkExternalMemoryImageCreateInfo vkExternalMemImageCreateInfo = {};
    HandleOrFD memoryHandle{};
    if (options.externalMemoryHandleType != ExternalMemoryHandleTypeFlagBits::None) {
        vkExternalMemImageCreateInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        vkExternalMemImageCreateInfo.handleTypes = externalMemoryHandleTypeToVkExternalMemoryHandleType(options.externalMemoryHandleType);
        createInfo.pNext = &vkExternalMemImageCreateInfo;
    }

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = memoryUsageToVmaMemoryUsage(options.memoryUsage);

    VkImage vkImage;
    VmaAllocation vmaAllocation;

    if (auto result = vmaCreateImage(vulkanDevice->allocator, &createInfo, &allocInfo, &vkImage, &vmaAllocation, nullptr); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating image: {}", result);
        return {};
    }

    // Retrieve Shared Memory FD/Handle
    if (options.externalMemoryHandleType != ExternalMemoryHandleTypeFlagBits::None) {
        VulkanAdapter *adapter = getAdapter(vulkanDevice->adapterHandle);
        VulkanInstance *instance = getInstance(adapter->instanceHandle);

        VmaAllocationInfo allocationInfo;
        vmaGetAllocationInfo(vulkanDevice->allocator, vmaAllocation, &allocationInfo);

#if defined(KDGPU_PLATFORM_LINUX)
        if (instance->vkGetMemoryFdKHR) {
            VkMemoryGetFdInfoKHR vkMemoryGetFdInfoKHR = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                .pNext = nullptr,
                .memory = allocationInfo.deviceMemory,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
            };
            int fd{};
            instance->vkGetMemoryFdKHR(vulkanDevice->device, &vkMemoryGetFdInfoKHR, &fd);
            memoryHandle = fd;
        }
#endif

#if defined(KDGPU_PLATFORM_WIN32)
        if (instance->vkGetMemoryWin32HandleKHR) {
            VkMemoryGetWin32HandleInfoKHR vkGetWin32HandleInfoKHR = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
                .pNext = nullptr,
                .memory = allocationInfo.deviceMemory,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
            };
            HANDLE winHandle{};
            instance->vkGetMemoryWin32HandleKHR(vulkanDevice->device, &vkGetWin32HandleInfoKHR, &winHandle);
            memoryHandle = winHandle;
        }
#endif
    }

    const auto vulkanTextureHandle = m_textures.emplace(VulkanTexture(
            vkImage,
            vmaAllocation,
            options.format,
            options.extent,
            options.mipLevels,
            options.arrayLayers,
            options.usage,
            this,
            deviceHandle,
            memoryHandle));
    return vulkanTextureHandle;
}

void VulkanResourceManager::deleteTexture(const Handle<Texture_t> &handle)
{
    VulkanTexture *vulkanTexture = m_textures.get(handle);
    if (vulkanTexture->ownedBySwapchain)
        return;

    VulkanDevice *vulkanDevice = m_devices.get(vulkanTexture->deviceHandle);

    vmaDestroyImage(vulkanDevice->allocator, vulkanTexture->image, vulkanTexture->allocation);

    m_textures.remove(handle);
}

VulkanTexture *VulkanResourceManager::getTexture(const Handle<Texture_t> &handle) const
{
    return m_textures.get(handle);
}

Handle<TextureView_t> VulkanResourceManager::createTextureView(const Handle<Device_t> &deviceHandle,
                                                               const Handle<Texture_t> &textureHandle,
                                                               const TextureViewOptions &options)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);
    VulkanTexture *vulkanTexture = m_textures.get(textureHandle);

    VkImageViewCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = vulkanTexture->image;
    createInfo.viewType = viewTypeToVkImageViewType(options.viewType);

    // Specify the format. If none specified, default to the source texture's format
    if (options.format == Format::UNDEFINED) {
        createInfo.format = formatToVkFormat(vulkanTexture->format);
    } else {
        createInfo.format = formatToVkFormat(options.format);
    }

    // Specify which subset of the texture the view exposes
    createInfo.subresourceRange = {
        .aspectMask = options.range.aspectMask.toInt(),
        .baseMipLevel = options.range.baseMipLevel,
        .levelCount = options.range.levelCount,
        .baseArrayLayer = options.range.baseArrayLayer,
        .layerCount = options.range.layerCount
    };

    // If no aspect is set, default to Color or Depth depending upon the texture usage
    if (options.range.aspectMask == TextureAspectFlagBits::None) {
        if (vulkanTexture->usage.testFlag(TextureUsageFlagBits::DepthStencilAttachmentBit))
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        else if (vulkanTexture->usage.testFlag(TextureUsageFlagBits::ColorAttachmentBit) ||
                 vulkanTexture->usage.testFlag(TextureUsageFlagBits::SampledBit)) {
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    VkImageView imageView;
    if (auto result = vkCreateImageView(vulkanDevice->device, &createInfo, nullptr, &imageView); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating image view: {}", result);
        return {};
    }

    const auto vulkanTextureViewHandle = m_textureViews.emplace(VulkanTextureView(imageView, textureHandle, deviceHandle));
    return vulkanTextureViewHandle;
}

void VulkanResourceManager::deleteTextureView(const Handle<TextureView_t> &handle)
{
    VulkanTextureView *vulkanTextureView = m_textureViews.get(handle);
    VulkanDevice *vulkanDevice = m_devices.get(vulkanTextureView->deviceHandle);
    vkDestroyImageView(vulkanDevice->device, vulkanTextureView->imageView, nullptr);

    m_textureViews.remove(handle);
}

VulkanTextureView *VulkanResourceManager::getTextureView(const Handle<TextureView_t> &handle) const
{
    return m_textureViews.get(handle);
}

Handle<Buffer_t> VulkanResourceManager::createBuffer(const Handle<Device_t> &deviceHandle, const BufferOptions &options, const void *initialData)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    VkBufferCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    createInfo.size = options.size;
    createInfo.usage = options.usage.toInt();
    createInfo.sharingMode = sharingModeToVkSharingMode(options.sharingMode);
    if (!options.queueTypeIndices.empty()) {
        createInfo.queueFamilyIndexCount = options.queueTypeIndices.size();
        createInfo.pQueueFamilyIndices = options.queueTypeIndices.data();
    }

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = memoryUsageToVmaMemoryUsage(options.memoryUsage);

    VkBuffer vkBuffer;
    VmaAllocation vmaAllocation;
    if (auto result = vmaCreateBuffer(vulkanDevice->allocator, &createInfo, &allocInfo, &vkBuffer, &vmaAllocation, nullptr); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating buffer: {}", result);
        return {};
    }

    const auto vulkanBufferHandle = m_buffers.emplace(VulkanBuffer(vkBuffer, vmaAllocation, this, deviceHandle));

    if (initialData) {
        VulkanBuffer *vulkanBuffer = m_buffers.get(vulkanBufferHandle);
        auto bufferData = vulkanBuffer->map();
        std::memcpy(bufferData, initialData, createInfo.size);
        vulkanBuffer->unmap();
    }

    return vulkanBufferHandle;
}

void VulkanResourceManager::deleteBuffer(const Handle<Buffer_t> &handle)
{
    VulkanBuffer *vulkanBuffer = m_buffers.get(handle);
    VulkanDevice *vulkanDevice = m_devices.get(vulkanBuffer->deviceHandle);

    vmaDestroyBuffer(vulkanDevice->allocator, vulkanBuffer->buffer, vulkanBuffer->allocation);

    m_buffers.remove(handle);
}

VulkanBuffer *VulkanResourceManager::getBuffer(const Handle<Buffer_t> &handle) const
{
    return m_buffers.get(handle);
}

Handle<ShaderModule_t> VulkanResourceManager::createShaderModule(const Handle<Device_t> &deviceHandle, const std::vector<uint32_t> &code)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size() * sizeof(uint32_t);
    createInfo.pCode = code.data();

    VkShaderModule vkShaderModule;
    if (auto result = vkCreateShaderModule(vulkanDevice->device, &createInfo, nullptr, &vkShaderModule); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating shader module: {}", result);
        return {};
    }

    const auto vulkanShaderModuleHandle = m_shaderModules.emplace(vkShaderModule, this, deviceHandle);
    return vulkanShaderModuleHandle;
}

void VulkanResourceManager::deleteShaderModule(const Handle<ShaderModule_t> &handle)
{
    VulkanShaderModule *shaderModule = m_shaderModules.get(handle);
    VulkanDevice *vulkanDevice = m_devices.get(shaderModule->deviceHandle);

    vkDestroyShaderModule(vulkanDevice->device, shaderModule->shaderModule, nullptr);

    m_shaderModules.remove(handle);
}

VulkanShaderModule *VulkanResourceManager::getShaderModule(const Handle<ShaderModule_t> &handle) const
{
    return m_shaderModules.get(handle);
}

Handle<PipelineLayout_t> VulkanResourceManager::createPipelineLayout(const Handle<Device_t> &deviceHandle, const PipelineLayoutOptions &options)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    // TODO: Extract the VkDescriptorSetLayout creation into a Device::createBindGroupLayout as we will need
    // to use the VkDescriptorSetLayout when creating the PipelineLayout as well as when creating the BindGroup
    assert(options.bindGroupLayouts.size() <= std::numeric_limits<uint32_t>::max());
    const uint32_t bindGroupLayoutCount = static_cast<uint32_t>(options.bindGroupLayouts.size());
    std::vector<VkDescriptorSetLayout> vkDescriptorSetLayouts;
    vkDescriptorSetLayouts.reserve(bindGroupLayoutCount);

    for (uint32_t i = 0; i < bindGroupLayoutCount; ++i) {
        VulkanBindGroupLayout *bindGroupLayout = getBindGroupLayout(options.bindGroupLayouts[i]);
        vkDescriptorSetLayouts.push_back(bindGroupLayout->descriptorSetLayout);
    }

    // Create the pipeline layout
    assert(options.pushConstantRanges.size() <= std::numeric_limits<uint32_t>::max());
    const uint32_t pushConstantRangeCount = options.pushConstantRanges.size();
    std::vector<VkPushConstantRange> vkPushConstantRanges;
    vkPushConstantRanges.reserve(pushConstantRangeCount);

    for (uint32_t i = 0; i < pushConstantRangeCount; ++i) {
        const auto &pushConstantRange = options.pushConstantRanges.at(i);

        VkPushConstantRange vkPushConstantRange = {
            .stageFlags = pushConstantRange.shaderStages.toInt(),
            .offset = pushConstantRange.offset,
            .size = pushConstantRange.size
        };

        vkPushConstantRanges.emplace_back(std::move(vkPushConstantRange));
    }

    VkPipelineLayoutCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    createInfo.setLayoutCount = static_cast<uint32_t>(vkDescriptorSetLayouts.size());
    createInfo.pSetLayouts = vkDescriptorSetLayouts.data();
    createInfo.pushConstantRangeCount = static_cast<uint32_t>(vkPushConstantRanges.size());
    createInfo.pPushConstantRanges = vkPushConstantRanges.data();

    VkPipelineLayout vkPipelineLayout{ VK_NULL_HANDLE };
    if (auto result = vkCreatePipelineLayout(vulkanDevice->device, &createInfo, nullptr, &vkPipelineLayout); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating pipeline layout: {}", result);
        return {};
    }

    // Store the results
    const auto vulkanPipelineLayoutHandle = m_pipelineLayouts.emplace(VulkanPipelineLayout(
            vkPipelineLayout,
            std::move(vkDescriptorSetLayouts),
            this,
            deviceHandle));

    return vulkanPipelineLayoutHandle;
}

void VulkanResourceManager::deletePipelineLayout(const Handle<PipelineLayout_t> &handle)
{
    VulkanPipelineLayout *vulkanPipelineLayout = m_pipelineLayouts.get(handle);
    VulkanDevice *vulkanDevice = m_devices.get(vulkanPipelineLayout->deviceHandle);

    vkDestroyPipelineLayout(vulkanDevice->device, vulkanPipelineLayout->pipelineLayout, nullptr);

    m_pipelineLayouts.remove(handle);
}

VulkanPipelineLayout *VulkanResourceManager::getPipelineLayout(const Handle<PipelineLayout_t> &handle) const
{
    return m_pipelineLayouts.get(handle);
}

Handle<GraphicsPipeline_t> VulkanResourceManager::createGraphicsPipeline(const Handle<Device_t> &deviceHandle, const GraphicsPipelineOptions &options)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    // Shader stages
    std::vector<VkPipelineShaderStageCreateInfo> shaderInfos;
    const uint32_t shaderCount = static_cast<uint32_t>(options.shaderStages.size());
    shaderInfos.reserve(shaderCount);
    for (uint32_t i = 0; i < shaderCount; ++i) {
        const auto &shaderStage = options.shaderStages.at(i);

        VkPipelineShaderStageCreateInfo shaderInfo = {};
        shaderInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderInfo.stage = shaderStageFlagBitsToVkShaderStageFlagBits(shaderStage.stage);

        // Lookup the shader module
        const auto vulkanShaderModule = getShaderModule(shaderStage.shaderModule);
        if (!vulkanShaderModule)
            return {};
        shaderInfo.module = vulkanShaderModule->shaderModule;
        shaderInfo.pName = shaderStage.entryPoint.data();

        shaderInfos.emplace_back(shaderInfo);
    }

    // Vertex input
    const uint32_t vertexBindingCount = static_cast<uint32_t>(options.vertex.buffers.size());
    std::vector<VkVertexInputBindingDescription> vertexBindings;
    vertexBindings.reserve(vertexBindingCount);
    for (uint32_t i = 0; i < vertexBindingCount; ++i) {
        const auto &binding = options.vertex.buffers.at(i);
        VkVertexInputBindingDescription vkBinding = {};
        vkBinding.binding = binding.binding;
        vkBinding.stride = binding.stride;
        vkBinding.inputRate = vertexRateToVkVertexInputRate(binding.inputRate);
        vertexBindings.emplace_back(vkBinding);
    }

    const uint32_t attributeCount = static_cast<uint32_t>(options.vertex.attributes.size());
    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.reserve(attributeCount);
    for (uint32_t i = 0; i < attributeCount; ++i) {
        const auto &attribute = options.vertex.attributes.at(i);
        VkVertexInputAttributeDescription vkAttribute = {};
        vkAttribute.location = attribute.location;
        vkAttribute.binding = attribute.binding;
        vkAttribute.format = formatToVkFormat(attribute.format);
        vkAttribute.offset = attribute.offset;
        attributes.emplace_back(vkAttribute);
    }

    VkPipelineVertexInputStateCreateInfo vertexInputState = {};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputState.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexBindings.size());
    vertexInputState.pVertexBindingDescriptions = vertexBindings.data();
    vertexInputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInputState.pVertexAttributeDescriptions = attributes.data();

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = primitiveTopologyToVkPrimitiveTopology(options.primitive.topology);
    inputAssembly.primitiveRestartEnable = options.primitive.primitiveRestart;

    // Tessellation
    VkPipelineTessellationStateCreateInfo tessellationStateInfo = {};
    tessellationStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    tessellationStateInfo.flags = 0;
    tessellationStateInfo.patchControlPoints = options.primitive.patchControlPoints;

    // Rasterizer
    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = polygonModeToVkPolygonMode(options.primitive.polygonMode);
    rasterizer.lineWidth = options.primitive.lineWidth;
    rasterizer.cullMode = options.primitive.cullMode.toInt();
    rasterizer.frontFace = frontFaceToVkFrontFace(options.primitive.frontFace);
    rasterizer.depthBiasEnable = options.primitive.depthBias.enabled;
    rasterizer.depthBiasConstantFactor = options.primitive.depthBias.biasConstantFactor;
    rasterizer.depthBiasClamp = options.primitive.depthBias.biasClamp;
    rasterizer.depthBiasSlopeFactor = options.primitive.depthBias.biasSlopeFactor;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = options.multisample.samples > SampleCountFlagBits::Samples1Bit;
    multisampling.rasterizationSamples = sampleCountFlagBitsToVkSampleFlagBits(options.multisample.samples);
    multisampling.minSampleShading = 1.0f;
    multisampling.pSampleMask = options.multisample.sampleMasks.data();
    multisampling.alphaToCoverageEnable = options.multisample.alphaToCoverageEnabled;
    multisampling.alphaToOneEnable = VK_FALSE;

    // Depth and stencil testing
    auto vkStencilOpStateFromStencilOperationOptions = [](const StencilOperationOptions &options) {
        VkStencilOpState stencilOp = {};
        stencilOp.failOp = stencilOperationToVkStencilOp(options.failOp);
        stencilOp.passOp = stencilOperationToVkStencilOp(options.passOp);
        stencilOp.depthFailOp = stencilOperationToVkStencilOp(options.depthFailOp);
        stencilOp.compareOp = compareOperationToVkCompareOp(options.compareOp);
        stencilOp.compareMask = options.compareMask;
        stencilOp.writeMask = options.writeMask;
        stencilOp.reference = options.reference;
        return stencilOp;
    };

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = options.depthStencil.depthTestEnabled;
    depthStencil.depthWriteEnable = options.depthStencil.depthWritesEnabled;
    depthStencil.depthCompareOp = compareOperationToVkCompareOp(options.depthStencil.depthCompareOperation);
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.minDepthBounds = 0.0f;
    depthStencil.maxDepthBounds = 1.0f;
    depthStencil.stencilTestEnable = options.depthStencil.stencilTestEnabled;
    depthStencil.front = vkStencilOpStateFromStencilOperationOptions(options.depthStencil.stencilFront);
    depthStencil.back = vkStencilOpStateFromStencilOperationOptions(options.depthStencil.stencilBack);

    // Blending
    const uint32_t attachmentCount = options.renderTargets.size();
    std::vector<VkPipelineColorBlendAttachmentState> attachmentBlends;
    attachmentBlends.reserve(attachmentCount);
    for (uint32_t i = 0; i < attachmentCount; ++i) {
        const auto &renderTarget = options.renderTargets.at(i);

        VkPipelineColorBlendAttachmentState vkAttachmentBlend = {};
        vkAttachmentBlend.colorWriteMask = renderTarget.writeMask.toInt();
        vkAttachmentBlend.blendEnable = renderTarget.blending.blendingEnabled;
        vkAttachmentBlend.srcColorBlendFactor = blendFactorToVkBlendFactor(renderTarget.blending.color.srcFactor);
        vkAttachmentBlend.dstColorBlendFactor = blendFactorToVkBlendFactor(renderTarget.blending.color.dstFactor);
        vkAttachmentBlend.colorBlendOp = blendOperationToVkBlendOp(renderTarget.blending.color.operation);
        vkAttachmentBlend.srcAlphaBlendFactor = blendFactorToVkBlendFactor(renderTarget.blending.alpha.srcFactor);
        vkAttachmentBlend.dstAlphaBlendFactor = blendFactorToVkBlendFactor(renderTarget.blending.alpha.dstFactor);
        vkAttachmentBlend.alphaBlendOp = blendOperationToVkBlendOp(renderTarget.blending.alpha.operation);

        attachmentBlends.emplace_back(vkAttachmentBlend);
    }

    VkPipelineColorBlendStateCreateInfo colorBlending = {};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = VK_LOGIC_OP_COPY; // Optional
    colorBlending.attachmentCount = static_cast<uint32_t>(attachmentBlends.size());
    colorBlending.pAttachments = attachmentBlends.data();
    colorBlending.blendConstants[0] = 0.0f;
    colorBlending.blendConstants[1] = 0.0f;
    colorBlending.blendConstants[2] = 0.0f;
    colorBlending.blendConstants[3] = 0.0f;

    // Dynamic pipeline state. This is state that can be overridden whilst recording
    // command buffers with commands such as vkCmdSetViewport or vkCmdSetScissor. We
    // always make the viewport and scissor states dynamic and require clients to
    // set these when recording.
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    for (auto dynamicState : options.dynamicState.enabledDynamicStates) {
        dynamicStates.push_back(dynamicStateToVkDynamicState(dynamicState));
    }

    VkPipelineDynamicStateCreateInfo dynamicStateInfo{};
    dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicStateInfo.pDynamicStates = dynamicStates.data();
    dynamicStateInfo.flags = 0;

    // We do still need to specify the number of viewports (and scissor rects) though
    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = nullptr; // Provided by dynamic state
    viewportState.scissorCount = 1;
    viewportState.pScissors = nullptr; // Provided by dynamic state

    // Fetch the specified pipeline layout
    VulkanPipelineLayout *vulkanPipelineLayout = getPipelineLayout(options.layout);
    if (!vulkanPipelineLayout) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Invalid pipeline layout requested");
        return {};
    }

    // TODO: Investigate using VK_KHR_dynamic_rendering (core in Vulkan 1.3).
    //       Do the other graphics APIs have an equivalent or perhaps they default
    //       to that sort of model? We also need this to be supported across all
    //       Vulkan target platforms (desktop, pi, android, imx8).
    //
    // Create a render pass that serves to specify the layout / compatibility of
    // concrete render passes and framebuffers used to perform rendering with this
    // pipeline at command record time. We only do this if the pipeline outputs to
    // render targets.
    VkRenderPass vkRenderPass = VK_NULL_HANDLE;
    Handle<RenderPass_t> vulkanRenderPassHandle;
    if (!options.renderTargets.empty()) {
        // Specify attachment refs for all color and resolve render targets and any
        // depth-stencil target. Concrete render passes that want to use this pipeline
        // to render, must begin a render pass that is compatible with this render pass.
        // See the detailed description of render pass compatibility at:
        //
        // https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#renderpass-compatibility
        //
        // But in short, the concrete render pass must match attachment counts of each
        // type and match the formats and sample counts in each case.

        vulkanRenderPassHandle = createRenderPass(deviceHandle,
                                                  options.renderTargets,
                                                  options.depthStencil,
                                                  options.multisample.samples,
                                                  options.viewCount);

        // Note: at the moment this render pass isn't shared. It might make sense to do so at some point,
        // in which case, the renderPass handle will have to be added to vulkanDevice->renderPasses
        VulkanRenderPass *vulkanRenderPass = m_renderPasses.get(vulkanRenderPassHandle);
        vkRenderPass = vulkanRenderPass->renderPass;
    }

    // Bring it all together in the all-knowing pipeline create info
    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderInfos.size());
    pipelineInfo.pStages = shaderInfos.data();
    pipelineInfo.pVertexInputState = &vertexInputState;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pTessellationState = &tessellationStateInfo;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicStateInfo;
    pipelineInfo.layout = vulkanPipelineLayout->pipelineLayout;
    pipelineInfo.renderPass = vkRenderPass;
    pipelineInfo.subpass = 0;

    VkPipeline vkPipeline{ VK_NULL_HANDLE };
    if (auto result = vkCreateGraphicsPipelines(vulkanDevice->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &vkPipeline); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating graphics pipeline: {}", result);
        return {};
    }

    // Create VulkanPipeline object and return handle
    const auto vulkanGraphicsPipelineHandle = m_graphicsPipelines.emplace(VulkanGraphicsPipeline(
            vkPipeline,
            this,
            vulkanRenderPassHandle,
            dynamicStates,
            deviceHandle,
            options.layout));

    return vulkanGraphicsPipelineHandle;
}

void VulkanResourceManager::deleteGraphicsPipeline(const Handle<GraphicsPipeline_t> &handle)
{
    VulkanGraphicsPipeline *vulkanPipeline = m_graphicsPipelines.get(handle);
    VulkanDevice *vulkanDevice = m_devices.get(vulkanPipeline->deviceHandle);

    vkDestroyPipeline(vulkanDevice->device, vulkanPipeline->pipeline, nullptr);

    // Note this renderPass isn't shared since we haven't added it to vulkanDevice->renderPasses
    // so we're in charge of releasing it
    VulkanRenderPass *vulkanRenderPass = m_renderPasses.get(vulkanPipeline->renderPassHandle);
    if (vulkanRenderPass) {
        vkDestroyRenderPass(vulkanDevice->device, vulkanRenderPass->renderPass, nullptr);
        m_renderPasses.remove(vulkanPipeline->renderPassHandle);
    }

    m_graphicsPipelines.remove(handle);
}

VulkanGraphicsPipeline *VulkanResourceManager::getGraphicsPipeline(const Handle<GraphicsPipeline_t> &handle) const
{
    return m_graphicsPipelines.get(handle);
}

Handle<ComputePipeline_t> VulkanResourceManager::createComputePipeline(const Handle<Device_t> &deviceHandle, const ComputePipelineOptions &options)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    // Fetch the specified pipeline layout
    VulkanPipelineLayout *vulkanPipelineLayout = getPipelineLayout(options.layout);
    if (!vulkanPipelineLayout) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Invalid pipeline layout requested");
        return {};
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo computeShaderInfo{};
    computeShaderInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    computeShaderInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;

    // Lookup the shader module
    const auto vulkanShaderModule = getShaderModule(options.shaderStage.shaderModule);
    if (!vulkanShaderModule)
        return {};
    computeShaderInfo.module = vulkanShaderModule->shaderModule;
    computeShaderInfo.pName = options.shaderStage.entryPoint.data();

    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = computeShaderInfo;
    pipelineInfo.layout = vulkanPipelineLayout->pipelineLayout;

    VkPipeline vkPipeline{ VK_NULL_HANDLE };

    if (auto result = vkCreateComputePipelines(vulkanDevice->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &vkPipeline); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating compute pipeline: {}", result);
        return {};
    }

    // Create VulkanPipeline object and return handle
    const auto vulkanComputePipelineHandle = m_computePipelines.emplace(VulkanComputePipeline(
            vkPipeline,
            this,
            deviceHandle,
            options.layout));

    return vulkanComputePipelineHandle;
}
void VulkanResourceManager::deleteComputePipeline(const Handle<ComputePipeline_t> &handle)
{
    VulkanComputePipeline *vulkanPipeline = m_computePipelines.get(handle);
    VulkanDevice *vulkanDevice = m_devices.get(vulkanPipeline->deviceHandle);

    vkDestroyPipeline(vulkanDevice->device, vulkanPipeline->pipeline, nullptr);

    m_computePipelines.remove(handle);
}

VulkanComputePipeline *VulkanResourceManager::getComputePipeline(const Handle<ComputePipeline_t> &handle) const
{
    return m_computePipelines.get(handle);
}

Handle<GpuSemaphore_t> VulkanResourceManager::createGpuSemaphore(const Handle<Device_t> &deviceHandle, const GpuSemaphoreOptions &options)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    VkSemaphoreCreateInfo semaphoreInfo = {};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkExportSemaphoreCreateInfo exportSemaphoreCreateInfo = {};
    if (options.externalSemaphoreHandleType != ExternalSemaphoreHandleTypeFlagBits::None) {
        exportSemaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        exportSemaphoreCreateInfo.pNext = nullptr;
        exportSemaphoreCreateInfo.handleTypes = externalSemaphoreHandleTypeToVkExternalSemaphoreHandleType(options.externalSemaphoreHandleType);
        semaphoreInfo.pNext = &exportSemaphoreCreateInfo;
    }

    VkSemaphore vkSemaphore{ VK_NULL_HANDLE };
    if (auto result = vkCreateSemaphore(vulkanDevice->device, &semaphoreInfo, nullptr, &vkSemaphore); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating semaphore: {}", result);
        return {};
    }

    HandleOrFD externalSemaphoreHandle{};
    if (options.externalSemaphoreHandleType != ExternalSemaphoreHandleTypeFlagBits::None) {
#if defined(KDGPU_PLATFORM_LINUX)
        if (vulkanDevice->vkGetSemaphoreFdKHR) {
            VkSemaphoreGetFdInfoKHR vulkanSemaphoreGetFdInfoKHR = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
                .pNext = nullptr,
                .semaphore = vkSemaphore,
                .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
            };
            int fd{};
            vulkanDevice->vkGetSemaphoreFdKHR(vulkanDevice->device, &vulkanSemaphoreGetFdInfoKHR, &fd);
            externalSemaphoreHandle = fd;
        }
#endif

#if defined(KDGPU_PLATFORM_WIN32)
        if (vulkanDevice->vkGetSemaphoreWin32HandleKHR) {
            VkSemaphoreGetWin32HandleInfoKHR vulkanSemaphoreGetHandleInfoKHR = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
                .pNext = nullptr,
                .semaphore = vkSemaphore,
                .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT
            };
            HANDLE winHandle{};
            vulkanDevice->vkGetSemaphoreWin32HandleKHR(vulkanDevice->device, &vulkanSemaphoreGetHandleInfoKHR, &winHandle);
            externalSemaphoreHandle = winHandle;
        }
#endif
    }

    const auto vulkanGpuSemaphoreHandle = m_gpuSemaphores.emplace(VulkanGpuSemaphore(
            vkSemaphore,
            this,
            deviceHandle,
            externalSemaphoreHandle));

    return vulkanGpuSemaphoreHandle;
}

void VulkanResourceManager::deleteGpuSemaphore(const Handle<GpuSemaphore_t> &handle)
{
    VulkanGpuSemaphore *vulkanSemaphore = m_gpuSemaphores.get(handle);
    VulkanDevice *vulkanDevice = m_devices.get(vulkanSemaphore->deviceHandle);

    vkDestroySemaphore(vulkanDevice->device, vulkanSemaphore->semaphore, nullptr);

    m_gpuSemaphores.remove(handle);
}

VulkanGpuSemaphore *VulkanResourceManager::getGpuSemaphore(const Handle<GpuSemaphore_t> &handle) const
{
    return m_gpuSemaphores.get(handle);
}

Handle<CommandRecorder_t> VulkanResourceManager::createCommandRecorder(const Handle<Device_t> &deviceHandle, const CommandRecorderOptions &options)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    // Which queue is the command recorder requested for?

    QueueDescription *queueDescription = nullptr;

    if (!options.queue.isValid()) {
        if (vulkanDevice->queueDescriptions.empty()) {
            SPDLOG_LOGGER_ERROR(Logger::logger(), "No more queue descriptors available for device");
            return {};
        }
        queueDescription = &vulkanDevice->queueDescriptions[0];
    } else {
        // Look for this queue on the device
        const auto it = std::find_if(
                vulkanDevice->queueDescriptions.begin(),
                vulkanDevice->queueDescriptions.end(),
                [options](const QueueDescription &queueDescription) { return queueDescription.queue == options.queue; });
        if (it == vulkanDevice->queueDescriptions.end()) {
            SPDLOG_LOGGER_ERROR(Logger::logger(), "Cannot find requested queue for device");
            return {};
        }
        queueDescription = &(*it);
    }

    Handle<Queue_t> queueHandle = queueDescription->queue;
    const uint32_t queueTypeIndex = queueDescription->queueTypeIndex;
    assert(queueHandle.isValid());
    assert(queueTypeIndex != std::numeric_limits<uint32_t>::max());

    // Find or create a command pool for this combination of thread and queue family
    if (vulkanDevice->commandPools[queueTypeIndex] == VK_NULL_HANDLE) {
        // No command pool exists yet for this queue family, let's create one why not!
        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queueTypeIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        VkCommandPool vkCommandPool = VK_NULL_HANDLE;
        if (auto result = vkCreateCommandPool(vulkanDevice->device, &poolInfo, nullptr, &vkCommandPool); result != VK_SUCCESS) {
            SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating command pool for queue family {}: {}", queueTypeIndex, result);
            return {};
        }
        vulkanDevice->commandPools[queueTypeIndex] = vkCommandPool;
    }

    // Create the Command Buffer
    VkCommandPool vkCommandPool = vulkanDevice->commandPools[queueTypeIndex];
    const Handle<CommandBuffer_t> commandBufferHandle = createCommandBuffer(deviceHandle,
                                                                            *queueDescription,
                                                                            options.level);

    // Finally, we can create the command recorder object
    const auto vulkanCommandRecorderHandle = m_commandRecorders.emplace(VulkanCommandRecorder(
            vkCommandPool,
            commandBufferHandle,
            this,
            deviceHandle));

    return vulkanCommandRecorderHandle;
}

void VulkanResourceManager::deleteCommandRecorder(const Handle<CommandRecorder_t> &handle)
{
    // VulkanCommandRecorder actually doesn't map to an actual Vulkan Resource.
    // It creates a VulkanCommandBuffer that holds VkCommandBuffer and whose lifetime
    m_commandRecorders.remove(handle);
}

VulkanCommandRecorder *VulkanResourceManager::getCommandRecorder(const Handle<CommandRecorder_t> &handle) const
{
    return m_commandRecorders.get(handle);
}

Handle<CommandBuffer_t> VulkanResourceManager::createCommandBuffer(const Handle<Device_t> &deviceHandle,
                                                                   const QueueDescription &queueDescription,
                                                                   CommandBufferLevel commandLevel)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);
    VkCommandPool vkCommandPool = vulkanDevice->commandPools[queueDescription.queueTypeIndex];

    // Allocate a command buffer object from the pool
    // TODO: Support secondary command buffers? Is that a thing outside of Vulkan? Do we care?
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = vkCommandPool;
    allocInfo.level = commandBufferLevelToVkCommandBufferLevel(commandLevel);
    allocInfo.commandBufferCount = 1U;

    VkCommandBuffer vkCommandBuffer{ VK_NULL_HANDLE };
    if (auto result = vkAllocateCommandBuffers(vulkanDevice->device, &allocInfo, &vkCommandBuffer); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating allocating command buffers: {}", result);
        return {};
    }

    const auto vulkanCommandBufferHandle = m_commandBuffers.emplace(VulkanCommandBuffer(vkCommandBuffer,
                                                                                        vkCommandPool,
                                                                                        allocInfo.level,
                                                                                        this,
                                                                                        deviceHandle));

    return vulkanCommandBufferHandle;
}

void VulkanResourceManager::deleteCommandBuffer(const Handle<CommandBuffer_t> &handle)
{
    VulkanCommandBuffer *commandBuffer = m_commandBuffers.get(handle);
    VulkanDevice *vulkanDevice = m_devices.get(commandBuffer->deviceHandle);

    vkFreeCommandBuffers(vulkanDevice->device, commandBuffer->commandPool, 1, &commandBuffer->commandBuffer);

    m_commandBuffers.remove(handle);
}

VulkanCommandBuffer *VulkanResourceManager::getCommandBuffer(const Handle<CommandBuffer_t> &handle) const
{
    return m_commandBuffers.get(handle);
}

Handle<RenderPassCommandRecorder_t> VulkanResourceManager::createRenderPassCommandRecorder(const Handle<Device_t> &deviceHandle,
                                                                                           const Handle<CommandRecorder_t> &commandRecorderHandle,
                                                                                           const RenderPassCommandRecorderOptions &options)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    // TODO: Should we make RenderPass and Framebuffer objects explicitly available to the API?
    // Doing so would make our API more Vulkan-like and perhaps give a tiny performance boost. On the downside
    // it's more API surface area (like Vulkan) and other backends would need to store the render pass and
    // framebuffer requirements to look them up later when using whatever wrapper they have around render passes.
    // E.g in a WebGPU backend, the render pass backend would just store the options, ready to pass to beginRenderPass().
    // For now we take a similar approach to WebGPU or the Vulkan dynamic rendering extension.

    // Find or create a render pass object that matches the request
    const VulkanRenderPassKey renderPassKey{ options };
    auto itRenderPass = vulkanDevice->renderPasses.find(renderPassKey);
    Handle<RenderPass_t> vulkanRenderPassHandle;
    if (itRenderPass == vulkanDevice->renderPasses.end()) {
        // TODO: Create the render pass and cache the handle for it
        vulkanRenderPassHandle = createRenderPass(deviceHandle,
                                                  options.colorAttachments,
                                                  options.depthStencilAttachment,
                                                  options.samples,
                                                  options.viewCount);
        vulkanDevice->renderPasses.insert({ renderPassKey, vulkanRenderPassHandle });
    } else {
        vulkanRenderPassHandle = itRenderPass->second;
    }

    VulkanRenderPass *vulkanRenderPass = m_renderPasses.get(vulkanRenderPassHandle);
    if (!vulkanRenderPass) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Unable to find or create a render pass");
        return {};
    }
    VkRenderPass vkRenderPass = vulkanRenderPass->renderPass;

    // Find or create a framebuffer as per the render pass above
    const bool usingMsaa = options.samples > SampleCountFlagBits::Samples1Bit;
    VulkanAttachmentKey attachmentKey;
    for (const auto &colorAttachment : options.colorAttachments) {
        attachmentKey.addAttachmentView(colorAttachment.view);
        // Include resolve attachments if using MSAA.
        if (usingMsaa)
            attachmentKey.addAttachmentView(colorAttachment.resolveView);
    }
    if (options.depthStencilAttachment.view.isValid()) {
        attachmentKey.addAttachmentView(options.depthStencilAttachment.view);
        if (options.depthStencilAttachment.resolveView.isValid())
            attachmentKey.addAttachmentView(options.depthStencilAttachment.resolveView);
    }

    // Take the dimensions of the first attachment as the framebuffer dimensions
    // TODO: Should this be the dimensions of the view rather than the texture itself? i.e. can we
    // use views to render to a subset of a texture?
    assert(!options.colorAttachments.empty());
    VulkanTextureView *firstView = getTextureView(options.colorAttachments.at(0).view);
    if (!firstView) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Invalid texture view when creating render pass");
        return {};
    }
    VulkanTexture *firstTexture = getTexture(firstView->textureHandle);
    if (!firstTexture) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Invalid texture when creating render pass");
        return {};
    }

    // TODO: Use VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT to create just one framebuffer rather than
    // 1 per swapchain image by allowing us to defer the image configuration to the begin render
    // pass call. See.
    // https://github.com/KhronosGroup/Vulkan-Guide/blob/main/chapters/extensions/VK_KHR_imageless_framebuffer.adoc
    VulkanFramebufferKey framebufferKey;
    framebufferKey.renderPass = vulkanRenderPassHandle;
    framebufferKey.attachmentsKey = attachmentKey;
    framebufferKey.width = firstTexture->extent.width;
    framebufferKey.height = firstTexture->extent.height;
    framebufferKey.layers = firstTexture->arrayLayers;

    if (options.viewCount > 1)
        framebufferKey.layers = 1;

    auto itFramebuffer = vulkanDevice->framebuffers.find(framebufferKey);
    Handle<Framebuffer_t> vulkanFramebufferHandle;
    if (itFramebuffer == vulkanDevice->framebuffers.end()) {
        // Create the framebuffer and cache the handle for it
        vulkanFramebufferHandle = createFramebuffer(deviceHandle, options, framebufferKey);
        vulkanDevice->framebuffers.insert({ framebufferKey, vulkanFramebufferHandle });
    } else {
        vulkanFramebufferHandle = itFramebuffer->second;
    }

    VulkanFramebuffer *vulkanFramebuffer = m_framebuffers.get(vulkanFramebufferHandle);
    if (!vulkanFramebuffer) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Could not create or find a framebuffer");
        return {};
    }
    VkFramebuffer vkFramebuffer = vulkanFramebuffer->framebuffer;

    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = vkRenderPass;
    renderPassInfo.framebuffer = vkFramebuffer;

    // Render area - assume full view area for now. Can expose as an option later if needed.
    renderPassInfo.renderArea = {
        .offset = { .x = 0, .y = 0 },
        .extent = { .width = firstTexture->extent.width, .height = firstTexture->extent.height }
    };

    // Clear values - at most 2 x color attachments + depth
    constexpr size_t MaxAttachmentCount = 20;
    assert(2 * options.colorAttachments.size() + 1 <= MaxAttachmentCount);
    std::array<VkClearValue, MaxAttachmentCount> vkClearValues;
    size_t clearIdx = 0;
    for (const auto &colorAttachment : options.colorAttachments) {
        VkClearValue vkClearValue = {};
        vkClearValue.color.uint32[0] = colorAttachment.clearValue.uint32[0];
        vkClearValue.color.uint32[1] = colorAttachment.clearValue.uint32[1];
        vkClearValue.color.uint32[2] = colorAttachment.clearValue.uint32[2];
        vkClearValue.color.uint32[3] = colorAttachment.clearValue.uint32[3];
        vkClearValues[clearIdx++] = vkClearValue;

        // Include resolve clear color again if using MSAA. Must match number of attachments.
        if (usingMsaa)
            vkClearValues[clearIdx++] = vkClearValue;
    }
    if (options.depthStencilAttachment.view.isValid()) {
        VkClearValue vkClearValue = {};
        vkClearValue.depthStencil.depth = options.depthStencilAttachment.depthClearValue;
        vkClearValue.depthStencil.stencil = options.depthStencilAttachment.stencilClearValue;
        vkClearValues[clearIdx++] = vkClearValue;

        // Include resolve clear if using MSAA and Depth Resolve. Must match number of attachments.
        if (options.depthStencilAttachment.resolveView.isValid()) {
            vkClearValues[clearIdx++] = vkClearValue;
            attachmentKey.addAttachmentView(options.depthStencilAttachment.resolveView);
        }
    }

    renderPassInfo.clearValueCount = clearIdx;
    renderPassInfo.pClearValues = vkClearValues.data();

    VulkanCommandRecorder *vulkanCommandRecorder = m_commandRecorders.get(commandRecorderHandle);
    if (!vulkanCommandRecorder) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Could not find a valid command recorder");
        return {};
    }
    VkCommandBuffer vkCommandBuffer = vulkanCommandRecorder->commandBuffer;

    vkCmdBeginRenderPass(vkCommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    const auto vulkanRenderPassCommandRecorderHandle = m_renderPassCommandRecorders.emplace(
            VulkanRenderPassCommandRecorder(vkCommandBuffer, renderPassInfo.renderArea, this, deviceHandle));
    return vulkanRenderPassCommandRecorderHandle;
}

VulkanRenderPassCommandRecorder *VulkanResourceManager::getRenderPassCommandRecorder(const Handle<RenderPassCommandRecorder_t> &handle) const
{
    return m_renderPassCommandRecorders.get(handle);
}

void VulkanResourceManager::deleteRenderPassCommandRecorder(const Handle<RenderPassCommandRecorder_t> &handle)
{
    VulkanRenderPassCommandRecorder *vulkanCommandPassRecorder = m_renderPassCommandRecorders.get(handle);

    m_renderPassCommandRecorders.remove(handle);
}

Handle<ComputePassCommandRecorder_t> VulkanResourceManager::createComputePassCommandRecorder(const Handle<Device_t> &deviceHandle,
                                                                                             const Handle<CommandRecorder_t> &commandRecorderHandle,
                                                                                             const ComputePassCommandRecorderOptions &)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    VulkanCommandRecorder *vulkanCommandRecorder = m_commandRecorders.get(commandRecorderHandle);
    if (!vulkanCommandRecorder) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Could not find a valid command recorder");
        return {};
    }
    VkCommandBuffer vkCommandBuffer = vulkanCommandRecorder->commandBuffer;

    const auto vulkanComputePassCommandRecorderHandle = m_computePassCommandRecorders.emplace(
            VulkanComputePassCommandRecorder(vkCommandBuffer, this, deviceHandle));
    return vulkanComputePassCommandRecorderHandle;
}

void VulkanResourceManager::deleteComputePassCommandRecorder(const Handle<ComputePassCommandRecorder_t> &handle)
{
    VulkanComputePassCommandRecorder *vulkanCommandPassRecorder = m_computePassCommandRecorders.get(handle);

    m_computePassCommandRecorders.remove(handle);
}

VulkanComputePassCommandRecorder *VulkanResourceManager::getComputePassCommandRecorder(const Handle<ComputePassCommandRecorder_t> &handle) const
{
    return m_computePassCommandRecorders.get(handle);
}

void VulkanResourceManager::fillColorAttachmnents(std::vector<VkAttachmentReference2> &colorAttachmentRefs,
                                                  std::vector<VkAttachmentReference2> &colorResolveAttachmentRefs,
                                                  std::vector<VkAttachmentDescription2> &attachments,
                                                  const std::vector<ColorAttachment> &colorAttachments,
                                                  SampleCountFlagBits samples)
{
    // Color and resolve attachments
    const bool usingMultisampling = samples > SampleCountFlagBits::Samples1Bit;
    const uint32_t colorTargetsCount = colorAttachments.size();
    {
        for (uint32_t i = 0; i < colorTargetsCount; ++i) {
            const ColorAttachment &renderTarget = colorAttachments.at(i);

            VulkanTextureView *view = getTextureView(renderTarget.view);
            if (!view) {
                // Log invalid view requested
                return;
            }
            VulkanTexture *texture = getTexture(view->textureHandle);
            if (!texture) {
                // Log invalid texture requested
                return;
            }

            // NB: We don't care about load/store operations and initial/final layouts here
            // so we just set some random values;
            VkAttachmentDescription2 colorAttachment = {};
            colorAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            colorAttachment.format = formatToVkFormat(texture->format);
            colorAttachment.samples = sampleCountFlagBitsToVkSampleFlagBits(samples);
            colorAttachment.loadOp = attachmentLoadOperationToVkAttachmentLoadOp(renderTarget.loadOperation);
            colorAttachment.storeOp = attachmentStoreOperationToVkAttachmentStoreOp(renderTarget.storeOperation);
            colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            colorAttachment.initialLayout = textureLayoutToVkImageLayout(renderTarget.initialLayout);
            colorAttachment.finalLayout = textureLayoutToVkImageLayout(renderTarget.finalLayout);

            VkAttachmentReference2 colorAttachmentRef = {};
            colorAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
            colorAttachmentRef.attachment = attachments.size();
            colorAttachmentRef.layout = textureLayoutToVkImageLayout(renderTarget.layout);

            colorAttachmentRefs.emplace_back(colorAttachmentRef);
            attachments.emplace_back(colorAttachment);

            // If using multisampling, then for each color attachment we need a resolve attachment
            if (usingMultisampling) {
                VulkanTextureView *view = getTextureView(renderTarget.resolveView);
                if (!view) {
                    // Log invalid view requested
                    return;
                }
                VulkanTexture *texture = getTexture(view->textureHandle);
                if (!texture) {
                    // Log invalid texture requested
                    return;
                }

                VkAttachmentDescription2 resolveAttachment = {};
                resolveAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
                resolveAttachment.format = formatToVkFormat(texture->format);
                resolveAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
                resolveAttachment.loadOp = attachmentLoadOperationToVkAttachmentLoadOp(renderTarget.loadOperation);
                resolveAttachment.storeOp = attachmentStoreOperationToVkAttachmentStoreOp(renderTarget.storeOperation);
                resolveAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                resolveAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                resolveAttachment.initialLayout = textureLayoutToVkImageLayout(renderTarget.initialLayout);
                resolveAttachment.finalLayout = textureLayoutToVkImageLayout(renderTarget.finalLayout);

                VkAttachmentReference2 resolveAttachmentRef = {};
                resolveAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
                resolveAttachmentRef.attachment = attachments.size();
                resolveAttachmentRef.layout = textureLayoutToVkImageLayout(renderTarget.layout);

                colorResolveAttachmentRefs.emplace_back(resolveAttachmentRef);
                attachments.emplace_back(resolveAttachment);
            }
        }
    }
}

void VulkanResourceManager::fillColorAttachmnents(std::vector<VkAttachmentReference2> &colorAttachmentRefs,
                                                  std::vector<VkAttachmentReference2> &colorResolveAttachmentRefs,
                                                  std::vector<VkAttachmentDescription2> &attachments,
                                                  const std::vector<RenderTargetOptions> &colorAttachments,
                                                  SampleCountFlagBits samples)
{
    const bool usingMultisampling = samples > SampleCountFlagBits::Samples1Bit;
    const uint32_t colorTargetsCount = colorAttachments.size();
    for (uint32_t i = 0; i < colorTargetsCount; ++i) {
        const auto &renderTarget = colorAttachments.at(i);

        // NB: We don't care about load/store operations and initial/final layouts here
        // so we just set some random values;
        VkAttachmentDescription2 colorAttachment = {};
        colorAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        colorAttachment.format = formatToVkFormat(renderTarget.format);
        colorAttachment.samples = sampleCountFlagBitsToVkSampleFlagBits(samples);
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = usingMultisampling ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference2 colorAttachmentRef = {};
        colorAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        colorAttachmentRef.attachment = attachments.size();
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        colorAttachmentRefs.emplace_back(colorAttachmentRef);
        attachments.emplace_back(colorAttachment);

        // If using multisampling, then for each color attachment we need a resolve attachment
        if (usingMultisampling) {
            VkAttachmentDescription2 resolveAttachment = {};
            resolveAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            resolveAttachment.format = formatToVkFormat(renderTarget.format);
            resolveAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            resolveAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            resolveAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            resolveAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            resolveAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            resolveAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            resolveAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

            VkAttachmentReference2 resolveAttachmentRef = {};
            resolveAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
            resolveAttachmentRef.attachment = attachments.size();
            resolveAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            colorResolveAttachmentRefs.emplace_back(resolveAttachmentRef);
            attachments.emplace_back(resolveAttachment);
        }
    }
}

std::pair<bool, bool> VulkanResourceManager::fillDepthAttachments(VkAttachmentReference2 &depthStencilAttachmentRef,
                                                                  VkAttachmentReference2 &depthStencilResolveAttachmentRef,
                                                                  std::vector<VkAttachmentDescription2> &attachments,
                                                                  VkSubpassDescriptionDepthStencilResolve &depthResolve,
                                                                  const DepthStencilAttachment &depthStencil,
                                                                  SampleCountFlagBits samples)
{
    const bool hasDepthAttachment = depthStencil.view.isValid();
    const bool usingMultisampling = samples > SampleCountFlagBits::Samples1Bit;
    const bool hasDepthResolveAttachment = hasDepthAttachment && usingMultisampling && depthStencil.resolveView.isValid();

    if (hasDepthAttachment) {
        const DepthStencilAttachment &renderTarget = depthStencil;

        VulkanTextureView *view = getTextureView(renderTarget.view);
        if (!view) {
            return { false, false };
            // Log invalid view requested
        }
        VulkanTexture *texture = getTexture(view->textureHandle);
        if (!texture) {
            // Log invalid texture requested
            return { false, false };
        }

        VkAttachmentDescription2 depthStencilAttachment = {};
        depthStencilAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        depthStencilAttachment.format = formatToVkFormat(texture->format);
        depthStencilAttachment.samples = sampleCountFlagBitsToVkSampleFlagBits(samples);
        depthStencilAttachment.loadOp = attachmentLoadOperationToVkAttachmentLoadOp(renderTarget.depthLoadOperation);
        depthStencilAttachment.storeOp = attachmentStoreOperationToVkAttachmentStoreOp(renderTarget.depthStoreOperation);
        depthStencilAttachment.stencilLoadOp = attachmentLoadOperationToVkAttachmentLoadOp(renderTarget.stencilLoadOperation);
        depthStencilAttachment.stencilStoreOp = attachmentStoreOperationToVkAttachmentStoreOp(renderTarget.stencilStoreOperation);
        depthStencilAttachment.initialLayout = textureLayoutToVkImageLayout(renderTarget.initialLayout);
        depthStencilAttachment.finalLayout = textureLayoutToVkImageLayout(renderTarget.finalLayout);

        depthStencilAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        depthStencilAttachmentRef.attachment = attachments.size();
        depthStencilAttachmentRef.layout = textureLayoutToVkImageLayout(renderTarget.layout);

        attachments.emplace_back(depthStencilAttachment);

        // If using multisampling, then we might need to resolve the depth attachment
        if (hasDepthResolveAttachment) {
            VulkanTextureView *resolveView = getTextureView(renderTarget.resolveView);
            if (resolveView) {
                VulkanTexture *texture = getTexture(resolveView->textureHandle);
                if (texture) {
                    VkAttachmentDescription2 resolveAttachment = {};
                    resolveAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
                    resolveAttachment.format = formatToVkFormat(texture->format);
                    resolveAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
                    resolveAttachment.loadOp = attachmentLoadOperationToVkAttachmentLoadOp(renderTarget.depthLoadOperation);
                    resolveAttachment.storeOp = attachmentStoreOperationToVkAttachmentStoreOp(renderTarget.depthStoreOperation);
                    resolveAttachment.stencilLoadOp = attachmentLoadOperationToVkAttachmentLoadOp(renderTarget.stencilLoadOperation);
                    resolveAttachment.stencilStoreOp = attachmentStoreOperationToVkAttachmentStoreOp(renderTarget.stencilStoreOperation);
                    resolveAttachment.initialLayout = textureLayoutToVkImageLayout(renderTarget.initialLayout);
                    resolveAttachment.finalLayout = textureLayoutToVkImageLayout(renderTarget.finalLayout);

                    depthStencilResolveAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
                    depthStencilResolveAttachmentRef.attachment = attachments.size();
                    depthStencilResolveAttachmentRef.layout = textureLayoutToVkImageLayout(renderTarget.layout);

                    attachments.emplace_back(resolveAttachment);

                    depthResolve.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
                    depthResolve.depthResolveMode = resolveModeToVkResolveMode(renderTarget.depthResolveMode);
                    depthResolve.stencilResolveMode = resolveModeToVkResolveMode(renderTarget.stencilResolveMode);
                    depthResolve.pDepthStencilResolveAttachment = &depthStencilResolveAttachmentRef;

                    // set a ptr to a VkSubpassDescriptionDepthStencilResolve on the VkSubpassDescription2::pNext
                    // to allow resolving of a msaaDepthBuffer
                }
            }
        }
    }

    return { hasDepthAttachment, hasDepthResolveAttachment };
}

std::pair<bool, bool> VulkanResourceManager::fillDepthAttachments(VkAttachmentReference2 &depthStencilAttachmentRef,
                                                                  VkAttachmentReference2 &depthStencilResolveAttachmentRef,
                                                                  std::vector<VkAttachmentDescription2> &attachments,
                                                                  VkSubpassDescriptionDepthStencilResolve &depthResolve,
                                                                  const DepthStencilOptions &depthStencil,
                                                                  SampleCountFlagBits samples)
{
    const bool hasDepthAttachment = depthStencil.format != Format::UNDEFINED;
    const bool usingMultisampling = samples > SampleCountFlagBits::Samples1Bit;
    const bool hasDepthResolveAttachment = hasDepthAttachment && usingMultisampling && depthStencil.resolveDepthStencil;

    if (hasDepthAttachment) {
        VkAttachmentDescription2 depthStencilAttachment = {};
        depthStencilAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        depthStencilAttachment.format = formatToVkFormat(depthStencil.format);
        depthStencilAttachment.samples = sampleCountFlagBitsToVkSampleFlagBits(samples);
        depthStencilAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthStencilAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthStencilAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthStencilAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthStencilAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthStencilAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        depthStencilAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        depthStencilAttachmentRef.attachment = attachments.size();
        depthStencilAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        attachments.emplace_back(depthStencilAttachment);

        if (usingMultisampling && depthStencil.resolveDepthStencil) {
            VkAttachmentDescription2 resolveAttachment = {};
            resolveAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            resolveAttachment.format = formatToVkFormat(depthStencil.format);
            resolveAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            resolveAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            resolveAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            resolveAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            resolveAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            resolveAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            resolveAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            depthStencilResolveAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
            depthStencilResolveAttachmentRef.attachment = attachments.size();
            depthStencilResolveAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            attachments.emplace_back(resolveAttachment);

            depthResolve.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
            depthResolve.depthResolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            depthResolve.stencilResolveMode = VK_RESOLVE_MODE_NONE;
            depthResolve.pDepthStencilResolveAttachment = &depthStencilResolveAttachmentRef;
        }
    }

    return { hasDepthAttachment, hasDepthResolveAttachment };
}

template<typename ColorAtt, typename DepthAtt>
Handle<RenderPass_t> VulkanResourceManager::createRenderPass(const Handle<Device_t> &deviceHandle,
                                                             const std::vector<ColorAtt> &colorAttachments,
                                                             const DepthAtt &depthAttachment,
                                                             SampleCountFlagBits samples,
                                                             uint32_t viewCount)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    // The subpass description will then index into the above vector of attachment
    // descriptions to specify which subpasses use which of the available attachments.
    constexpr size_t MaxAttachmentCount = 8;
    std::vector<VkAttachmentReference2> colorAttachmentRefs{};
    std::vector<VkAttachmentReference2> colorResolveAttachmentRefs{};
    std::vector<VkAttachmentDescription2> allAttachments{};
    VkAttachmentReference2 depthStencilAttachmentRef = {};
    VkAttachmentReference2 depthStencilResolveAttachmentRef = {};

    colorAttachmentRefs.reserve(MaxAttachmentCount);
    colorResolveAttachmentRefs.reserve(MaxAttachmentCount);
    allAttachments.reserve(MaxAttachmentCount * 2);

    // Color and resolve attachments
    fillColorAttachmnents(colorAttachmentRefs,
                          colorResolveAttachmentRefs,
                          allAttachments,
                          colorAttachments,
                          samples);

    // Depth-stencil attachment
    VkSubpassDescriptionDepthStencilResolve depthResolve{};
    bool hasDepthAttachment, hasDepthResolveAttachment;
    std::tie(hasDepthAttachment, hasDepthResolveAttachment) = fillDepthAttachments(depthStencilAttachmentRef,
                                                                                   depthStencilResolveAttachmentRef,
                                                                                   allAttachments,
                                                                                   depthResolve,
                                                                                   depthAttachment,
                                                                                   samples);

    assert(viewCount > 0);
    const uint32_t multiViewMaskMask = uint32_t(1 << viewCount) - 1;

    // Just create a single subpass. We do not support multiple subpasses at this
    // stage as other graphics APIs do not have an equivalent to subpasses.
    VkSubpassDescription2 subpass = {};
    subpass.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = colorAttachmentRefs.size();
    subpass.pColorAttachments = colorAttachmentRefs.data();
    subpass.pResolveAttachments = colorResolveAttachmentRefs.size() > 0 ? colorResolveAttachmentRefs.data() : nullptr;
    subpass.pDepthStencilAttachment = hasDepthAttachment ? &depthStencilAttachmentRef : nullptr;
    // Depth MSAA Resolve
    if (hasDepthResolveAttachment)
        subpass.pNext = &depthResolve;

    VkRenderPassCreateInfo2 renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
    renderPassInfo.attachmentCount = allAttachments.size();
    renderPassInfo.pAttachments = allAttachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 0;
    renderPassInfo.pDependencies = nullptr;

    if (viewCount > 1) {
        // pViewMask is a bitfield of view indices describing which views rendering is broadcast to in this subpass
        subpass.viewMask = multiViewMaskMask;
        // CorrelationMasks sets the number of views to be rendered concurrently from a mask
        // For now, we expect all of them to be rendered concurrently as we don't want to
        // mess with subpass and view dependencies.
        renderPassInfo.correlatedViewMaskCount = 1;
        renderPassInfo.pCorrelatedViewMasks = &multiViewMaskMask;
    }

    VkRenderPass vkRenderPass{ VK_NULL_HANDLE };
    if (auto result = vkCreateRenderPass2(vulkanDevice->device, &renderPassInfo, nullptr, &vkRenderPass); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating render pass: {}", result);
        return {};
    }

    const auto vulkanRenderPassHandle = m_renderPasses.emplace(VulkanRenderPass(vkRenderPass, this, deviceHandle));
    return vulkanRenderPassHandle;
}

// explicitly instantiate the template specializations so that their definitions are known at compile time (since we haven't implemented it in the header)
template Handle<RenderPass_t> VulkanResourceManager::createRenderPass<RenderTargetOptions, DepthStencilOptions>(const Handle<Device_t> &,
                                                                                                                const std::vector<RenderTargetOptions> &,
                                                                                                                const DepthStencilOptions &,
                                                                                                                SampleCountFlagBits,
                                                                                                                uint32_t);
template Handle<RenderPass_t> VulkanResourceManager::createRenderPass<ColorAttachment, DepthStencilAttachment>(const Handle<Device_t> &,
                                                                                                               const std::vector<ColorAttachment> &,
                                                                                                               const DepthStencilAttachment &,
                                                                                                               SampleCountFlagBits,
                                                                                                               uint32_t);

Handle<Framebuffer_t> VulkanResourceManager::createFramebuffer(const Handle<Device_t> &deviceHandle,
                                                               const RenderPassCommandRecorderOptions &options,
                                                               const VulkanFramebufferKey &frameBufferKey)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    VkRenderPass vkRenderPass = m_renderPasses.get(frameBufferKey.renderPass)->renderPass;

    const bool usingMsaa = options.samples > SampleCountFlagBits::Samples1Bit;
    std::vector<Handle<TextureView_t>> attachments;
    attachments.reserve(2 * options.colorAttachments.size() + 2); // (Color + Resolve) + (DepthStencil + Resolve)

    for (const auto &colorAttachment : options.colorAttachments) {
        attachments.push_back(colorAttachment.view);
        // Include resolve attachments if using MSAA.
        if (usingMsaa)
            attachments.push_back(colorAttachment.resolveView);
    }
    if (options.depthStencilAttachment.view.isValid()) {
        attachments.push_back(options.depthStencilAttachment.view);
        if (options.depthStencilAttachment.resolveView.isValid())
            attachments.push_back(options.depthStencilAttachment.resolveView);
    }

    const uint32_t attachmentCount = static_cast<uint32_t>(attachments.size());
    std::vector<VkImageView> vkAttachments;
    vkAttachments.reserve(attachmentCount);
    for (uint32_t i = 0; i < attachmentCount; ++i)
        vkAttachments.push_back(m_textureViews.get(attachments.at(i))->imageView);

    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = vkRenderPass;
    framebufferInfo.attachmentCount = attachmentCount;
    framebufferInfo.pAttachments = vkAttachments.data();
    framebufferInfo.width = frameBufferKey.width;
    framebufferInfo.height = frameBufferKey.height;
    framebufferInfo.layers = frameBufferKey.layers;

    VkFramebuffer vkFramebuffer{ VK_NULL_HANDLE };
    if (auto result = vkCreateFramebuffer(vulkanDevice->device, &framebufferInfo, nullptr, &vkFramebuffer); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating framebuffer: {}", result);
        return {};
    }

    const auto vulkanFramebufferHandle = m_framebuffers.emplace(VulkanFramebuffer(vkFramebuffer));
    return vulkanFramebufferHandle;
}

Handle<BindGroup_t> VulkanResourceManager::createBindGroup(const Handle<Device_t> &deviceHandle, const BindGroupOptions &options)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    auto createDescriptorSetPool = [](VkDevice device) {
        const std::vector<VkDescriptorPoolSize> poolSizes{
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 128 },
            { VK_DESCRIPTOR_TYPE_SAMPLER, 8 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8 }
        };

        VkDescriptorPool pool{ VK_NULL_HANDLE };
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();

        poolInfo.maxSets = 1024;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

        if (auto result = vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool); result != VK_SUCCESS) {
            SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating descriptor pool: {}", result);
            return static_cast<VkDescriptorPool>(VK_NULL_HANDLE);
        }

        return pool;
    };

    auto allocateDescriptorSet = [](VkDevice device, VkDescriptorPool descriptorPool,
                                    VulkanBindGroupLayout *bindGroupLayout, VkDescriptorSet &descriptorSet) {
        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &bindGroupLayout->descriptorSetLayout;

        return vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet);
    };

    // Have we create a DescriptorSet pool already?
    if (vulkanDevice->descriptorSetPools.empty())
        vulkanDevice->descriptorSetPools.emplace_back(createDescriptorSetPool(vulkanDevice->device));

    VulkanBindGroupLayout *bindGroupLayout = getBindGroupLayout(options.layout);
    VkDescriptorSet descriptorSet{ VK_NULL_HANDLE };

    //  Create DescriptorSet
    VkResult result = allocateDescriptorSet(vulkanDevice->device, vulkanDevice->descriptorSetPools.back(),
                                            bindGroupLayout, descriptorSet);

    // If we have run out of pool memory
    if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
        // We need to allocate a new DescriptorPool and retry
        vulkanDevice->descriptorSetPools.emplace_back(createDescriptorSetPool(vulkanDevice->device));
        result = allocateDescriptorSet(vulkanDevice->device, vulkanDevice->descriptorSetPools.back(),
                                       bindGroupLayout, descriptorSet);
    }
    if (result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when allocating descriptor set: {}", result);
        return {};
    }

    const auto vulkanBindGroupHandle = m_bindGroups.emplace(VulkanBindGroup(descriptorSet, vulkanDevice->descriptorSetPools.back(), this, deviceHandle));
    auto vulkanBindGroup = m_bindGroups.get(vulkanBindGroupHandle);

    // Set up the initial bindings
    for (const auto &resource : options.resources)
        vulkanBindGroup->update(resource);

    return vulkanBindGroupHandle;
}

void VulkanResourceManager::deleteBindGroup(const Handle<BindGroup_t> &handle)
{
    VulkanBindGroup *vulkanBindGroup = m_bindGroups.get(handle);
    VulkanDevice *vulkanDevice = m_devices.get(vulkanBindGroup->deviceHandle);

    vkFreeDescriptorSets(vulkanDevice->device, vulkanBindGroup->descriptorPool, 1, &vulkanBindGroup->descriptorSet);

    m_bindGroups.remove(handle);
}

VulkanBindGroup *VulkanResourceManager::getBindGroup(const Handle<BindGroup_t> &handle) const
{
    return m_bindGroups.get(handle);
}

Handle<BindGroupLayout_t> VulkanResourceManager::createBindGroupLayout(const Handle<Device_t> &deviceHandle, const BindGroupLayoutOptions &options)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    assert(options.bindings.size() <= std::numeric_limits<uint32_t>::max());
    const uint32_t bindingLayoutCount = static_cast<uint32_t>(options.bindings.size());
    std::vector<VkDescriptorSetLayoutBinding> vkBindingLayouts;
    vkBindingLayouts.reserve(bindingLayoutCount);

    for (uint32_t j = 0; j < bindingLayoutCount; ++j) {
        const auto &bindingLayout = options.bindings.at(j);

        VkDescriptorSetLayoutBinding vkBindingLayout = {};
        vkBindingLayout.binding = bindingLayout.binding;
        vkBindingLayout.descriptorCount = bindingLayout.count;
        vkBindingLayout.descriptorType = resourceBindingTypeToVkDescriptorType(bindingLayout.resourceType);
        vkBindingLayout.stageFlags = bindingLayout.shaderStages.toInt();
        vkBindingLayout.pImmutableSamplers = nullptr; // TODO: Expose immutable samplers?

        vkBindingLayouts.emplace_back(std::move(vkBindingLayout));
    }

    // Associate the bindings into a descriptor set layout
    VkDescriptorSetLayoutCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    createInfo.bindingCount = static_cast<uint32_t>(vkBindingLayouts.size());
    createInfo.pBindings = vkBindingLayouts.data();

    VkDescriptorSetLayout vkDescriptorSetLayout{ VK_NULL_HANDLE };
    if (auto result = vkCreateDescriptorSetLayout(vulkanDevice->device, &createInfo, nullptr, &vkDescriptorSetLayout); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating pipeline layout: {}", result);
    }

    const auto vulkanBindGroupLayoutHandle = m_bindGroupLayouts.emplace(VulkanBindGroupLayout(vkDescriptorSetLayout, deviceHandle));
    return vulkanBindGroupLayoutHandle;
}

void VulkanResourceManager::deleteBindGroupLayout(const Handle<BindGroupLayout_t> &handle)
{
    VulkanBindGroupLayout *vulkanBindGroupLayout = m_bindGroupLayouts.get(handle);
    VulkanDevice *vulkanDevice = m_devices.get(vulkanBindGroupLayout->deviceHandle);

    vkDestroyDescriptorSetLayout(vulkanDevice->device, vulkanBindGroupLayout->descriptorSetLayout, nullptr);

    m_bindGroupLayouts.remove(handle);
}

VulkanBindGroupLayout *VulkanResourceManager::getBindGroupLayout(const Handle<BindGroupLayout_t> &handle) const
{
    return m_bindGroupLayouts.get(handle);
}

Handle<Sampler_t> VulkanResourceManager::createSampler(const Handle<Device_t> &deviceHandle, const SamplerOptions &options)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = filterModeToVkFilterMode(options.magFilter);
    samplerInfo.minFilter = filterModeToVkFilterMode(options.minFilter);

    samplerInfo.addressModeU = addressModeToVkSamplerAddressMode(options.u);
    samplerInfo.addressModeV = addressModeToVkSamplerAddressMode(options.v);
    samplerInfo.addressModeW = addressModeToVkSamplerAddressMode(options.w);

    samplerInfo.anisotropyEnable = options.anisotropyEnabled;
    samplerInfo.maxAnisotropy = options.maxAnisotropy;

    samplerInfo.compareEnable = options.compareEnabled;
    samplerInfo.compareOp = compareOperationToVkCompareOp(options.compare);

    samplerInfo.mipmapMode = mipMapFilterModeToVkSamplerMipmapMode(options.mipmapFilter);
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = options.lodMinClamp;
    samplerInfo.maxLod =
            options.lodMaxClamp == MipmapLodClamping::NoClamping
            ? VK_LOD_CLAMP_NONE
            : options.lodMaxClamp;

    samplerInfo.unnormalizedCoordinates = !options.normalizedCoordinates;

    VkSampler sampler{ VK_NULL_HANDLE };
    if (auto result = vkCreateSampler(vulkanDevice->device, &samplerInfo, nullptr, &sampler); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating sampler: {}", result);
        return {};
    }

    auto samplerHandle = m_samplers.emplace(VulkanSampler(sampler, deviceHandle));
    return samplerHandle;
}

void VulkanResourceManager::deleteSampler(const Handle<Sampler_t> &handle)
{
    VulkanSampler *sampler = m_samplers.get(handle);
    VulkanDevice *vulkanDevice = m_devices.get(sampler->deviceHandle);

    vkDestroySampler(vulkanDevice->device, sampler->sampler, nullptr);

    m_samplers.remove(handle);
}

VulkanSampler *VulkanResourceManager::getSampler(const Handle<Sampler_t> &handle) const
{
    return m_samplers.get(handle);
}

Handle<Fence_t> VulkanResourceManager::createFence(const Handle<Device_t> &deviceHandle, const FenceOptions &options)
{
    VulkanDevice *vulkanDevice = m_devices.get(deviceHandle);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (options.createSignalled)
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkFence fence{ VK_NULL_HANDLE };
    if (auto result = vkCreateFence(vulkanDevice->device, &fenceInfo, nullptr, &fence); result != VK_SUCCESS) {
        SPDLOG_LOGGER_ERROR(Logger::logger(), "Error when creating fence: {}", result);
        return {};
    }

    auto fenceHandle = m_fences.emplace(VulkanFence(fence, this, deviceHandle));
    return fenceHandle;
}

void VulkanResourceManager::deleteFence(const Handle<Fence_t> &handle)
{
    VulkanFence *fence = m_fences.get(handle);
    VulkanDevice *vulkanDevice = m_devices.get(fence->deviceHandle);

    vkDestroyFence(vulkanDevice->device, fence->fence, nullptr);

    m_fences.remove(handle);
}

VulkanFence *VulkanResourceManager::getFence(const Handle<Fence_t> &handle) const
{
    return m_fences.get(handle);
}

} // namespace KDGpu
