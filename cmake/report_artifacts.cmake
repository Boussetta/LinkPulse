if(NOT DEFINED LP_ARTIFACTS)
  message(FATAL_ERROR "LP_ARTIFACTS must contain at least one executable path")
endif()
if(NOT DEFINED LP_VERSION)
  set(LP_VERSION "unknown")
endif()
if(NOT DEFINED LP_GIT_DESCRIPTION)
  set(LP_GIT_DESCRIPTION "v${LP_VERSION}")
endif()

find_program(LP_SIZE_TOOL
  NAMES llvm-size size
  HINTS
    "C:/Program Files/LLVM/bin"
    "C:/Program Files (x86)/LLVM/bin")

message(STATUS "")
message(STATUS "LinkPulse build artifacts")
message(STATUS "  Version: ${LP_VERSION}")
message(STATUS "  Git description: ${LP_GIT_DESCRIPTION}")
message(STATUS "  Size tool: ${LP_SIZE_TOOL}")

foreach(artifact IN LISTS LP_ARTIFACTS)
  if(NOT EXISTS "${artifact}")
    message(WARNING "  Missing artifact: ${artifact}")
    continue()
  endif()

  file(SIZE "${artifact}" artifact_size)
  get_filename_component(artifact_name "${artifact}" NAME)
  math(EXPR artifact_size_kib "(${artifact_size} + 1023) / 1024")
  message(STATUS "  ${artifact_name}")
  message(STATUS "    Version: ${LP_VERSION}")
  message(STATUS "    Git description: ${LP_GIT_DESCRIPTION}")
  message(STATUS "    File size: ${artifact_size} bytes (${artifact_size_kib} KiB)")

  if(LP_SIZE_TOOL)
    execute_process(
      COMMAND "${LP_SIZE_TOOL}" "${artifact}"
      RESULT_VARIABLE size_result
      OUTPUT_VARIABLE size_output
      ERROR_VARIABLE size_error
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(size_result EQUAL 0)
      string(REPLACE "\n" "\n    " size_output_indented "${size_output}")
      message(STATUS "    Sections (text/data/bss):")
      message(STATUS "    ${size_output_indented}")
    else()
      message(STATUS "    Sections: size tool failed (${size_error})")
    endif()
  else()
    message(STATUS "    Sections (text/data/bss): unavailable; install LLVM or MinGW size tools")
  endif()
endforeach()

message(STATUS "")
