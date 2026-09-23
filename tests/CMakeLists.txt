add_library(dpuf_test_support STATIC support/harness.cpp support/scenario.cpp support/process.cpp)
if(MSVC)
  target_link_libraries(dpuf_test_support PUBLIC ws2_32)
endif()
target_link_libraries(dpuf_test_support PUBLIC dpu::fabric)
target_include_directories(dpuf_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
dpuf_apply_warnings(dpuf_test_support)

set(DPUF_TEST_SOURCES
    unit/test_core.cpp
    unit/test_model.cpp
    unit/test_planning.cpp
    integration/test_lifecycle.cpp
    integration/test_persistence.cpp
    integration/test_transport.cpp
    integration/test_crash.cpp
    integration/test_concurrency.cpp
    integration/test_property.cpp
    integration/test_adversarial.cpp)

foreach(source IN LISTS DPUF_TEST_SOURCES)
  get_filename_component(name ${source} NAME_WE)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE dpuf_test_support)
  dpuf_apply_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
endforeach()
