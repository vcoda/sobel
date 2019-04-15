#include "../framework/vulkanApp.h"
#include "../framework/bezierMesh.h"
#include "teapot.h"

class SobelApp : public VulkanApp
{
    struct Framebuffer
    {
        std::shared_ptr<magma::ColorAttachment2D> color;
        std::shared_ptr<magma::ImageView> colorView;
        std::shared_ptr<magma::RenderPass> renderPass;
        std::shared_ptr<magma::Framebuffer> framebuffer;
    } fb;

    std::shared_ptr<magma::CommandBuffer> rtCmdBuffer;
    std::shared_ptr<magma::Semaphore> rtSemaphore;
    std::shared_ptr<magma::GraphicsPipeline> rtSolidDrawPipeline;
    std::vector<magma::PipelineShaderStage> rtShaderStages;

    std::unique_ptr<BezierPatchMesh> mesh;
    std::unique_ptr<magma::aux::BlitRectangle> blitRect;

    std::shared_ptr<magma::UniformBuffer<rapid::matrix>> uniformBuffer;
    std::shared_ptr<magma::DescriptorPool> descriptorPool;
    std::shared_ptr<magma::DescriptorSetLayout> descriptorSetLayout;
    std::shared_ptr<magma::DescriptorSet> descriptorSet;
    std::shared_ptr<magma::PipelineLayout> pipelineLayout;

    rapid::matrix viewProj;
    bool negateViewport = false;
    constexpr static uint32_t fbSize = 256;

public:
    SobelApp(const AppEntry& entry):
        VulkanApp(entry, TEXT("Sobel"), 512, 512, true)
    {
        initialize();

        // https://stackoverflow.com/questions/48036410/why-doesnt-vulkan-use-the-standard-cartesian-coordinate-system
        negateViewport = extensions->KHR_maintenance1 || extensions->AMD_negative_viewport_height;

        setupView();
        createMesh();
        createFramebuffer({fbSize, fbSize});
        createUniformBuffer();
        setupDescriptorSet();
        setupPipelines();
        recordRenderToTextureCommandBuffer();
        recordCommandBuffer(FrontBuffer);
        recordCommandBuffer(BackBuffer);
        timer->run();
    }

    virtual void render(uint32_t bufferIndex) override
    {
        updatePerspectiveTransform();
        queue->submit(
            rtCmdBuffer,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            presentFinished, // Wait for swapchain
            rtSemaphore,
            nullptr);

        queue->submit(
            commandBuffers[bufferIndex],
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            rtSemaphore, // Wait for render-to-texture
            renderFinished,
            waitFences[bufferIndex]);
    }

    void setupView()
    {
        const rapid::vector3 eye(0.f, 3.f, 8.f);
        const rapid::vector3 center(0.f, 2.f, 0.f);
        const rapid::vector3 up(0.f, 1.f, 0.f);
        const float fov = rapid::radians(60.f);
        const float aspect = width/(float)height;
        const float zn = 1.f, zf = 100.f;
        const rapid::matrix view = rapid::lookAtRH(eye, center, up);
        const rapid::matrix proj = rapid::perspectiveFovRH(fov, aspect, zn, zf);
        viewProj = view * proj;
    }

    void updatePerspectiveTransform()
    {
        const float speed = 0.05f;
        static float angle = 0.f;
        angle += timer->millisecondsElapsed() * speed;
        const rapid::matrix world = rapid::rotationY(rapid::radians(angle));
        magma::helpers::mapScoped<rapid::matrix>(uniformBuffer, true, [this, &world](auto *worldViewProj)
        {
            *worldViewProj = world * viewProj;
        });
    }

    void createMesh()
    {
        const uint32_t subdivisionDegree = 8;
        mesh = std::make_unique<BezierPatchMesh>(teapotPatches, kTeapotNumPatches, teapotVertices, subdivisionDegree, cmdBufferCopy);
    }

    void createFramebuffer(const VkExtent2D& extent)
    {
        fb.color = std::make_shared<magma::ColorAttachment2D>(device, VK_FORMAT_R8G8B8A8_UNORM, extent, 1, 1);
        fb.colorView = std::make_shared<magma::ImageView>(fb.color);

        // Make sure that we are fit to hardware limits
        const VkImageFormatProperties formatProperties = physicalDevice->getImageFormatProperties(
            fb.color->getFormat(), VK_IMAGE_TYPE_2D, true, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        const magma::AttachmentDescription colorAttachment(fb.color->getFormat(), 1,
            magma::attachments::colorClearStoreReadOnly);
        fb.renderPass = std::make_shared<magma::RenderPass>(device, colorAttachment);
        fb.framebuffer = std::make_shared<magma::Framebuffer>(fb.renderPass, fb.colorView);
    }

    void createUniformBuffer()
    {
        uniformBuffer = std::make_shared<magma::UniformBuffer<rapid::matrix>>(device);
    }

    void setupDescriptorSet()
    {   // Create descriptor pool
        constexpr uint32_t maxDescriptorSets = 1; // One set is enough for us
        const magma::Descriptor uniformBufferDesc = magma::descriptors::UniformBuffer(1);
        descriptorPool = std::make_shared<magma::DescriptorPool>(device, maxDescriptorSets,
            std::vector<magma::Descriptor>
            {   // Allocate simply one uniform buffer
                uniformBufferDesc
            });
        // Setup descriptor set layout:
        // Here we describe that slot 0 in vertex shader will have uniform buffer binding
        descriptorSetLayout = std::make_shared<magma::DescriptorSetLayout>(device,
            std::initializer_list<magma::DescriptorSetLayout::Binding>{
                magma::bindings::VertexStageBinding(0, uniformBufferDesc)
            });
        // Connect our uniform buffer to binding point
        descriptorSet = descriptorPool->allocateDescriptorSet(descriptorSetLayout);
        descriptorSet->update(0, uniformBuffer);
    }

    void setupPipelines()
    {
        pipelineLayout = std::make_shared<magma::PipelineLayout>(descriptorSetLayout);
        rtSolidDrawPipeline = std::make_shared<magma::GraphicsPipeline>(device, pipelineCache,
            std::vector<magma::PipelineShaderStage>
            {
                VertexShader(device, "transform.o"),
                FragmentShader(device, "fill.o")
            },
            mesh->getVertexInput(),
            magma::renderstates::triangleList,
            negateViewport ? magma::renderstates::fillCullBackCW : magma::renderstates::fillCullBackCCW,
            magma::renderstates::noMultisample,
            magma::renderstates::depthLessOrEqual,
            magma::renderstates::dontBlendWriteRGB,
            std::initializer_list<VkDynamicState>{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR},
            pipelineLayout,
            fb.renderPass);

        blitRect = std::make_unique<magma::aux::BlitRectangle>(renderPass,
            VertexShader(device, "quad.o"),
            FragmentShader(device, "sobel.o"));
    }

    void recordRenderToTextureCommandBuffer()
    {
        rtCmdBuffer = commandPools[0]->allocateCommandBuffer(true);
        rtSemaphore = std::make_shared<magma::Semaphore>(device);

        rtCmdBuffer->begin();
        {
            rtCmdBuffer->setRenderArea(0, 0, fb.framebuffer->getExtent());
            rtCmdBuffer->beginRenderPass(fb.renderPass, fb.framebuffer, {magma::clears::blackColor});
            {
                const uint32_t width = fb.framebuffer->getExtent().width;
                const uint32_t height = fb.framebuffer->getExtent().height;
                rtCmdBuffer->setViewport(0, 0, width, negateViewport ? -height : height);
                rtCmdBuffer->setScissor(magma::Scissor(0, 0, fb.framebuffer->getExtent()));
                rtCmdBuffer->bindDescriptorSet(pipelineLayout, descriptorSet);
                rtCmdBuffer->bindPipeline(rtSolidDrawPipeline);
                mesh->draw(rtCmdBuffer);
            }
            rtCmdBuffer->endRenderPass();
        }
        rtCmdBuffer->end();
    }

    void recordCommandBuffer(uint32_t index)
    {
        std::shared_ptr<magma::CommandBuffer> cmdBuffer = commandBuffers[index];
        cmdBuffer->begin();
        {
            blitRect->blit(framebuffers[index], fb.colorView, cmdBuffer);
        }
        cmdBuffer->end();
    }
};

std::unique_ptr<IApplication> appFactory(const AppEntry& entry)
{
    return std::make_unique<SobelApp>(entry);
}
