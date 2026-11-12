# Granite (third_party/parallel-gs/Granite) with a uniform or storage buffer bound again unchanged left alone. Without
# descriptor buffers or heaps (the PC renderer's device: flags 0), CommandBuffer::set_uniform_buffer and
# set_storage_buffer marked their set for a new descriptor set even when the same buffer, offset and range were bound
# there already (the descriptor buffer and heap paths return early; set_texture does too). The PC renderer binds the
# pictures between's buffers, a native batch's three and the CLUT's empty buffer at every batch, so every batch
# allocated and wrote up to three descriptor sets (PERFORMANCE.md, issue #110: flush_descriptor_sets 36% of the pump).
# Made in Granite's checkout at configure, as the other granite_*.cmake. Run after kf4_granite_dynamic_state (it
# defines kf4_granite_replace).

function(kf4_granite_descriptor_reuse granite)
    set(cpp ${granite}/vulkan/command_buffer.cpp)
    kf4_granite_replace(${cpp}
        "\telse if (buffer.get_cookie() != bindings.cookies[set][binding] ||\n\t         b.buffer.range != range || b.buffer.offset != offset)\n\t{\n\t\tb.buffer = { buffer.get_buffer(), offset, range };\n\t}\n"
        "\telse if (buffer.get_cookie() != bindings.cookies[set][binding] ||\n\t         b.buffer.range != range || b.buffer.offset != offset)\n\t{\n\t\tb.buffer = { buffer.get_buffer(), offset, range };\n\t}\n\telse\n\t\treturn; // (holyland) the same uniform buffer bound again: the set is still good\n")
    kf4_granite_replace(${cpp}
        "\telse if (buffer.get_cookie() != bindings.cookies[set][binding] ||\n\t         b.buffer.offset != offset || b.buffer.range != range)\n\t{\n\t\tb.buffer = { buffer.get_buffer(), offset, range };\n\t}\n"
        "\telse if (buffer.get_cookie() != bindings.cookies[set][binding] ||\n\t         b.buffer.offset != offset || b.buffer.range != range)\n\t{\n\t\tb.buffer = { buffer.get_buffer(), offset, range };\n\t}\n\telse\n\t\treturn; // (holyland) the same storage buffer bound again: the set is still good\n")
endfunction()
