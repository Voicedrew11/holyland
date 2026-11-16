# Granite (third_party/parallel-gs/Granite) with Device::wait_next_frame_context_idle: waits, without Granite's device
# lock, for the GPU to finish the work of the frame context next_frame_context comes round to. next_frame_context
# itself waits for that work holding the lock (DRAIN_FRAME_LOCK, then PerFrame::begin), so every other thread that
# asks Granite for anything that takes the lock (a buffer made or let go, a command buffer requested or submitted)
# waits for the GPU too: the PC renderer's pump ("kf4-pc-pump", pcr.cpp) did, 2.4 ms at a time at map 009, starting its
# picture between late. The PC renderer's kf4-pc calls this just before next_frame_context, which then finds that
# work done. The context waited for is closed (nothing is submitted into it until it is current again, which only
# next_frame_context makes it), so its timeline values read under the lock stay the ones next_frame_context waits
# for. False when the wait timed out or could not be made (no timeline semaphores): next_frame_context waits as before.
# Made in Granite's checkout at configure, as the other granite_*.cmake. Run after kf4_granite_try_frame_context.

function(kf4_granite_frame_context_wait granite)
    kf4_granite_replace(${granite}/vulkan/device.hpp
        "\tbool try_next_frame_context(); // (holyland) next_frame_context unless a command buffer is open\n"
        "\tbool try_next_frame_context(); // (holyland) next_frame_context unless a command buffer is open\n\tbool wait_next_frame_context_idle(uint64_t timeout_ns); // (holyland) the GPU's work of the context next_frame_context begins, waited for without the device lock\n")
    kf4_granite_replace(${granite}/vulkan/device.cpp
        "bool Device::try_next_frame_context() // (holyland)\n"
        "bool Device::wait_next_frame_context_idle(uint64_t timeout_ns) // (holyland)\n{\n\tVkSemaphore sems[QUEUE_INDEX_COUNT];\n\tuint64_t values[QUEUE_INDEX_COUNT];\n\tuint32_t count = 0;\n\t{\n\t\tstd::lock_guard<std::mutex> holder{lock.lock};\n\t\tif (!get_device_features().vk12_features.timelineSemaphore)\n\t\t\treturn false;\n\t\tuint32_t next_context = frame_context_index + 1;\n\t\tif (next_context >= per_frame.size())\n\t\t\tnext_context = 0;\n\t\tconst PerFrame &next = *per_frame[next_context];\n\t\tfor (int i = 0; i < QUEUE_INDEX_COUNT; i++)\n\t\t{\n\t\t\tif (next.timeline_semaphores[i] == VK_NULL_HANDLE)\n\t\t\t\treturn false;\n\t\t\tif (next.timeline_fences[i])\n\t\t\t{\n\t\t\t\tsems[count] = next.timeline_semaphores[i];\n\t\t\t\tvalues[count] = next.timeline_fences[i];\n\t\t\t\tcount++;\n\t\t\t}\n\t\t}\n\t}\n\tif (count == 0)\n\t\treturn true;\n\tVkSemaphoreWaitInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };\n\tinfo.semaphoreCount = count;\n\tinfo.pSemaphores = sems;\n\tinfo.pValues = values;\n\treturn table->vkWaitSemaphores(device, &info, timeout_ns) == VK_SUCCESS;\n}\n\nbool Device::try_next_frame_context() // (holyland)\n")
endfunction()
