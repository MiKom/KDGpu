#include "vulkan_shader_module.h"

namespace ToyRenderer {

VulkanShaderModule::VulkanShaderModule(VkShaderModule _shaderModule,
                                       VulkanResourceManager *_vulkanResourceManager,
                                       const Handle<Device_t> _deviceHandle)
    : ApiShaderModule()
    , shaderModule(_shaderModule)
    , vulkanResourceManager(_vulkanResourceManager)
    , deviceHandle(_deviceHandle)
{
}

} // namespace ToyRenderer
