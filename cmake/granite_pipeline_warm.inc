// (holyland) Included into Granite's vulkan/command_buffer.cpp, in namespace Vulkan, by cmake/granite_pipeline_warm.cmake:
// the hooks a pipeline list uses.

// a pipeline Granite made (sync: a draw or dispatch waited for it; ok: it was made), and a compatible render pass made
// for the first time (any thread)
void (*holyland_on_compile)(Device *device, const DeferredPipelineCompile &compile, bool compute, int64_t ns, bool sync,
                            bool ok) = nullptr;
void (*holyland_on_render_pass)(Util::Hash hash, const RenderPassInfo &info) = nullptr;

// the hash Granite looks a pipeline up by, from what it is made from
void CommandBuffer::holyland_rehash(DeferredPipelineCompile &compile, bool compute)
{
	if (compute)
		update_hash_compute_pipeline(compile);
	else
		update_hash_graphics_pipeline(compile, nullptr);
}

// the pipeline cache a command buffer makes its pipelines with
VkPipelineCache CommandBuffer::holyland_pipeline_cache(Device *device)
{
	return device->legacy_pipeline_cache;
}

// a compatible render pass (a pipeline list makes them again before a draw asks for them)
const RenderPass &CommandBuffer::holyland_render_pass(Device *device, const RenderPassInfo &info)
{
	return device->request_render_pass(info, true);
}
