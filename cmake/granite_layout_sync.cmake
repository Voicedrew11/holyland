# Granite (third_party/parallel-gs/Granite) with an image's first layout transition made visible to what follows.
# Device::create_image, for an image with an initial layout and no initial data (the PC renderer's surfaces and
# copies, made GENERAL), records the transition in a command buffer of its own with VK_PIPELINE_STAGE_NONE as the
# barrier's second scope. Nothing after it can then wait for the transition (a later barrier's first scope does not
# take in a layout transition whose own second scope is empty), so the first clear or copy into the image races
# with it: Vulkan's synchronization validation names it (SYNC-HAZARD-WRITE-AFTER-WRITE, vkCmdClearColorImage after
# the transition; found 2026-11-11). Drivers order a queue's work anyway, which is why nothing showed. The second
# scope is made every command and every memory access. Made in Granite's checkout at configure, as the other
# granite_*.cmake (a checkout that has it is left alone).

function(kf4_granite_layout_sync granite)
    set(cpp ${granite}/vulkan/device.cpp)
    file(READ ${cpp} text)
    string(FIND "${text}" "// (holyland) the transition visible to what follows" have)
    if(NOT have EQUAL -1)
        return()
    endif()
    set(old "\t\tcmd->image_barrier(*handle, info.initialLayout, create_info.initial_layout,\n\t\t                   VK_PIPELINE_STAGE_NONE, 0,\n\t\t                   VK_PIPELINE_STAGE_NONE, 0);")
    set(new "\t\tcmd->image_barrier(*handle, info.initialLayout, create_info.initial_layout,\n\t\t                   VK_PIPELINE_STAGE_NONE, 0,\n\t\t                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT); // (holyland) the transition visible to what follows")
    string(FIND "${text}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "granite_layout_sync.cmake: create_image's initial transition is not in ${cpp} (Granite moved on?)")
    endif()
    string(REPLACE "${old}" "${new}" text "${text}")
    file(WRITE ${cpp} "${text}")
endfunction()
