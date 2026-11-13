# Granite (third_party/parallel-gs/Granite) with Device::try_next_frame_context: next_frame_context unless a thread has
# a command buffer open, in which case it returns false at once rather than waiting for that buffer's submit. The PC
# renderer's kf4-pc moved the frame context on after each picture the pump draws (pcr.cpp, Renderer::take_picture) and
# waited there for the pump's open command buffer: 32% of kf4-pc at 500 (PERFORMANCE.md, 2026-11-12). It now moves it
# on when that costs no wait, and waits only when it owes several. (A thread may still open one between the test and
# next_frame_context's own drain: then it waits as before, once.) Made in Granite's checkout at configure, as the other
# granite_*.cmake. Run after kf4_granite_dynamic_state (it defines kf4_granite_replace).

function(kf4_granite_try_frame_context granite)
    kf4_granite_replace(${granite}/vulkan/device.hpp
        "\tvoid next_frame_context();\n"
        "\tvoid next_frame_context();\n\tbool try_next_frame_context(); // (holyland) next_frame_context unless a command buffer is open\n")
    kf4_granite_replace(${granite}/vulkan/device.cpp
        "void Device::next_frame_context()\n{\n\tDRAIN_FRAME_LOCK();\n"
        "bool Device::try_next_frame_context() // (holyland)\n{\n\t{\n\t\tstd::lock_guard<std::mutex> holder{lock.lock};\n\t\tif (lock.counter != 0)\n\t\t\treturn false;\n\t}\n\tnext_frame_context();\n\treturn true;\n}\n\nvoid Device::next_frame_context()\n{\n\tDRAIN_FRAME_LOCK();\n")
endfunction()
