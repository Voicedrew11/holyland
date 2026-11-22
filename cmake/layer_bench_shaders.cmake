# kf4-layerbench's shaders (tools/layer_bench), compiled to SPIR-V arrays in OUT (CMakeLists.txt runs this):
# glslangValidator names each file it reads, so its output is shown only when one does not compile.
#   cmake -DGLSLANG=<glslangValidator> -DSRC=<tools/layer_bench> -DOUT=<dir> -P layer_bench_shaders.cmake
function(layer_shader name source)
    execute_process(COMMAND ${GLSLANG} -V ${ARGN} --vn ${name} -o ${OUT}/${name}.inc ${SRC}/${source}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name} (${source}) did not compile:\n${output}")
    endif()
endfunction()

layer_shader(layer_vert layer.vert)
layer_shader(layer_frag_fill layer.frag)
layer_shader(layer_frag_textured layer.frag -DTEXTURED)
layer_shader(layer_frag_discards layer.frag -DTEXTURED -DDISCARDS)
layer_shader(layer_frag_own_depth layer.frag -DTEXTURED -DDISCARDS -DOWN_DEPTH)
layer_shader(layer_frag_mask_one layer.frag -DTEXTURED -DMASK_ONE)
layer_shader(layer_frag_mask_all layer.frag -DTEXTURED -DMASK_ALL)
