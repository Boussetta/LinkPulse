add_library(lp_test_support INTERFACE)
target_include_directories(lp_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

function(lp_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE lp_platform lp_test_support lp_warnings)
  add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
endfunction()

lp_add_test(test_core test_core.c)
lp_add_test(test_clock test_clock.c)
lp_add_test(test_sampler test_sampler.c)
lp_add_test(test_format test_format.c)
lp_add_test(test_config test_config.c)
