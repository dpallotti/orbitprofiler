// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <absl/base/casts.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <csignal>
#include <optional>
#include <thread>

#include "OrbitBase/Logging.h"
#include "OrbitBase/WriteStringToFile.h"
#include "frag_spv.h"
#include "vert_spv.h"

#define CHECK_VK(call)           \
  do {                           \
    CHECK((call) == VK_SUCCESS); \
  } while (false)

namespace {

const uint32_t kWidth = 800;
const uint32_t kHeight = 600;

const std::vector<const char*> kValidationLayers = {"VK_LAYER_KHRONOS_validation"};

#ifdef NDEBUG
const bool kEnableValidationLayers = false;
#else
const bool kEnableValidationLayers = true;
#endif

class VulkanTutorial {
 public:
  void Run(std::atomic<bool>* exit_requested) {
    InitVulkan();
    MainLoop(exit_requested);
    CleanUp();
  }

 private:
  void InitVulkan() {
    LOG("InitVulkan");
    CreateInstance();
    // TODO: CreateSurface():
    //  https://vulkan-tutorial.com/en/Drawing_a_triangle/Presentation/Window_surface
    PickPhysicalDevice();
    CreateLogicalDevice();
    // TODO: CreateSwapChain()
    //  https://vulkan-tutorial.com/en/Drawing_a_triangle/Presentation/Swap_chain
    CreateOffscreenImage();
    CreateImageView();
    CreateRenderPass();
    CreateGraphicsPipeline();
    CreateFramebuffer();
    CreateCommandPool();
    CreateCommandBuffer();
    CreateFence();
  }

  void MainLoop(std::atomic<bool>* exit_requested) {
    LOG("MainLoop");
    while (!*exit_requested) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      DrawFrame();
    }
    vkDeviceWaitIdle(device_);
  }

  void CleanUp() {
    LOG("CleanUp");
    vkDestroyFence(device_, fence_, nullptr);
    // Command buffers will be automatically freed when their command pool is destroyed, so we don't
    // need an explicit cleanup.
    vkDestroyCommandPool(device_, command_pool_, nullptr);
    vkDestroyFramebuffer(device_, framebuffer_, nullptr);
    vkDestroyPipeline(device_, graphics_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    vkDestroyRenderPass(device_, render_pass_, nullptr);
    vkDestroyImageView(device_, image_view_, nullptr);
    vkFreeMemory(device_, memory_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    vkDestroyDevice(device_, nullptr);
    vkDestroyInstance(instance_, nullptr);
  }

  // ----
  void CreateInstance() {
    VkApplicationInfo app_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "VulkanTutorial",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0,
    };

    VkInstanceCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        // TODO: Extension for window system:
        //  https://vulkan-tutorial.com/Drawing_a_triangle/Setup/Instance
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
    };
    if (kEnableValidationLayers) {
      CHECK(AreValidationLayersSupported());
      create_info.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
      create_info.ppEnabledLayerNames = kValidationLayers.data();
    }

    CHECK_VK(vkCreateInstance(&create_info, nullptr, &instance_));
  }

  static bool AreValidationLayersSupported() {
    uint32_t layer_count = 0;
    CHECK_VK(vkEnumerateInstanceLayerProperties(&layer_count, nullptr));

    std::vector<VkLayerProperties> available_layers(layer_count);
    CHECK_VK(vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data()));

    for (const char* layer_name : kValidationLayers) {
      bool layer_found = false;

      for (const VkLayerProperties& layer_properties : available_layers) {
        if (strcmp(layer_properties.layerName, layer_name) == 0) {
          layer_found = true;
          break;
        }
      }

      if (!layer_found) {
        return false;
      }
    }
    return true;
  }

  void PickPhysicalDevice() {
    uint32_t physical_device_count = 0;
    CHECK_VK(vkEnumeratePhysicalDevices(instance_, &physical_device_count, nullptr));
    CHECK(physical_device_count > 0);

    std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
    vkEnumeratePhysicalDevices(instance_, &physical_device_count, physical_devices.data());

    for (const VkPhysicalDevice& physical_device : physical_devices) {
      if (IsPhysicalDeviceSuitable(physical_device)) {
        physical_device_ = physical_device;
        break;
      }
    }

    CHECK(physical_device_ != VK_NULL_HANDLE);
  }

  static bool IsPhysicalDeviceSuitable(const VkPhysicalDevice& physical_device) {
    QueueFamilyIndices queue_family_indices = FindQueueFamilies(physical_device);
    return queue_family_indices.graphics_family.has_value();
  }

  struct QueueFamilyIndices {
    std::optional<uint32_t> graphics_family;
  };

  static QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice physical_device) {
    QueueFamilyIndices queue_family_indices;

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);

    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count,
                                             queue_families.data());

    int queue_family_index = 0;
    for (const VkQueueFamilyProperties& queue_family : queue_families) {
      if ((queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
        queue_family_indices.graphics_family = queue_family_index;
      }
      queue_family_index++;
    }

    return queue_family_indices;
  }

  void CreateLogicalDevice() {
    QueueFamilyIndices queue_family_indices = FindQueueFamilies(physical_device_);

    constexpr float kQueuePriority = 1.0f;
    VkDeviceQueueCreateInfo queue_create_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family_indices.graphics_family.value(),
        .queueCount = 1,
        .pQueuePriorities = &kQueuePriority,
    };

    VkPhysicalDeviceFeatures device_features{};

    VkDeviceCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
        .pEnabledFeatures = &device_features,
    };
    if (kEnableValidationLayers) {
      create_info.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
      create_info.ppEnabledLayerNames = kValidationLayers.data();
    }

    CHECK_VK(vkCreateDevice(physical_device_, &create_info, nullptr, &device_));

    vkGetDeviceQueue(device_, queue_family_indices.graphics_family.value(), 0, &graphics_queue_);
  }

  void CreateOffscreenImage() {
    VkImageCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_UNDEFINED,
        .extent = {.width = kWidth, .height = kHeight, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        //        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    image_extent_ = create_info.extent;

    if (false) {
      for (int format_int = VK_FORMAT_BEGIN_RANGE; format_int <= VK_FORMAT_END_RANGE;
           ++format_int) {
        const auto format = static_cast<VkFormat>(format_int);

        VkImageFormatProperties format_properties{};
        VkResult result = vkGetPhysicalDeviceImageFormatProperties(
            physical_device_, format, create_info.imageType, create_info.tiling, create_info.usage,
            create_info.flags, &format_properties);
        if (result == VK_SUCCESS) {
          image_format_ = format;
          break;
        }
      }
    } else {
      image_format_ = VK_FORMAT_R8G8B8A8_UNORM;
    }

    CHECK(image_format_ != VkFormat::VK_FORMAT_UNDEFINED);
    LOG("image_format_=%d", image_format_);
    create_info.format = image_format_;
    CHECK_VK(vkCreateImage(device_, &create_info, nullptr, &image_));

    // Good reference for the code that follows:
    // https://www.informit.com/articles/article.aspx?p=2756465&seqNum=3

    VkMemoryRequirements memory_requirements{};
    vkGetImageMemoryRequirements(device_, image_, &memory_requirements);
    CHECK(memory_requirements.memoryTypeBits != 0);
    LOG("memory_requirements.memoryTypeBits: %#lx", memory_requirements.memoryTypeBits);

    // "memoryTypeBits is a bitmask and contains one bit set for every supported memory type for the
    // resource. Bit i is set if and only if the memory type i in the
    // VkPhysicalDeviceMemoryProperties structure for the physical device is supported for the
    // resource."
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
    //    uint32_t memory_type_index = __builtin_ctz(memory_requirements.memoryTypeBits);
    uint32_t memory_type_index = -1;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
      const auto required_properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      if ((memory_requirements.memoryTypeBits & (1 << i)) != 0) {
        LOG("memoryTypeBits: %d", i);
        LOG("memory_properties.memoryTypes[i].propertyFlags: %#lx",
            memory_properties.memoryTypes[i].propertyFlags);
      }
      if ((memory_requirements.memoryTypeBits & (1 << i)) != 0 &&
          (memory_properties.memoryTypes[i].propertyFlags & required_properties) ==
              required_properties) {
        memory_type_index = i;
        break;
      }
    }
    LOG("memory_type_index=%d", memory_type_index);
    CHECK(memory_type_index < memory_properties.memoryTypeCount);

    VkMemoryAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory_requirements.size,
        .memoryTypeIndex = memory_type_index,
    };
    CHECK_VK(vkAllocateMemory(device_, &allocate_info, nullptr, &memory_));

    CHECK_VK(vkBindImageMemory(device_, image_, memory_, 0));
  }

  void CreateImageView() {
    VkImageViewCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_format_,
        .components = {.r = VK_COMPONENT_SWIZZLE_IDENTITY,
                       .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                       .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                       .a = VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel = 0,
                             .levelCount = 1,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
    };
    CHECK_VK(vkCreateImageView(device_, &create_info, nullptr, &image_view_));
  }

  void CreateRenderPass() {
    VkAttachmentDescription color_attachment{
        .format = image_format_,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentReference color_attachment_ref{
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass{
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment_ref,
        .pResolveAttachments = nullptr,
        .pDepthStencilAttachment = nullptr,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = nullptr,
    };

    // TODO: VkSubpassDependency?
    //  https://vulkan-tutorial.com/Drawing_a_triangle/Drawing/Rendering_and_presentation

    VkRenderPassCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 0,
        .pDependencies = nullptr,
    };
    CHECK_VK(vkCreateRenderPass(device_, &create_info, nullptr, &render_pass_));
  }

  VkShaderModule CreateShaderModule(const uint8_t* shader_code, size_t shader_code_size) {
    VkShaderModuleCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_code_size,
        .pCode = absl::bit_cast<const uint32_t*>(shader_code),
    };

    VkShaderModule shader_module{};
    CHECK_VK(vkCreateShaderModule(device_, &create_info, nullptr, &shader_module));
    return shader_module;
  }

  void CreateGraphicsPipeline() {
    VkShaderModule vertex_shader_module = CreateShaderModule(kVertSpv, kVertSpvLength);
    VkShaderModule fragment_shader_module = CreateShaderModule(kFragSpv, kFragSpvLength);

    VkPipelineShaderStageCreateInfo vertex_shader_stage_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vertex_shader_module,
        .pName = "main",
        .pSpecializationInfo = nullptr,
    };

    VkPipelineShaderStageCreateInfo fragment_shader_stage_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = fragment_shader_module,
        .pName = "main",
        .pSpecializationInfo = nullptr,
    };

    std::array shader_stages = {vertex_shader_stage_info, fragment_shader_stage_info};

    VkPipelineVertexInputStateCreateInfo vertex_input_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions = nullptr,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkViewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = kWidth,
        .height = kHeight,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    VkRect2D scissor{
        .offset = {0, 0},
        .extent = {image_extent_.width, image_extent_.height},
    };

    VkPipelineViewportStateCreateInfo viewport_state_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisampling_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 1.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };

    VkPipelineColorBlendAttachmentState color_blend_attachment{
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo color_blending_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
        .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 0,
        .pSetLayouts = nullptr,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = nullptr,
    };
    CHECK_VK(vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr, &pipeline_layout_));

    VkGraphicsPipelineCreateInfo pipeline_create_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = shader_stages.size(),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly_info,
        .pTessellationState = nullptr,
        .pViewportState = &viewport_state_info,
        .pRasterizationState = &rasterizer_info,
        .pMultisampleState = &multisampling_info,
        .pDepthStencilState = nullptr,
        .pColorBlendState = &color_blending_info,
        .pDynamicState = nullptr,
        .layout = pipeline_layout_,
        .renderPass = render_pass_,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };
    CHECK_VK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                       &graphics_pipeline_));

    vkDestroyShaderModule(device_, fragment_shader_module, nullptr);
    vkDestroyShaderModule(device_, vertex_shader_module, nullptr);
  }

  void CreateFramebuffer() {
    VkFramebufferCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = render_pass_,
        .attachmentCount = 1,
        .pAttachments = &image_view_,
        .width = image_extent_.width,
        .height = image_extent_.height,
        .layers = 1,
    };
    CHECK_VK(vkCreateFramebuffer(device_, &create_info, nullptr, &framebuffer_));
  }

  void CreateCommandPool() {
    QueueFamilyIndices queue_family_indices = FindQueueFamilies(physical_device_);

    VkCommandPoolCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = queue_family_indices.graphics_family.value(),
    };
    CHECK_VK(vkCreateCommandPool(device_, &create_info, nullptr, &command_pool_));
  }

  void CreateCommandBuffer() {
    VkCommandBufferAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool_,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    CHECK_VK(vkAllocateCommandBuffers(device_, &alloc_info, &command_buffer_));

    VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pInheritanceInfo = nullptr,
    };
    CHECK_VK(vkBeginCommandBuffer(command_buffer_, &begin_info));

    VkClearValue clear_value{.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};

    VkRenderPassBeginInfo render_pass_info{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass_,
        .framebuffer = framebuffer_,
        .renderArea = {.offset = {0, 0},
                       .extent = {.width = image_extent_.width, .height = image_extent_.height}},
        .clearValueCount = 1,
        .pClearValues = &clear_value,
    };

    vkCmdBeginRenderPass(command_buffer_, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline_);

    vkCmdDraw(command_buffer_, 3, 1, 0, 0);

    vkCmdEndRenderPass(command_buffer_);

    CHECK_VK(vkEndCommandBuffer(command_buffer_));
  }

  void CreateFence() {
    VkFenceCreateInfo fence_create_info{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    CHECK_VK(vkCreateFence(device_, &fence_create_info, nullptr, &fence_));
  }

  void DrawFrame() {
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &fence_);

    SaveScreenshot();

    VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer_,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    CHECK_VK(vkQueueSubmit(graphics_queue_, 1, &submit_info, fence_));
  }

  // Refer to https://github.com/SaschaWillems/Vulkan/blob/master/examples/screenshot/screenshot.cpp
  void SaveScreenshot() {
    VkImageSubresource subresource{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .arrayLayer = 0,
    };
    VkSubresourceLayout subresource_layout;
    vkGetImageSubresourceLayout(device_, image_, &subresource, &subresource_layout);

    char* data;
    CHECK_VK(vkMapMemory(device_, memory_, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&data)));
    data += subresource_layout.offset;

    std::string output;
    output.append("P6\n")
        .append(absl::StrFormat("%d\n", image_extent_.width))
        .append(absl::StrFormat("%d\n", image_extent_.height))
        .append("255\n");
    for (uint32_t y = 0; y < image_extent_.height; ++y) {
      uint32_t* row = reinterpret_cast<uint32_t*>(data);
      for (uint32_t x = 0; x < image_extent_.width; ++x) {
        // Change to RGB.
        output.push_back(reinterpret_cast<char*>(row)[2]);
        output.push_back(reinterpret_cast<char*>(row)[1]);
        output.push_back(reinterpret_cast<char*>(row)[0]);
        ++row;
      }
      data += subresource_layout.rowPitch;
    }
    CHECK(orbit_base::WriteStringToFile("screenshot.ppm", output).has_value());

    vkUnmapMemory(device_, memory_);
  }

 private:
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue graphics_queue_ = VK_NULL_HANDLE;
  VkFormat image_format_ = VkFormat::VK_FORMAT_UNDEFINED;
  VkExtent3D image_extent_{};
  VkImage image_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  VkImageView image_view_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline graphics_pipeline_ = VK_NULL_HANDLE;
  VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
};

std::atomic<bool> exit_requested;

void SigintHandler(int signum) {
  if (signum == SIGINT) {
    exit_requested = true;
  }
}

void InstallSigintHandler() {
  struct sigaction act {};
  act.sa_handler = SigintHandler;
  sigemptyset(&act.sa_mask);
  act.sa_flags = 0;
  act.sa_restorer = nullptr;
  sigaction(SIGINT, &act, nullptr);
}

}  // namespace

int main() {
  InstallSigintHandler();
  LOG("kEnableValidationLayers=%d", kEnableValidationLayers);

  VulkanTutorial app{};
  app.Run(&exit_requested);

  LOG("Finished");
  return 0;
}