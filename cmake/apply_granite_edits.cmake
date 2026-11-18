# Granite's edits without configuring the game (cmake/granite_edits.cmake): for packaging/payload.sh, where no
# compiler is set up yet.
#   cmake -DGRANITE=<third_party/parallel-gs/Granite> -P cmake/apply_granite_edits.cmake
if(NOT GRANITE)
    message(FATAL_ERROR "usage: cmake -DGRANITE=<Granite folder> -P cmake/apply_granite_edits.cmake")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/granite_edits.cmake)
kf4_granite_edits(${GRANITE})
