# SPDX-License-Identifier: GPL-3.0-or-later
execute_process(COMMAND "${PROGRAM}" --exact-boundary RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT status EQUAL 1 OR NOT err MATCHES "authored exact-boundary V4-32 expected value")
    message(FATAL_ERROR "Old-offset control did not fail the intended value assertion: ${status}; ${out}; ${err}")
endif()
message(STATUS "Old-offset control detected the authored exact-boundary V4-32 defect")
