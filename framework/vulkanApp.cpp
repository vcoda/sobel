#include <algorithm>
#include <iostream>
#include <sstream>
#include "vulkanApp.h"
#include "linearAllocator.h"

VulkanApp::VulkanApp(const AppEntry& entry, const std::tstring& caption, uint32_t width, uint32_t height,
    bool depthBuffer /* false */):
    PlatformApp(entry, caption, width, height),
    timer(std::make_unique<Timer>()),
    depthBuffer(depthBuffer)
{
    magma::Object::setAllocator(std::make_shared<LinearAllocator>());
}

VulkanApp::~VulkanApp()
{}

void VulkanApp::onIdle()
{
	onPaint();
}

void VulkanApp::onPaint()
{
    const uint32_t bufferIndex = swapchain->acquireNextImage(presentFinished, nullptr);
    waitFences[bufferIndex]->wait();
    waitFences[bufferIndex]->reset();
    {
        render(bufferIndex);
    }
    queue->present(swapchain, bufferIndex, renderFinished);
    device->waitIdle(); // Flush
}

void VulkanApp::onKeyDown(char key, int repeat, uint32_t flags)
{
	PlatformApp::onKeyDown(key, repeat, flags);
}

void VulkanApp::initialize()
{
    createInstance();
    createLogicalDevice();
    createSwapchain(false);
    createRenderPass();
    createFramebuffer();
    createCommandBuffers();
    createSyncPrimitives();
    pipelineCache = std::make_shared<magma::PipelineCache>(device);
}

static VkBool32 VKAPI_PTR reportCallback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType,
    uint64_t object, size_t location, int32_t messageCode,
    const char *pLayerPrefix, const char *pMessage, void *pUserData);

void VulkanApp::createInstance()
{
    const std::vector<const char *> layerNames = {
#ifdef _DEBUG
        "VK_LAYER_LUNARG_standard_validation"
#endif
    };
    const std::vector<const char *> extensionNames = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
#elif defined(VK_USE_PLATFORM_XCB_KHR)
        VK_KHR_XCB_SURFACE_EXTENSION_NAME,
#endif // VK_USE_PLATFORM_XCB_KHR
#ifdef _DEBUG
        VK_EXT_DEBUG_REPORT_EXTENSION_NAME
#endif
    };
    MAGMA_STACK_ARRAY(char, appName, caption.length() + 1);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    size_t count = 0;
    wcstombs_s(&count, appName, appName.size(), caption.c_str(), appName.size());
#else
    strcpy(appName, caption.c_str());
#endif

    instance = std::make_shared<magma::Instance>(
        appName,
        "Magma",
        VK_API_VERSION_1_0,
        layerNames, extensionNames);

    debugReportCallback = std::make_unique<magma::DebugReportCallback>(
        instance,
        reportCallback);

    physicalDevice = instance->getPhysicalDevice(0);
    const VkPhysicalDeviceProperties& properties = physicalDevice->getProperties();
    std::cout << "Running on " << properties.deviceName << "\n";

    instanceExtensions = std::make_unique<magma::InstanceExtensions>();
    extensions = std::make_unique<magma::PhysicalDeviceExtensions>(physicalDevice);
}

void VulkanApp::createLogicalDevice()
{
    const std::vector<float> defaultQueuePriorities = {1.0f};
    const magma::DeviceQueueDescriptor graphicsQueue(VK_QUEUE_GRAPHICS_BIT, physicalDevice, defaultQueuePriorities);
    const magma::DeviceQueueDescriptor transferQueue(VK_QUEUE_TRANSFER_BIT, physicalDevice, defaultQueuePriorities);
    std::vector<magma::DeviceQueueDescriptor> queueDescriptors;
    queueDescriptors.push_back(graphicsQueue);
    if (transferQueue.queueFamilyIndex != graphicsQueue.queueFamilyIndex)
        queueDescriptors.push_back(transferQueue);

    // Enable some widely used features
    VkPhysicalDeviceFeatures features = {0};
    features.fillModeNonSolid = VK_TRUE;
    features.samplerAnisotropy = VK_TRUE;
    features.textureCompressionBC = VK_TRUE;
    features.occlusionQueryPrecise = VK_TRUE;

    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (extensions->AMD_negative_viewport_height)
        enabledExtensions.push_back(VK_AMD_NEGATIVE_VIEWPORT_HEIGHT_EXTENSION_NAME);
    else if (extensions->KHR_maintenance1)
        enabledExtensions.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);

    const std::vector<const char*> noLayers;
    device = physicalDevice->createDevice(queueDescriptors, noLayers, enabledExtensions, features);
}

void VulkanApp::createSwapchain(bool vSync)
{
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    surface = std::make_shared<magma::Win32Surface>(instance, hInstance, hWnd);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    surface = std::make_shared<magma::XlibSurface>(instance, dpy, window);
#elif defined(VK_USE_PLATFORM_XCB_KHR)
    surface = std::make_shared<magma::XcbSurface>(instance, connection, window);
#endif // VK_USE_PLATFORM_XCB_KHR
    if (!physicalDevice->getSurfaceSupport(surface))
        throw std::runtime_error("surface not supported");
    // Get surface caps
    VkSurfaceCapabilitiesKHR surfaceCaps;
	surfaceCaps = physicalDevice->getSurfaceCapabilities(surface);
    assert(surfaceCaps.currentExtent.width == width);
    assert(surfaceCaps.currentExtent.height == height);
    // Find supported transform flags
    VkSurfaceTransformFlagBitsKHR preTransform;
    if (surfaceCaps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    else
        preTransform = surfaceCaps.currentTransform;
    // Find supported alpha composite
    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    VkCompositeAlphaFlagBitsKHR compositeAlphaFlags[] =
    {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (auto alphaFlag : compositeAlphaFlags)
    {
        if (surfaceCaps.supportedCompositeAlpha & alphaFlag)
        {
            compositeAlpha = alphaFlag;
            break;
        }
    }
	const std::vector<VkSurfaceFormatKHR> surfaceFormats = physicalDevice->getSurfaceFormats(surface);
    // Choose available present mode
    const std::vector<VkPresentModeKHR> presentModes = physicalDevice->getSurfacePresentModes(surface);
    VkPresentModeKHR presentMode;
    if (vSync)
        presentMode = VK_PRESENT_MODE_FIFO_KHR;
    else
    {   // Search for first appropriate present mode
        const VkPresentModeKHR modes[] =
        {
            VK_PRESENT_MODE_IMMEDIATE_KHR,  // AMD
            VK_PRESENT_MODE_MAILBOX_KHR,    // NVidia, Intel
            VK_PRESENT_MODE_FIFO_RELAXED_KHR
        };
        bool found = false;
        for (auto mode : modes)
        {
            if (std::find(presentModes.begin(), presentModes.end(), mode)
                != presentModes.end())
            {
                presentMode = mode;
                found = true;
                break;
            }
        }
        if (!found)
        {   // Must always be present
            presentMode = VK_PRESENT_MODE_FIFO_KHR;
        }
    }
    swapchain = std::make_shared<magma::Swapchain>(device, surface,
        std::min(2U, surfaceCaps.maxImageCount),
        surfaceFormats[0], surfaceCaps.currentExtent,
        preTransform, compositeAlpha, presentMode);
}

void VulkanApp::createRenderPass()
{
    const std::vector<VkSurfaceFormatKHR> surfaceFormats = physicalDevice->getSurfaceFormats(surface);
    const magma::AttachmentDescription colorAttachment(surfaceFormats[0].format, 1,
        magma::op::clearStore, // Clear color, store
        magma::op::dontCareDontCare, // Stencil don't care
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (depthBuffer)
    {
        const VkFormat depthFormat = getSupportedDepthFormat(false, true);
        const magma::AttachmentDescription depthStencilAttachment(depthFormat, 1,
            magma::op::clearStore, // Clear depth, store
            magma::op::clearDontCare, // Stencil don't care
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        const std::initializer_list<magma::AttachmentDescription> attachments = {colorAttachment, depthStencilAttachment};
        renderPass = std::make_shared<magma::RenderPass>(device, attachments);
    }
    else
    {
        renderPass = std::make_shared<magma::RenderPass>(device, colorAttachment);
    }
}

void VulkanApp::createFramebuffer()
{
    const VkSurfaceCapabilitiesKHR surfaceCaps = physicalDevice->getSurfaceCapabilities(surface);
    if (depthBuffer)
    {
        const VkFormat depthFormat = getSupportedDepthFormat(false, true);
        depthStencil = std::make_shared<magma::DepthStencilAttachment2D>(device, depthFormat, surfaceCaps.currentExtent, 1, 1);
        depthStencilView = std::make_shared<magma::ImageView>(depthStencil);
    }
    for (const auto& image : swapchain->getImages())
    {
        std::vector<std::shared_ptr<const magma::ImageView>> attachments;
        std::shared_ptr<magma::ImageView> colorView(std::make_shared<magma::ImageView>(image));
        attachments.push_back(colorView);
        if (depthBuffer)
            attachments.push_back(depthStencilView);
        std::shared_ptr<magma::Framebuffer> framebuffer(std::make_shared<magma::Framebuffer>(renderPass, attachments));
        framebuffers.push_back(framebuffer);
    }
}

void VulkanApp::createCommandBuffers()
{
    queue = device->getQueue(VK_QUEUE_GRAPHICS_BIT, 0);
    commandPools[0] = std::make_shared<magma::CommandPool>(device, queue->getFamilyIndex());
    // Create draw command buffers
    commandBuffers = commandPools[0]->allocateCommandBuffers(static_cast<uint32_t>(framebuffers.size()), true);
    // Create image copy command buffer
    cmdImageCopy = commandPools[0]->allocateCommandBuffer(true);
    try
    {
        std::shared_ptr<magma::Queue> transferQueue = device->getQueue(VK_QUEUE_TRANSFER_BIT, 0);
        commandPools[1] = std::make_shared<magma::CommandPool>(device, transferQueue->getFamilyIndex());
        // Create buffer copy command buffer
        cmdBufferCopy = commandPools[1]->allocateCommandBuffer(true);
    }
    catch (...)
    {
        std::cout << "Transfer queue not present\n";
    }
}

void VulkanApp::createSyncPrimitives()
{
    presentFinished = std::make_shared<magma::Semaphore>(device);
    renderFinished = std::make_shared<magma::Semaphore>(device);
    for (int i = 0; i < (int)commandBuffers.size(); ++i)
    {
        constexpr bool signaled = true; // Don't wait on first render of each command buffer
        waitFences.push_back(std::make_shared<magma::Fence>(device, signaled));
    }
}

bool VulkanApp::submitCmdBuffer(uint32_t bufferIndex)
{
    return queue->submit(commandBuffers[bufferIndex],
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        presentFinished,
        renderFinished,
        waitFences[bufferIndex]);
}

VkFormat VulkanApp::getSupportedDepthFormat(bool hasStencil, bool optimalTiling) const
{
    const std::vector<VkFormat> depthFormats = {
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM,
    };
    for (VkFormat format : depthFormats)
    {
        if (hasStencil && !magma::Format(format).depthStencil())
            continue;
        const VkFormatProperties properties = physicalDevice->getFormatProperties(format);
        if (optimalTiling && (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            return format;
        else if (!optimalTiling && (properties.linearTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            return format;
    }
    return VK_FORMAT_UNDEFINED;
}

static VkBool32 VKAPI_PTR reportCallback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType,
    uint64_t object, size_t location, int32_t messageCode,
    const char *pLayerPrefix, const char *pMessage, void *pUserData)
{
    if (strstr(pMessage, "Extension"))
        return VK_FALSE;
    std::stringstream msg;
    msg << "[" << pLayerPrefix << "] " << pMessage << "\n";
    if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT)
        std::cerr << msg.str();
    else
        std::cout << msg.str();
    return VK_FALSE;
}
