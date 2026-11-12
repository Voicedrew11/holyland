# Granite (third_party/parallel-gs/Granite) with a render pass's samples in its hash. Device::request_render_pass hashes
# the attachments' formats but not their samples, so a multisampled pass was given the single-sampled pass of the same
# formats made before it (or the other way round), and its pipelines the wrong sample count. The PC renderer draws
# into both (MSAA: a surface's multisampled image, and the passes that are not), so each attachment's samples are
# hashed too. Made as cmake/granite_depth_bounds.cmake makes its edits (kf4_granite_edit). Run after
# kf4_granite_pipeline_warm.
function(kf4_granite_msaa granite)
    set(dev ${granite}/vulkan/device.cpp)
    kf4_granite_edit(${dev} "\t\th.u32(info.color_attachments[i]->get_image().get_swapchain_layout());"
        "\t\th.u32(info.color_attachments[i]->get_image().get_create_info().samples); // (holyland: cmake/granite_msaa.cmake)" FALSE)
    kf4_granite_edit(${dev} "\t\tif (info.depth_stencil->get_image().get_create_info().domain == ImageDomain::Transient)"
        "\t\th.u32(info.depth_stencil->get_image().get_create_info().samples); // (holyland: cmake/granite_msaa.cmake)" TRUE)
endfunction()
