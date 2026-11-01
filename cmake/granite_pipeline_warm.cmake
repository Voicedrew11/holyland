# Granite (third_party/parallel-gs/Granite) with the hooks the native GS's pipeline list needs (gsn_pipeline_warm.inc,
# docs/port/RENDERER.md "Pipelines made ahead"): a pipeline made by Granite is told to the native GS (what it was made
# from, so that it can be made again at the next start before a draw needs it), a compatible render pass made for
# the first time is told too (its attachments' formats and samples, which a pipeline is made against), and the
# pipeline's hash can be worked out again from outside a command buffer. The code is cmake/granite_pipeline_warm.inc,
# included into command_buffer.cpp; the edits are made as cmake/granite_dynamic_state.cmake makes its own (at
# configure, once, beside an anchor line of Granite's). Run after kf4_granite_dynamic_state.
function(kf4_granite_pipeline_warm granite)
    set(hpp ${granite}/vulkan/command_buffer.hpp)
    set(cpp ${granite}/vulkan/command_buffer.cpp)
    set(dev ${granite}/vulkan/device.cpp)
    kf4_granite_edit(${hpp} "\tstatic Pipeline build_compute_pipeline(Device *device, const DeferredPipelineCompile &compile, CompileMode mode);"
        "\tstatic void holyland_rehash(DeferredPipelineCompile &compile, bool compute); static VkPipelineCache holyland_pipeline_cache(Device *device); static const RenderPass &holyland_render_pass(Device *device, const RenderPassInfo &info); // (holyland)" FALSE)
    kf4_granite_edit(${cpp} "static void log_compile_time(const char *tag, Hash hash,"
        "#include \"../../../../cmake/granite_pipeline_warm.inc\" // (holyland)" TRUE)
    kf4_granite_edit(${cpp} "\tlog_compile_time(\"compute\", compile.hash, end_ts - start_ts, vr, mode);"
        "\tif (holyland_on_compile) holyland_on_compile(device, compile, true, end_ts - start_ts, mode == CompileMode::Sync, vr == VK_SUCCESS && compute_pipeline != VK_NULL_HANDLE); // (holyland)" FALSE)
    kf4_granite_edit(${cpp} "\tlog_compile_time(\"graphics\", compile.hash, end_ts - start_ts, res, mode);"
        "\tif (holyland_on_compile) holyland_on_compile(device, compile, false, end_ts - start_ts, mode == CompileMode::Sync, res == VK_SUCCESS && pipeline != VK_NULL_HANDLE); // (holyland)" FALSE)
    kf4_granite_edit(${dev} "const RenderPass &Device::request_render_pass(const RenderPassInfo &info, bool compatible)"
        "extern void (*holyland_on_render_pass)(Util::Hash hash, const RenderPassInfo &info); // (holyland: cmake/granite_pipeline_warm.inc)" TRUE)
    kf4_granite_replace(${dev} "\t\tret = render_passes.emplace_yield(hash, hash, this, info);\n"
        "\t{\n\t\tret = render_passes.emplace_yield(hash, hash, this, info);\n\t\tif (compatible && holyland_on_render_pass) holyland_on_render_pass(hash, info); // (holyland)\n\t}\n")
endfunction()

# Granite's pipeline binaries (VK_KHR_pipeline_binary, RADV's way) are serialized for the pipeline cache's file by
# walking its maps without their locks, while a draw or the warm-up may be adding a pipeline to them on another thread.
# The native GS writes the file on a thread of its own (gsn.cpp, "The pipeline cache"), so the walk holds the maps'
# read locks: a pipeline made meanwhile waits for it (a few milliseconds), and is in the next file.
function(kf4_granite_pipeline_cache_locks granite)
    set(map ${granite}/util/intrusive_hash_map.hpp)
    set(cache ${granite}/vulkan/pipeline_cache.cpp)
    kf4_granite_edit(${map} "\t// Not supposed to be called in racy conditions,"
        "\tRWSpinLock &holyland_lock() const { return lock; } // (holyland: cmake/granite_pipeline_warm.cmake)" TRUE)
    kf4_granite_edit(${cache} "\t// Granite's magic UUID."
        "\tUtil::RWSpinLockReadHolder holyland_sized(binary_mapping.holyland_lock()), holyland_sized_binaries(binaries.holyland_lock()); // (holyland)" TRUE)
    kf4_granite_edit(${cache} "\tif (size < get_serialized_size())"
        "\tUtil::RWSpinLockReadHolder holyland_serialized(binary_mapping.holyland_lock()), holyland_serialized_binaries(binaries.holyland_lock()); // (holyland)" TRUE)
endfunction()
