#include "graphics/host_gpu/renderer/image/blitHelper.h"

#include "common/assert.h"
#include "gpu_blit_shaders/gpu_blit_atomic_shadow_r16_to_r32_2d_spv.h"
#include "gpu_blit_shaders/gpu_blit_atomic_shadow_r16_to_r32_3d_spv.h"
#include "gpu_blit_shaders/gpu_blit_atomic_shadow_r32_to_r16_2d_spv.h"
#include "gpu_blit_shaders/gpu_blit_atomic_shadow_r32_to_r16_3d_spv.h"
#include "gpu_blit_shaders/gpu_blit_atomic_shadow_r32_to_r8_2d_spv.h"
#include "gpu_blit_shaders/gpu_blit_atomic_shadow_r32_to_r8_3d_spv.h"
#include "gpu_blit_shaders/gpu_blit_atomic_shadow_r8_to_r32_2d_spv.h"
#include "gpu_blit_shaders/gpu_blit_atomic_shadow_r8_to_r32_3d_spv.h"
#include "gpu_blit_shaders/gpu_blit_color_to_ms_depth_spv.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/renderTarget.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics {

BlitHelper::BlitHelper(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler) {
	vk::DescriptorSetLayoutBinding texture_binding {};
	texture_binding.binding         = 0;
	texture_binding.descriptorType  = vk::DescriptorType::eSampledImage;
	texture_binding.descriptorCount = 1;
	texture_binding.stageFlags      = vk::ShaderStageFlagBits::eFragment;

	vk::DescriptorSetLayoutCreateInfo descriptor_info {};
	descriptor_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	descriptor_info.bindingCount = 1;
	descriptor_info.pBindings    = &texture_binding;
	RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr,
	                                                                 &m_descriptor_layout),
	                     "create BlitHelper descriptor layout");

	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts    = &m_descriptor_layout;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&layout_info, nullptr, &m_pipeline_layout),
	    "create BlitHelper pipeline layout");

	m_vertex_shader   = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, m_graphics.device);
	m_fragment_shader = CompileSPV(GPU_BLIT_COLOR_TO_MS_DEPTH_SPV, m_graphics.device);

	std::array<vk::DescriptorSetLayoutBinding, 2> shadow_bindings {};
	for (uint32_t i = 0; i < shadow_bindings.size(); ++i) {
		shadow_bindings[i].binding         = i;
		shadow_bindings[i].descriptorType  = vk::DescriptorType::eStorageImage;
		shadow_bindings[i].descriptorCount = 1;
		shadow_bindings[i].stageFlags      = vk::ShaderStageFlagBits::eCompute;
	}
	vk::DescriptorSetLayoutCreateInfo shadow_desc_info {};
	shadow_desc_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	shadow_desc_info.bindingCount = static_cast<uint32_t>(shadow_bindings.size());
	shadow_desc_info.pBindings    = shadow_bindings.data();
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&shadow_desc_info, nullptr,
	                                                &m_atomic_shadow_descriptor_layout),
	    "create BlitHelper atomic shadow descriptor layout");

	vk::PushConstantRange push_range {};
	push_range.stageFlags = vk::ShaderStageFlagBits::eCompute;
	push_range.offset     = 0;
	push_range.size       = sizeof(uint32_t) * 3;
	vk::PipelineLayoutCreateInfo shadow_layout_info {};
	shadow_layout_info.setLayoutCount         = 1;
	shadow_layout_info.pSetLayouts            = &m_atomic_shadow_descriptor_layout;
	shadow_layout_info.pushConstantRangeCount = 1;
	shadow_layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&shadow_layout_info, nullptr,
	                                           &m_atomic_shadow_pipeline_layout),
	    "create BlitHelper atomic shadow pipeline layout");
}

BlitHelper::~BlitHelper() {
	for (const auto& pipeline: m_atomic_shadow_pipelines) {
		if (pipeline != nullptr) {
			m_graphics.device.destroyPipeline(pipeline, nullptr);
		}
	}
	m_graphics.device.destroyPipelineLayout(m_atomic_shadow_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_atomic_shadow_descriptor_layout, nullptr);
	for (const auto& pipeline: m_pipelines) {
		m_graphics.device.destroyPipeline(pipeline.handle, nullptr);
	}
	m_graphics.device.destroyShaderModule(m_fragment_shader, nullptr);
	m_graphics.device.destroyShaderModule(m_vertex_shader, nullptr);
	m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
}

vk::Pipeline BlitHelper::GetPipeline(PipelineKey key) {
	const auto cached = std::ranges::find(m_pipelines, key, &Pipeline::key);
	if (cached != m_pipelines.end()) {
		return cached->handle;
	}

	const auto samples = vulkan_sample_count(key.samples);
	EXIT_IF(samples == vk::SampleCountFlagBits {} || key.format == vk::Format::eUndefined);

	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
	stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
	stages[0].module = m_vertex_shader;
	stages[0].pName  = "main";
	stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
	stages[1].module = m_fragment_shader;
	stages[1].pName  = "main";

	vk::PipelineVertexInputStateCreateInfo vertex_input {};
	vk::PipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
	vk::PipelineViewportStateCreateInfo viewport {};
	viewport.viewportCount = 1;
	viewport.scissorCount  = 1;
	vk::PipelineRasterizationStateCreateInfo rasterization {};
	rasterization.lineWidth = 1.0f;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	multisample.rasterizationSamples = samples;
	vk::PipelineDepthStencilStateCreateInfo depth {};
	depth.depthTestEnable  = VK_TRUE;
	depth.depthWriteEnable = VK_TRUE;
	depth.depthCompareOp   = vk::CompareOp::eAlways;
	vk::PipelineColorBlendStateCreateInfo color_blend {};
	const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic.pDynamicStates    = dynamic_states.data();

	vk::PipelineRenderingCreateInfo rendering {};
	rendering.depthAttachmentFormat = key.format;

	vk::GraphicsPipelineCreateInfo create {};
	create.pNext               = &rendering;
	create.stageCount          = static_cast<uint32_t>(stages.size());
	create.pStages             = stages.data();
	create.pVertexInputState   = &vertex_input;
	create.pInputAssemblyState = &input_assembly;
	create.pViewportState      = &viewport;
	create.pRasterizationState = &rasterization;
	create.pMultisampleState   = &multisample;
	create.pDepthStencilState  = &depth;
	create.pColorBlendState    = &color_blend;
	create.pDynamicState       = &dynamic;
	create.layout              = m_pipeline_layout;

	vk::Pipeline pipeline = nullptr;
	RequireVulkanSuccess(
	    m_graphics.device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &pipeline),
	    "create color-to-MS-depth pipeline");
	m_pipelines.push_back({key, pipeline});
	return pipeline;
}

void BlitHelper::ReinterpretColorAsMsDepth(Image& source, Image& destination) {
	const auto& source_info      = source.info;
	const auto& destination_info = destination.info;
	EXIT_IF(DepthAspectTransferFormat(source_info.pixel_format) != vk::Format::eUndefined ||
	        DepthAspectTransferFormat(destination_info.pixel_format) == vk::Format::eUndefined ||
	        source_info.samples != 1 || destination_info.samples <= 1 ||
	        destination_info.samples > 4 || source.backing.image_type != vk::ImageType::e2D ||
	        destination.backing.image_type != vk::ImageType::e2D ||
	        source_info.extent.width != destination_info.extent.width ||
	        source_info.extent.height != destination_info.extent.height ||
	        source_info.extent.depth != 1 || destination_info.extent.depth != 1 ||
	        source.backing.image == nullptr || destination.backing.image == nullptr);
	m_scheduler.EndRendering();

	ImageViewInfo source_view_info {};
	source_view_info.format = source_info.pixel_format;
	source_view_info.type   = vk::ImageViewType::e2D;
	source_view_info.aspect = vk::ImageAspectFlagBits::eColor;
	source_view_info.usage  = vk::ImageUsageFlagBits::eSampled;
	const auto source_view  = source.FindView(source_view_info);

	ImageViewInfo destination_view_info {};
	destination_view_info.format = destination_info.pixel_format;
	destination_view_info.type   = vk::ImageViewType::e2D;
	destination_view_info.aspect = vk::ImageAspectFlagBits::eDepth;
	destination_view_info.usage  = vk::ImageUsageFlagBits::eDepthStencilAttachment;
	const auto destination_view  = destination.FindView(destination_view_info);

	auto& command_buffer = m_scheduler.Current();
	auto  command        = command_buffer.Handle();
	source.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {},
	               command);
	destination.Transit(ColorToMsDepthLayout, vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {},
	                    command);

	vk::RenderingAttachmentInfo depth_attachment {};
	depth_attachment.imageView               = destination_view;
	depth_attachment.imageLayout             = ColorToMsDepthLayout;
	depth_attachment.loadOp                  = vk::AttachmentLoadOp::eClear;
	depth_attachment.storeOp                 = vk::AttachmentStoreOp::eStore;
	depth_attachment.clearValue.depthStencil = {0.0f, 0};

	vk::RenderingInfo rendering {};
	rendering.renderArea.extent = {destination_info.extent.width, destination_info.extent.height};
	rendering.layerCount        = 1;
	rendering.pDepthAttachment  = &depth_attachment;
	command.beginRendering(&rendering);

	vk::DescriptorImageInfo descriptor_image {};
	descriptor_image.imageView   = source_view;
	descriptor_image.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	vk::WriteDescriptorSet descriptor_write {};
	descriptor_write.dstBinding      = 0;
	descriptor_write.descriptorCount = 1;
	descriptor_write.descriptorType  = vk::DescriptorType::eSampledImage;
	descriptor_write.pImageInfo      = &descriptor_image;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_pipeline_layout, 0, 1,
	                             &descriptor_write);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics,
	                     GetPipeline({destination_info.samples, destination_info.pixel_format}));

	const vk::Viewport viewport {0.0f,
	                             0.0f,
	                             static_cast<float>(destination_info.extent.width),
	                             static_cast<float>(destination_info.extent.height),
	                             0.0f,
	                             1.0f};
	const vk::Rect2D   scissor {{0, 0},
	                            {destination_info.extent.width, destination_info.extent.height}};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
	// The helper pipeline has static state that invalidates the guest draw's dynamic state.
	m_scheduler.GetDynamicState().Invalidate();
}

vk::Pipeline BlitHelper::GetAtomicShadowPipeline(uint32_t bits, bool to_shadow, bool is_3d) {
	EXIT_IF(bits != 8u && bits != 16u);
	const size_t slot =
	    (bits == 16u ? 4u : 0u) | (to_shadow ? 0u : 2u) | (is_3d ? 1u : 0u);
	if (m_atomic_shadow_pipelines[slot] != nullptr) {
		return m_atomic_shadow_pipelines[slot];
	}
	std::span<const uint32_t> spv;
	switch (slot) {
		case 0: spv = GPU_BLIT_ATOMIC_SHADOW_R8_TO_R32_2D_SPV; break;
		case 1: spv = GPU_BLIT_ATOMIC_SHADOW_R8_TO_R32_3D_SPV; break;
		case 2: spv = GPU_BLIT_ATOMIC_SHADOW_R32_TO_R8_2D_SPV; break;
		case 3: spv = GPU_BLIT_ATOMIC_SHADOW_R32_TO_R8_3D_SPV; break;
		case 4: spv = GPU_BLIT_ATOMIC_SHADOW_R16_TO_R32_2D_SPV; break;
		case 5: spv = GPU_BLIT_ATOMIC_SHADOW_R16_TO_R32_3D_SPV; break;
		case 6: spv = GPU_BLIT_ATOMIC_SHADOW_R32_TO_R16_2D_SPV; break;
		case 7: spv = GPU_BLIT_ATOMIC_SHADOW_R32_TO_R16_3D_SPV; break;
		default: EXIT("invalid atomic shadow pipeline slot %zu\n", slot);
	}
	const auto shader = CompileSPV(spv, m_graphics.device);
	vk::ComputePipelineCreateInfo create {};
	create.stage.stage  = vk::ShaderStageFlagBits::eCompute;
	create.stage.module = shader;
	create.stage.pName  = "main";
	create.layout       = m_atomic_shadow_pipeline_layout;
	vk::Pipeline pipeline = nullptr;
	RequireVulkanSuccess(
	    m_graphics.device.createComputePipelines(nullptr, 1, &create, nullptr, &pipeline),
	    "create atomic shadow copy pipeline");
	m_graphics.device.destroyShaderModule(shader, nullptr);
	m_atomic_shadow_pipelines[slot] = pipeline;
	return pipeline;
}

void BlitHelper::CopyAtomicShadow(vk::ImageView source, vk::ImageView destination,
                                  vk::Extent3D extent, uint32_t bits, bool to_shadow, bool is_3d) {
	EXIT_IF(source == nullptr || destination == nullptr || extent.width == 0 ||
	        extent.height == 0 || extent.depth == 0);
	auto command = m_scheduler.Current().Handle();
	const std::array<vk::DescriptorImageInfo, 2> infos {
	    vk::DescriptorImageInfo {nullptr, source, vk::ImageLayout::eGeneral},
	    vk::DescriptorImageInfo {nullptr, destination, vk::ImageLayout::eGeneral},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t i = 0; i < writes.size(); ++i) {
		writes[i].dstBinding      = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eStorageImage;
		writes[i].pImageInfo      = &infos[i];
	}
	command.bindPipeline(vk::PipelineBindPoint::eCompute,
	                     GetAtomicShadowPipeline(bits, to_shadow, is_3d));
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_atomic_shadow_pipeline_layout,
	                             0, static_cast<uint32_t>(writes.size()), writes.data());
	const std::array<uint32_t, 3> params {extent.width, extent.height, extent.depth};
	command.pushConstants(m_atomic_shadow_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
	                      static_cast<uint32_t>(sizeof(params)), params.data());
	command.dispatch((extent.width + 7u) / 8u, (extent.height + 7u) / 8u, extent.depth);
}

} // namespace Libs::Graphics
