# Granite (third_party/parallel-gs/Granite) with VK_EXT_fragment_shader_interlock and its features turned on where the
# device has it, for the PC renderer's depth rule (port/gs/pc/shaders/pcr_prim.h's PCR_DEPTH_RULE): the depth tested
# in the fragment shader against each depth surface's own copy of it, read and written in the shader's critical
# section, which runs in the triangles' order at a pixel. Made as cmake/granite_depth_bounds.cmake makes its edits
# (kf4_granite_edit). Run after kf4_granite_dynamic_state.
function(kf4_granite_interlock granite)
    set(ctx_hpp ${granite}/vulkan/context.hpp)
    set(ctx ${granite}/vulkan/context.cpp)
    kf4_granite_edit(${ctx_hpp} "\tVkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3_features = {}; // (holyland)"
        "\tVkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock_features = {}; // (holyland)" FALSE)
    kf4_granite_edit(${ctx} "\tif (has_extension(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME))"
        "\tif (has_extension(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME)) { enabled_extensions.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME); ADD_CHAIN(ext.interlock_features, FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT); } // (holyland)" TRUE)
endfunction()
