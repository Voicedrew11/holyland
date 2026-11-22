# Khronos's synchronization2 layer (third_party/vk-sync2-layer), built with the game into vk_layers/ beside kf4: the
# layer's library and its manifest. The game enables it only on a driver without VK_KHR_synchronization2
# (port/gs/pc/pcr_sync2.h). Built with Granite's Vulkan headers; the target is kf4_vk_sync2_layer.
#
#   kf4_vk_sync2_layer(VULKAN_HEADERS_INCLUDE_DIR)
function(kf4_vk_sync2_layer headers)
    set(src ${CMAKE_SOURCE_DIR}/third_party/vk-sync2-layer)
    set(ext ${src}/extension-layer)
    set(vul ${src}/utility-libraries)
    set(out ${CMAKE_BINARY_DIR}/vk_layers)

    add_library(kf4_vk_sync2_layer MODULE
        ${ext}/layers/synchronization2.cpp
        ${ext}/utils/allocator.cpp
        ${vul}/src/layer/layer_settings_manager.cpp
        ${vul}/src/layer/layer_settings_util.cpp
        ${vul}/src/layer/vk_layer_settings.cpp
        ${vul}/src/layer/vk_layer_settings_helper.cpp
        ${vul}/src/vulkan/vk_safe_struct_core.cpp
        ${vul}/src/vulkan/vk_safe_struct_ext.cpp
        ${vul}/src/vulkan/vk_safe_struct_khr.cpp
        ${vul}/src/vulkan/vk_safe_struct_manual.cpp
        ${vul}/src/vulkan/vk_safe_struct_utils.cpp
        ${vul}/src/vulkan/vk_safe_struct_vendor.cpp)
    target_include_directories(kf4_vk_sync2_layer PRIVATE ${headers} ${vul}/include ${vul}/src/layer ${ext}/layers
                               ${ext}/utils)
    set_target_properties(kf4_vk_sync2_layer PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
        OUTPUT_NAME VkLayer_khronos_synchronization2
        LIBRARY_OUTPUT_DIRECTORY ${out})
    # (third-party code: its warnings are not ours to fix)
    target_compile_options(kf4_vk_sync2_layer PRIVATE -w -fno-exceptions)
    if(TARGET Threads::Threads)
        target_link_libraries(kf4_vk_sync2_layer PRIVATE Threads::Threads)
    endif()
    if(WIN32)
        set(library VkLayer_khronos_synchronization2.dll)
        set_target_properties(kf4_vk_sync2_layer PROPERTIES PREFIX "" SUFFIX ".dll")
        target_compile_definitions(kf4_vk_sync2_layer PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
        # only the loader's entry points are exported (the .def), as upstream
        target_sources(kf4_vk_sync2_layer PRIVATE ${ext}/layers/VkLayer_khronos_synchronization2.def)
        set(JSON_LIBRARY_PATH ".\\\\${library}")
    else()
        set(library libVkLayer_khronos_synchronization2.so)
        set_target_properties(kf4_vk_sync2_layer PROPERTIES PREFIX "lib" SUFFIX ".so")
        target_link_options(kf4_vk_sync2_layer PRIVATE
            LINKER:--version-script=${ext}/layers/libVkLayer_khronos_synchronization2.map,-Bsymbolic,--exclude-libs,ALL)
        set(JSON_LIBRARY_PATH "./${library}")
    endif()

    # the manifest's api_version: the headers' (VK_HEADER_VERSION_COMPLETE's 1.4 and VK_HEADER_VERSION)
    file(STRINGS ${headers}/vulkan/vulkan_core.h header_line REGEX "^#define VK_HEADER_VERSION [0-9]+")
    string(REGEX MATCH "[0-9]+$" header_version "${header_line}")
    file(STRINGS ${headers}/vulkan/vulkan_core.h complete_line REGEX "^#define VK_HEADER_VERSION_COMPLETE ")
    string(REGEX MATCH "\\(0, ([0-9]+), ([0-9]+), VK_HEADER_VERSION\\)" _ "${complete_line}")
    if(NOT header_version OR NOT CMAKE_MATCH_1)
        message(FATAL_ERROR "vk_sync2_layer.cmake: no VK_HEADER_VERSION in ${headers}/vulkan/vulkan_core.h")
    endif()
    set(JSON_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${header_version}")
    configure_file(${ext}/layers/json/VkLayer_khronos_synchronization2.json.in
                   ${out}/VkLayer_khronos_synchronization2.json @ONLY)
endfunction()
