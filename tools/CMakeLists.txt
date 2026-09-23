# The operator tool is built as dpufabric_tool and installed as dpufabric, so the
# executable name never collides with the library target name.
add_executable(dpufabric_tool dpufabric.cpp)
set_target_properties(dpufabric_tool PROPERTIES OUTPUT_NAME dpufabric)
target_link_libraries(dpufabric_tool PRIVATE dpu::fabric)
dpuf_apply_warnings(dpufabric_tool)
install(TARGETS dpufabric_tool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
