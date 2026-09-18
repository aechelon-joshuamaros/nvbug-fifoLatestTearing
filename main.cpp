#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

static uint32_t choose(const char *prompt, uint32_t count) {
    uint32_t value = 0;
    do {
        std::cout << prompt << " [0-" << count - 1 << "]: ";
        std::string line;
        std::getline(std::cin, line);
        value = uint32_t(std::stoul(line));
    } while (value >= count);
    return value;
}

static std::vector<uint32_t> loadSpv(const char *path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    assert(file);
    const std::streamsize size = file.tellg();
    assert(size > 0 && size % 4 == 0);
    std::vector<uint32_t> code(uint32_t(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(code.data()), size);
    assert(file);
    return code;
}

int main() {
    ////////////////////////////////////////////////////////////////////////
    // Create instance.
    ////////////////////////////////////////////////////////////////////////

    VkApplicationInfo applicationInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Direct display tear reproduction",
        .applicationVersion = 1,
        .pEngineName = "none",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_3,
    };
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char *extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_DISPLAY_EXTENSION_NAME,
        VK_EXT_DIRECT_MODE_DISPLAY_EXTENSION_NAME,
    };
    VkInstanceCreateInfo instanceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &applicationInfo,
        .enabledLayerCount = 1,
        .ppEnabledLayerNames = layers,
        .enabledExtensionCount = 3,
        .ppEnabledExtensionNames = extensions,
    };
    VkInstance instance;
    assert(vkCreateInstance(&instanceCreateInfo, nullptr, &instance) == VK_SUCCESS);
    PFN_vkAcquireWinrtDisplayNV acquireWinrtDisplay = reinterpret_cast<PFN_vkAcquireWinrtDisplayNV>(
        vkGetInstanceProcAddr(instance, "vkAcquireWinrtDisplayNV")
    );
    assert(acquireWinrtDisplay);

    ////////////////////////////////////////////////////////////////////////
    // Select device and display
    ////////////////////////////////////////////////////////////////////////

    uint32_t physicalDeviceCount = 0;
    assert(vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr) == VK_SUCCESS);
    assert(physicalDeviceCount != 0);
    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    assert(
        vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices.data()) ==
        VK_SUCCESS
    );
    for (uint32_t i = 0; i < physicalDeviceCount; ++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physicalDevices[i], &properties);
        std::cout << i << ": " << properties.deviceName << std::endl;
    }
    const uint32_t physicalDeviceIndex = choose("Physical device", physicalDeviceCount);
    VkPhysicalDevice physicalDevice = physicalDevices[physicalDeviceIndex];

    uint32_t displayCount = 0;
    assert(
        vkGetPhysicalDeviceDisplayPropertiesKHR(physicalDevice, &displayCount, nullptr) ==
        VK_SUCCESS
    );
    assert(displayCount != 0);
    std::vector<VkDisplayPropertiesKHR> displays(displayCount);
    assert(
        vkGetPhysicalDeviceDisplayPropertiesKHR(physicalDevice, &displayCount, displays.data()) ==
        VK_SUCCESS
    );
    for (uint32_t i = 0; i < displayCount; ++i)
        std::cout << i << ": "
                  << (displays[i].displayName ? displays[i].displayName : "unnamed display")
                  << std::endl;
    const uint32_t displayIndex = choose("Display", displayCount);
    VkDisplayKHR display = displays[displayIndex].display;

    uint32_t planeCount = 0;
    assert(
        vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physicalDevice, &planeCount, nullptr) ==
        VK_SUCCESS
    );
    assert(planeCount != 0);
    std::vector<VkDisplayPlanePropertiesKHR> planes(planeCount);
    assert(
        vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physicalDevice, &planeCount, planes.data()) ==
        VK_SUCCESS
    );
    std::optional<uint32_t> selectedPlane;
    for (uint32_t i = 0; i < planeCount; ++i) {
        uint32_t supportedCount = 0;
        assert(
            vkGetDisplayPlaneSupportedDisplaysKHR(physicalDevice, i, &supportedCount, nullptr) ==
            VK_SUCCESS
        );
        std::vector<VkDisplayKHR> supportedDisplays(supportedCount);
        assert(
            vkGetDisplayPlaneSupportedDisplaysKHR(
                physicalDevice, i, &supportedCount, supportedDisplays.data()
            ) == VK_SUCCESS
        );
        for (VkDisplayKHR supportedDisplay : supportedDisplays)
            if (supportedDisplay == display)
                selectedPlane = i;
    }
    assert(selectedPlane.has_value());

    uint32_t modeCount = 0;
    assert(
        vkGetDisplayModePropertiesKHR(physicalDevice, display, &modeCount, nullptr) == VK_SUCCESS
    );
    assert(modeCount != 0);
    std::vector<VkDisplayModePropertiesKHR> modes(modeCount);
    assert(
        vkGetDisplayModePropertiesKHR(physicalDevice, display, &modeCount, modes.data()) ==
        VK_SUCCESS
    );
    std::optional<uint32_t> selectedMode;
    for (uint32_t i = 0; i < modeCount; ++i) {
        if (!selectedMode.has_value()) {
            selectedMode = i;
            continue;
        }
        const VkDisplayModeParametersKHR &candidate = modes[i].parameters;
        const VkDisplayModeParametersKHR &current = modes[*selectedMode].parameters;
        if (candidate.visibleRegion.width >= current.visibleRegion.width &&
            candidate.visibleRegion.height >= current.visibleRegion.height &&
            candidate.refreshRate >= current.refreshRate)
            selectedMode = i;
    }
    assert(selectedMode.has_value());

    ////////////////////////////////////////////////////////////////////////
    // Acquire display and create surface.
    ////////////////////////////////////////////////////////////////////////

    assert(acquireWinrtDisplay(physicalDevice, display) == VK_SUCCESS);
    VkDisplayModeKHR displayMode = modes[*selectedMode].displayMode;
    VkDisplayModeParametersKHR modeParams = modes[*selectedMode].parameters;
    std::cout << "Resolution: " << modeParams.visibleRegion.width << "x"
              << modeParams.visibleRegion.height
              << ", refresh rate: " << modeParams.refreshRate / 1000.0 << " Hz" << std::endl;

    VkDisplaySurfaceCreateInfoKHR displaySurfaceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = displayMode,
        .planeIndex = *selectedPlane,
        .planeStackIndex = planes[*selectedPlane].currentStackIndex,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha = 1.0f,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = modeParams.visibleRegion,
    };
    VkSurfaceKHR surface;
    assert(
        vkCreateDisplayPlaneSurfaceKHR(instance, &displaySurfaceCreateInfo, nullptr, &surface) ==
        VK_SUCCESS
    );

    ////////////////////////////////////////////////////////////////////////
    // Select surface format and present mode.
    ////////////////////////////////////////////////////////////////////////

    uint32_t presentModeCount = 0;
    assert(
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            physicalDevice, surface, &presentModeCount, nullptr
        ) == VK_SUCCESS
    );
    assert(presentModeCount != 0);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    assert(
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            physicalDevice, surface, &presentModeCount, presentModes.data()
        ) == VK_SUCCESS
    );
    std::cout << "Present mode values include VkPresentModeKHR(number)." << std::endl;
    for (uint32_t i = 0; i < presentModeCount; ++i) {
        std::cout << i << ": ";
        switch (presentModes[i]) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
            std::cout << "immediate";
            break;
        case VK_PRESENT_MODE_MAILBOX_KHR:
            std::cout << "mailbox";
            break;
        case VK_PRESENT_MODE_FIFO_KHR:
            std::cout << "fifo";
            break;
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
            std::cout << "fifo_relaxed";
            break;
        case VK_PRESENT_MODE_FIFO_LATEST_READY_KHR:
            std::cout << "fifo_latest_ready";
            break;
        default:
            std::cout << "VkPresentModeKHR(" << presentModes[i] << ")";
            break;
        }
        std::cout << std::endl;
    }
    const VkPresentModeKHR presentMode = presentModes[choose("Present mode", presentModeCount)];

    uint32_t formatCount = 0;
    assert(
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr) ==
        VK_SUCCESS
    );
    assert(formatCount != 0);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    assert(
        vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice, surface, &formatCount, formats.data()
        ) == VK_SUCCESS
    );
    std::optional<VkSurfaceFormatKHR> surfaceFormat;
    for (const VkSurfaceFormatKHR &format : formats) {
        if (format.format == VK_FORMAT_R8G8B8A8_UNORM ||
            format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8_UNORM ||
            format.format == VK_FORMAT_B8G8R8_UNORM) {
            surfaceFormat = format;
            break;
        }
    }
    assert(surfaceFormat.has_value());

    ////////////////////////////////////////////////////////////////////////
    // Create device
    ////////////////////////////////////////////////////////////////////////

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice, &queueFamilyCount, queueFamilies.data()
    );
    std::optional<uint32_t> queueFamily;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        VkBool32 presentSupported = VK_FALSE;
        assert(
            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &presentSupported) ==
            VK_SUCCESS
        );
        if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentSupported) {
            queueFamily = i;
            break;
        }
    }
    assert(queueFamily.has_value());
    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = *queueFamily,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };
    uint32_t deviceExtensionCount = 0;
    assert(
        vkEnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &deviceExtensionCount, nullptr
        ) == VK_SUCCESS
    );
    std::vector<VkExtensionProperties> deviceExtensionProperties(deviceExtensionCount);
    assert(
        vkEnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &deviceExtensionCount, deviceExtensionProperties.data()
        ) == VK_SUCCESS
    );
    bool fifoLatestReadyExtension = false;
    bool acquireWinrtDisplayExtension = false;
    for (const VkExtensionProperties &extension : deviceExtensionProperties) {
        if (std::string(extension.extensionName) ==
            VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME)
            fifoLatestReadyExtension = true;
        if (std::string(extension.extensionName) == VK_NV_ACQUIRE_WINRT_DISPLAY_EXTENSION_NAME)
            acquireWinrtDisplayExtension = true;
    }
    assert(acquireWinrtDisplayExtension);

    VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR fifoLatestReadyFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR,
    };
    bool fifoLatestReady = false;
    if (fifoLatestReadyExtension) {
        VkPhysicalDeviceFeatures2 features2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &fifoLatestReadyFeatures,
        };
        vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
        fifoLatestReady = fifoLatestReadyFeatures.presentModeFifoLatestReady == VK_TRUE;
    }
    VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        .pNext = fifoLatestReady ? &fifoLatestReadyFeatures : nullptr,
        .dynamicRendering = VK_TRUE,
    };
    std::vector<const char *> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_NV_ACQUIRE_WINRT_DISPLAY_EXTENSION_NAME
    };
    if (fifoLatestReady)
        deviceExtensions.push_back(VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
    VkDeviceCreateInfo deviceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &dynamicRendering,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueCreateInfo,
        .enabledExtensionCount = uint32_t(deviceExtensions.size()),
        .ppEnabledExtensionNames = deviceExtensions.data(),
    };
    VkDevice device;
    assert(vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device) == VK_SUCCESS);
    VkQueue queue;
    vkGetDeviceQueue(device, *queueFamily, 0, &queue);

    ////////////////////////////////////////////////////////////////////////
    // Create swapchain and retrieve images.
    ////////////////////////////////////////////////////////////////////////

    VkSurfaceCapabilitiesKHR capabilities;
    assert(
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities) ==
        VK_SUCCESS
    );
    assert(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    const VkExtent2D swapchainExtent = modeParams.visibleRegion;
    const uint32_t swapchainImageCount = 5;
    assert(swapchainImageCount >= capabilities.minImageCount);
    assert(capabilities.maxImageCount == 0 || swapchainImageCount <= capabilities.maxImageCount);
    std::cout << "Using " << swapchainImageCount << " swapchain images." << std::endl;
    VkSwapchainCreateInfoKHR swapchainCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = swapchainImageCount,
        .imageFormat = surfaceFormat->format,
        .imageColorSpace = surfaceFormat->colorSpace,
        .imageExtent = swapchainExtent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = presentMode,
        .clipped = VK_FALSE,
    };
    VkSwapchainKHR swapchain;
    assert(vkCreateSwapchainKHR(device, &swapchainCreateInfo, nullptr, &swapchain) == VK_SUCCESS);
    uint32_t imageCount = 0;
    assert(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr) == VK_SUCCESS);
    std::vector<VkImage> images(imageCount);
    assert(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data()) == VK_SUCCESS);
    std::vector<VkImageView> views(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = surfaceFormat->format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1
            },
        };
        assert(vkCreateImageView(device, &viewCreateInfo, nullptr, &views[i]) == VK_SUCCESS);
    }

    ////////////////////////////////////////////////////////////////////////
    // Create full-screen quad pipeline.
    ////////////////////////////////////////////////////////////////////////

    const std::vector<uint32_t> vertexCode = loadSpv("fullscreen.vert.spv");
    const std::vector<uint32_t> fragmentCode = loadSpv("fullscreen.frag.spv");
    VkShaderModuleCreateInfo vertexModuleInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vertexCode.size() * 4,
        .pCode = vertexCode.data()
    };
    VkShaderModuleCreateInfo fragmentModuleInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fragmentCode.size() * 4,
        .pCode = fragmentCode.data()
    };
    VkShaderModule vertexModule, fragmentModule;
    assert(vkCreateShaderModule(device, &vertexModuleInfo, nullptr, &vertexModule) == VK_SUCCESS);
    assert(
        vkCreateShaderModule(device, &fragmentModuleInfo, nullptr, &fragmentModule) == VK_SUCCESS
    );
    VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertexModule,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragmentModule,
         .pName = "main"},
    };
    VkPushConstantRange pushConstantRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(uint32_t)
    };
    VkPipelineLayoutCreateInfo layoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange
    };
    VkPipelineLayout pipelineLayout;
    assert(
        vkCreatePipelineLayout(device, &layoutCreateInfo, nullptr, &pipelineLayout) == VK_SUCCESS
    );
    VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
    };
    VkPipelineViewportStateCreateInfo viewportState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1
    };
    VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineColorBlendAttachmentState blendAttachment{
        .blendEnable = VK_FALSE, .colorWriteMask = 0xf
    };
    VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blendAttachment
    };
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates
    };
    VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &surfaceFormat->format
    };
    VkGraphicsPipelineCreateInfo pipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamicState,
        .layout = pipelineLayout
    };
    VkPipeline pipeline;
    assert(
        vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &pipeline
        ) == VK_SUCCESS
    );

    ////////////////////////////////////////////////////////////////////////
    // Create command buffer and sync objects.
    ////////////////////////////////////////////////////////////////////////

    VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = *queueFamily
    };
    VkCommandPool commandPool;
    assert(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer commandBuffer;
    assert(vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer) == VK_SUCCESS);
    VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore imageAvailable;
    assert(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailable) == VK_SUCCESS);
    std::vector<VkSemaphore> renderFinished(imageCount);
    for (VkSemaphore &semaphore : renderFinished)
        assert(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore) == VK_SUCCESS);
    VkFenceCreateInfo fenceInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    VkFence fence;
    assert(vkCreateFence(device, &fenceInfo, nullptr, &fence) == VK_SUCCESS);

    uint32_t frame = 0;
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    const Clock::duration frameInterval =
        Clock::duration(1000s) / (swapchainImageCount * modeParams.refreshRate);
    Clock::time_point lastFrameTime = Clock::now();
    while (true) {
        // Pace frames so that latest present doesn't just fill up with frames
        // immediately after a display refresh.
        std::this_thread::sleep_until(lastFrameTime + frameInterval);
        lastFrameTime = Clock::now();

        assert(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
        assert(vkResetFences(device, 1, &fence) == VK_SUCCESS);
        uint32_t imageIndex;
        assert(
            vkAcquireNextImageKHR(
                device, swapchain, UINT64_MAX, imageAvailable, VK_NULL_HANDLE, &imageIndex
            ) == VK_SUCCESS
        );

        assert(vkResetCommandBuffer(commandBuffer, 0) == VK_SUCCESS);
        VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        assert(vkBeginCommandBuffer(commandBuffer, &beginInfo) == VK_SUCCESS);
        VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = images[imageIndex],
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1
            }
        };
        vkCmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier
        );
        VkRenderingAttachmentInfo attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = views[imageIndex],
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}}
        };
        VkRenderingInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {.extent = swapchainExtent},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &attachment
        };
        vkCmdBeginRendering(commandBuffer, &renderingInfo);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkViewport viewport{
            .x = 0.0f,
            .y = 0.0f,
            .width = float(swapchainExtent.width),
            .height = float(swapchainExtent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        VkRect2D scissor{
            .offset = {.x = 0, .y = 0},
            .extent = swapchainExtent,
        };
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        vkCmdPushConstants(
            commandBuffer, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(frame), &frame
        );
        vkCmdDraw(commandBuffer, 4, 1, 0, 0);
        vkCmdEndRendering(commandBuffer);
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier
        );
        assert(vkEndCommandBuffer(commandBuffer) == VK_SUCCESS);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &imageAvailable,
            .pWaitDstStageMask = &waitStage,
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBuffer,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &renderFinished[imageIndex]
        };
        assert(vkQueueSubmit(queue, 1, &submitInfo, fence) == VK_SUCCESS);
        VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &renderFinished[imageIndex],
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &imageIndex
        };
        assert(vkQueuePresentKHR(queue, &presentInfo) == VK_SUCCESS);
        ++frame;
    }
}
