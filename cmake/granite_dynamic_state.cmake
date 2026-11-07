# Granite (third_party/parallel-gs/Granite) with Vulkan's dynamic state for the PC renderer's draws (pcr_canvas.cpp):
# depth test, write, compare, depth bounds test, stencil test and ops, culling, front face, topology (within its
# class) and blend constants (Vulkan 1.3's extended dynamic state); blend enable, the blend equation and the colour
# write mask where the device has VK_EXT_extended_dynamic_state3's. The tie rule's Z-first and colour draws differ in
# just that state: as pipeline state each such draw bound a pipeline of its own (a replay at the pond: 1,205 binds for
# 1,213 draws). The code is cmake/granite_dynamic_state.inc, included into command_buffer.cpp; the edits here are
# the calls into it, made as cmake/granite_depth_bounds.cmake makes its own (at configure, once, beside an anchor
# line of Granite's, failing if the anchor is gone). Run after kf4_granite_depth_bounds: it builds on its state bit.

# (a replacement: `old` is Granite's text, made `new` once; a checkout that has `new` is left alone)
function(kf4_granite_replace file old new)
    file(READ ${file} text)
    string(FIND "${text}" "\r\n" crlf)
    if(NOT crlf EQUAL -1) # (the file's own line endings)
        string(REPLACE "\n" "\r\n" old "${old}")
        string(REPLACE "\n" "\r\n" new "${new}")
    endif()
    string(FIND "${text}" "${new}" have)
    if(NOT have EQUAL -1)
        return()
    endif()
    string(FIND "${text}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "granite_dynamic_state.cmake: '${old}' is not in ${file} (Granite moved on?)")
    endif()
    string(REPLACE "${old}" "${new}" text "${text}")
    file(WRITE ${file} "${text}")
endfunction()

function(kf4_granite_dynamic_state granite)
    set(hpp ${granite}/vulkan/command_buffer.hpp)
    set(cpp ${granite}/vulkan/command_buffer.cpp)
    set(ctx ${granite}/vulkan/context.cpp)
    set(ctx_hpp ${granite}/vulkan/context.hpp)
    # the state bits (out of the padding), their setter, and what the command buffer last set
    kf4_granite_replace(${hpp} "uint32_t depth_bounds_test : 1; uint32_t padding : 6;"
        "uint32_t depth_bounds_test : 1; uint32_t dynamic_ds : 1; uint32_t dynamic_blend : 1; uint32_t padding : 4;")
    kf4_granite_edit(${hpp} "\tinline void set_color_write_mask(uint32_t write_mask)"
        "\tinline void set_dynamic_state(bool dynamic_ds, bool dynamic_blend) { SET_STATIC_STATE(dynamic_ds); SET_STATIC_STATE(dynamic_blend); } // (holyland)" TRUE)
    kf4_granite_edit(${hpp} "\tvoid bind_pipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline, uint32_t active_dynamic_state);"
        "\tstruct { bool valid; uint32_t ds[4]; float constants[4]; uint32_t count; VkBool32 enable[VULKAN_NUM_ATTACHMENTS]; VkColorComponentFlags mask[VULKAN_NUM_ATTACHMENTS]; VkColorBlendEquationEXT eq[VULKAN_NUM_ATTACHMENTS]; } holyland_emitted = {}; void holyland_flush_dynamic(); VkPipeline holyland_bound = VK_NULL_HANDLE; bool holyland_bind(VkPipelineBindPoint bind_point, VkPipeline pipeline, uint32_t active_dynamic_state); // (holyland)" TRUE)
    # the code, and the calls into it: the hash without the dynamic state, the pipeline made with it, set before a draw
    kf4_granite_edit(${cpp} "Pipeline CommandBuffer::build_graphics_pipeline(Device *device, const DeferredPipelineCompile &compile,"
        "#include \"../../../../cmake/granite_dynamic_state.inc\" // (holyland)" TRUE)
    kf4_granite_replace(${cpp} "\th.data(compile.static_state.words, sizeof(compile.static_state.words));"
        "\tconst PipelineState holyland_hashed = holyland_hash_state(compile.static_state); // (holyland)\n\th.data(holyland_hashed.words, sizeof(holyland_hashed.words));")
    kf4_granite_replace(${cpp} "\tif (compile.static_state.state.blend_enable)\n"
        "\tif (compile.static_state.state.blend_enable && !compile.static_state.state.dynamic_ds) // (holyland)\n")
    kf4_granite_replace(${cpp} "VkDynamicState states[8] = {" "VkDynamicState states[32] = {")
    kf4_granite_edit(${cpp} "\t// Blend state"
        "\tif (compile.static_state.state.dynamic_ds) dynamic_mask |= holyland_dynamic_states(*device, compile.static_state, states, dyn.dynamicStateCount); // (holyland)" TRUE)
    kf4_granite_edit(${cpp} "\tuint32_t update_vbo_mask = dirty_vbos & active_vbos;"
        "\tif (pipeline_state.static_state.state.dynamic_ds) holyland_flush_dynamic(); // (holyland)" TRUE)
    kf4_granite_edit(${cpp} "\ttable.vkCmdBindPipeline(cmd, bind_point, pipeline);"
        "\tif (holyland_bind(bind_point, pipeline, active_dynamic_state)) // (holyland: not the graphics pipeline bound already)" TRUE)
    kf4_granite_edit(${cpp} "\t                        pipeline_info.initialPipeline);"
        "\tholyland_bound = VK_NULL_HANDLE; // (holyland)" FALSE)
    # (the bits kept where Granite clears the render state: set_opaque_state and the like)
    kf4_granite_edit(${cpp} "\t// Preserve spec constant mask."
        "\tconst bool holyland_keep[2] = { pipeline_state.static_state.state.dynamic_ds != 0, pipeline_state.static_state.state.dynamic_blend != 0 }; // (holyland)" TRUE)
    kf4_granite_edit(${cpp} "\tmemset(&state, 0, sizeof(state));"
        "\tstate.dynamic_ds = holyland_keep[0]; state.dynamic_blend = holyland_keep[1]; // (holyland)" FALSE)
    # VK_EXT_extended_dynamic_state3 and its features, where the device has it
    kf4_granite_edit(${ctx_hpp} "\tVkPhysicalDeviceMaintenance5FeaturesKHR maintenance5_features = {};"
        "\tVkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3_features = {}; // (holyland)" FALSE)
    kf4_granite_edit(${ctx} "\tif (has_extension(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME))"
        "\tif (has_extension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)) { enabled_extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME); ADD_CHAIN(ext.eds3_features, EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT); } // (holyland)" TRUE)
endfunction()
