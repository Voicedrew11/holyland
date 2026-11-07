# Granite (third_party/parallel-gs/Granite) with the depth bounds test: a pipeline state bit, depth_bounds_test, that turns the test on with its bounds dynamic (set
# with vkCmdSetDepthBounds before each draw), and the device feature turned on where the GPU has it.
# Granite is a submodule of a submodule, so the change is made here, at configure, in its checkout: each
# edit is a line added beside one of Granite's, made once (a checkout that has it is left alone), with the
# file's own line endings. A fresh clone and `git submodule update` get it at the next configure; the
# submodule then shows as modified.

function(kf4_granite_edit file anchor added before)
    file(READ ${file} text)
    string(FIND "${text}" "${added}" have)
    if(NOT have EQUAL -1)
        return()
    endif()
    string(FIND "${text}" "${anchor}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "granite_depth_bounds.cmake: '${anchor}' is not in ${file} (Granite moved on?)")
    endif()
    string(FIND "${text}" "\r\n" crlf)
    if(crlf EQUAL -1)
        set(eol "\n")
    else()
        set(eol "\r\n")
    endif()
    if(before)
        string(REPLACE "${anchor}" "${added}${eol}${anchor}" text "${text}")
    else()
        string(REPLACE "${anchor}" "${anchor}${eol}${added}" text "${text}")
    endif()
    file(WRITE ${file} "${text}")
endfunction()

function(kf4_granite_depth_bounds granite)
    set(hpp ${granite}/vulkan/command_buffer.hpp)
    set(cpp ${granite}/vulkan/command_buffer.cpp)
    set(ctx ${granite}/vulkan/context.cpp)
    # the state bit, out of the padding
    file(READ ${hpp} text)
    string(FIND "${text}" "uint32_t depth_bounds_test : 1;" have)
    if(have EQUAL -1)
        string(REPLACE "uint32_t padding : 7;" "uint32_t depth_bounds_test : 1; uint32_t padding : 6;" text "${text}")
        file(WRITE ${hpp} "${text}")
    endif()
    kf4_granite_edit(${hpp} "\tinline void set_color_write_mask(uint32_t write_mask)"
        "\tinline void set_depth_bounds_test(bool depth_bounds_test) { SET_STATIC_STATE(depth_bounds_test); } // (holyland)" TRUE)
    # the pipeline: the test on, its bounds dynamic
    file(READ ${cpp} text)
    string(FIND "${text}" "VkDynamicState states[8] = {" have)
    if(have EQUAL -1)
        string(REPLACE "VkDynamicState states[7] = {" "VkDynamicState states[8] = {" text "${text}")
        file(WRITE ${cpp} "${text}")
    endif()
    kf4_granite_edit(${cpp} "\tif (compile.static_state.state.stencil_test)"
        "\tif (compile.static_state.state.depth_bounds_test) states[dyn.dynamicStateCount++] = VK_DYNAMIC_STATE_DEPTH_BOUNDS; // (holyland)" TRUE)
    kf4_granite_edit(${cpp} "\tds.depthWriteEnable = "
        "\tds.depthBoundsTestEnable = compile.compatible_render_pass->has_depth(compile.subpass_index) && compile.static_state.state.depth_bounds_test != 0; // (holyland)" TRUE)
    # the device feature
    kf4_granite_edit(${ctx} "\t\tif (pdf2.features.multiDrawIndirect)"
        "\t\tif (pdf2.features.depthBounds) enabled_features.depthBounds = VK_TRUE; // (holyland)" TRUE)
endfunction()
