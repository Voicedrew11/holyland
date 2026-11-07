// (holyland) Granite's command buffer with Vulkan's dynamic state for depth, stencil, culling, topology and blend
// constants (Vulkan 1.3's extended dynamic state) and, where the device has VK_EXT_extended_dynamic_state3's, for
// blend enable, the blend equation and the colour write mask. Included into vulkan/command_buffer.cpp (inside
// namespace Vulkan) by cmake/granite_dynamic_state.cmake, which also adds the calls into Granite's own code.
//
// A command buffer whose static state has `dynamic_ds` (and `dynamic_blend`) set (set_dynamic_state) makes its pipelines with that state dynamic and looks them up by a hash without it:
// draws that differ only there share one pipeline, and each draw sets what differs (holyland_flush_dynamic) instead of
// binding another.
// The rest of Granite is as it was: a command buffer without the bits makes and binds its pipelines as before.

std::atomic<uint64_t> holyland_pipeline_binds{0}; // graphics pipelines bound (--stats)

enum { HOLYLAND_DYNAMIC_BIT = 1u << 31 }; // (a pipeline's dynamic_mask: made with the state below dynamic)

// Primitive topology's class (Vulkan's dynamic topology may change only within one without
// dynamicPrimitiveTopologyUnrestricted): the pipeline is looked up by the class
static uint32_t holyland_topology_class(uint32_t topology)
{
	switch (topology)
	{
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		return 0;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
		return 1;
	case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
		return 3;
	default:
		return 2;
	}
}

// The static state a pipeline is looked up by: the dynamic state left out
static PipelineState holyland_hash_state(const PipelineState &in)
{
	PipelineState s = in;
	if (s.state.dynamic_ds)
	{
		s.state.depth_write = 0;
		s.state.depth_test = 0;
		s.state.depth_compare = 0;
		s.state.depth_bounds_test = 0;
		s.state.stencil_test = 0;
		s.state.stencil_front_fail = 0;
		s.state.stencil_front_pass = 0;
		s.state.stencil_front_depth_fail = 0;
		s.state.stencil_front_compare_op = 0;
		s.state.stencil_back_fail = 0;
		s.state.stencil_back_pass = 0;
		s.state.stencil_back_depth_fail = 0;
		s.state.stencil_back_compare_op = 0;
		s.state.cull_mode = 0;
		s.state.front_face = 0;
		s.state.topology = holyland_topology_class(in.state.topology);
	}
	if (s.state.dynamic_ds && s.state.dynamic_blend)
	{
		s.state.blend_enable = 0;
		s.state.src_color_blend = 0;
		s.state.dst_color_blend = 0;
		s.state.color_blend_op = 0;
		s.state.src_alpha_blend = 0;
		s.state.dst_alpha_blend = 0;
		s.state.alpha_blend_op = 0;
		s.state.write_mask = 0;
	}
	return s;
}

// The pipeline's dynamic states, those not in states[] yet added (Granite's own, from the state it is made with, may
// be some of them); returns the bits for its dynamic_mask
static uint32_t holyland_dynamic_states(const Device &device, const PipelineState &s, VkDynamicState *states,
                                        uint32_t &count)
{
	auto add = [&](VkDynamicState state) {
		for (uint32_t i = 0; i < count; i++)
			if (states[i] == state)
				return;
		states[count++] = state;
	};
	add(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE);
	add(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE);
	add(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP);
	if (device.get_device_features().enabled_features.depthBounds)
	{
		add(VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE);
		add(VK_DYNAMIC_STATE_DEPTH_BOUNDS);
	}
	add(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE);
	add(VK_DYNAMIC_STATE_STENCIL_OP);
	add(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK);
	add(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
	add(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
	add(VK_DYNAMIC_STATE_CULL_MODE);
	add(VK_DYNAMIC_STATE_FRONT_FACE);
	add(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY);
	add(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	if (s.state.dynamic_blend)
	{
		add(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
		add(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
		add(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
	}
	return HOLYLAND_DYNAMIC_BIT | COMMAND_BUFFER_DIRTY_STENCIL_REFERENCE_BIT;
}

// A pipeline about to be bound: whether it is to be (bind_pipeline). A command buffer with dynamic state does not bind
// the graphics pipeline it has bound already (Granite binds again after a render pass begins, or a program is set,
// whichever pipeline it then finds). And a pipeline without the dynamic state makes Vulkan's dynamic state unknown.
bool CommandBuffer::holyland_bind(VkPipelineBindPoint bind_point, VkPipeline pipeline, uint32_t active_dynamic_state)
{
	if (bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS)
		return true;
	if (pipeline_state.static_state.state.dynamic_ds && pipeline == holyland_bound)
		return false;
	holyland_bound = pipeline;
	holyland_pipeline_binds.fetch_add(1, std::memory_order_relaxed);
	if (!(active_dynamic_state & HOLYLAND_DYNAMIC_BIT))
		holyland_emitted.valid = false;
	return true;
}

// Before a draw: the dynamic state the draw's differs in from what was set last (all of it after a pipeline bound
// without it), as Granite would have made it into the pipeline (build_graphics_pipeline: depth and stencil only with
// such an attachment, a colour attachment's blending and writes only where it is used and the program writes it)
void CommandBuffer::holyland_flush_dynamic()
{
	auto &s = pipeline_state.static_state.state;
	auto &e = holyland_emitted;
	const RenderPass *rp = pipeline_state.compatible_render_pass;
	unsigned sub = pipeline_state.subpass_index;
	bool depth = rp->has_depth(sub), stencil = rp->has_stencil(sub);
	bool all = !e.valid;

	uint32_t ds[4];
	ds[0] = (depth && s.depth_test) | (depth && s.depth_write) << 1 |
	        (depth && s.depth_test ? s.depth_compare : 0) << 2 | (depth && s.depth_bounds_test) << 5 |
	        (stencil && s.stencil_test) << 6 | s.cull_mode << 7 | s.front_face << 9 | s.topology << 10;
	ds[1] = s.stencil_test ? (s.stencil_front_fail | s.stencil_front_pass << 3 | s.stencil_front_depth_fail << 6 |
	                          s.stencil_front_compare_op << 9) : 0;
	ds[2] = s.stencil_test ? (s.stencil_back_fail | s.stencil_back_pass << 3 | s.stencil_back_depth_fail << 6 |
	                          s.stencil_back_compare_op << 9) : 0;
	ds[3] = 0;
	uint32_t changed = all ? ~0u : ds[0] ^ e.ds[0];
	if (changed & 1)
		table.vkCmdSetDepthTestEnable(cmd, ds[0] & 1);
	if (changed & 2)
		table.vkCmdSetDepthWriteEnable(cmd, (ds[0] >> 1) & 1);
	if (changed & (7u << 2))
		table.vkCmdSetDepthCompareOp(cmd, VkCompareOp((ds[0] >> 2) & 7));
	if ((changed & (1u << 5)) && device->get_device_features().enabled_features.depthBounds)
		table.vkCmdSetDepthBoundsTestEnable(cmd, (ds[0] >> 5) & 1);
	if (changed & (1u << 6))
		table.vkCmdSetStencilTestEnable(cmd, (ds[0] >> 6) & 1);
	if (changed & (3u << 7))
		table.vkCmdSetCullMode(cmd, VkCullModeFlags((ds[0] >> 7) & 3));
	if (changed & (1u << 9))
		table.vkCmdSetFrontFace(cmd, VkFrontFace((ds[0] >> 9) & 1));
	if (changed & (15u << 10))
		table.vkCmdSetPrimitiveTopology(cmd, VkPrimitiveTopology((ds[0] >> 10) & 15));
	for (unsigned face = 0; face < 2; face++)
	{
		if (all || ds[1 + face] != e.ds[1 + face])
		{
			uint32_t v = ds[1 + face];
			table.vkCmdSetStencilOp(cmd, face ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT,
			                        VkStencilOp(v & 7), VkStencilOp((v >> 3) & 7), VkStencilOp((v >> 6) & 7),
			                        VkCompareOp((v >> 9) & 7));
		}
	}
	memcpy(e.ds, ds, sizeof(ds));

	if (all || memcmp(e.constants, pipeline_state.potential_static_state.blend_constants, sizeof(e.constants)) != 0)
	{
		memcpy(e.constants, pipeline_state.potential_static_state.blend_constants, sizeof(e.constants));
		table.vkCmdSetBlendConstants(cmd, e.constants);
	}

	if (s.dynamic_blend)
	{
		uint32_t n = rp->get_num_color_attachments(sub);
		uint32_t rt_mask = pipeline_state.layout->get_resource_layout().render_target_mask;
		VkBool32 enable[VULKAN_NUM_ATTACHMENTS];
		VkColorComponentFlags mask[VULKAN_NUM_ATTACHMENTS];
		VkColorBlendEquationEXT eq[VULKAN_NUM_ATTACHMENTS];
		for (uint32_t i = 0; i < n; i++)
		{
			bool used = rp->get_color_attachment(sub, i).attachment != VK_ATTACHMENT_UNUSED && (rt_mask & (1u << i));
			enable[i] = used && s.blend_enable;
			mask[i] = used ? (s.write_mask >> (4 * i)) & 0xf : 0;
			eq[i] = {};
			if (enable[i])
			{
				eq[i].srcColorBlendFactor = VkBlendFactor(s.src_color_blend);
				eq[i].dstColorBlendFactor = VkBlendFactor(s.dst_color_blend);
				eq[i].colorBlendOp = VkBlendOp(s.color_blend_op);
				eq[i].srcAlphaBlendFactor = VkBlendFactor(s.src_alpha_blend);
				eq[i].dstAlphaBlendFactor = VkBlendFactor(s.dst_alpha_blend);
				eq[i].alphaBlendOp = VkBlendOp(s.alpha_blend_op);
			}
		}
		bool count_changed = all || n != e.count;
		if (n && (count_changed || memcmp(enable, e.enable, n * sizeof(enable[0])) != 0))
			table.vkCmdSetColorBlendEnableEXT(cmd, 0, n, enable);
		if (n && (count_changed || memcmp(mask, e.mask, n * sizeof(mask[0])) != 0))
			table.vkCmdSetColorWriteMaskEXT(cmd, 0, n, mask);
		if (n && (count_changed || memcmp(eq, e.eq, n * sizeof(eq[0])) != 0))
			table.vkCmdSetColorBlendEquationEXT(cmd, 0, n, eq);
		memcpy(e.enable, enable, n * sizeof(enable[0]));
		memcpy(e.mask, mask, n * sizeof(mask[0]));
		memcpy(e.eq, eq, n * sizeof(eq[0]));
		e.count = n;
	}
	else
		e.count = ~0u;
	e.valid = true;
}
