cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(flowengine_pem LANGUAGES C)
endif()

include(CTest)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

add_library(flowengine_pem STATIC
    pem_log.c
    pem_runtime.c
)
add_library(FlowEngine::pem ALIAS flowengine_pem)
set_target_properties(flowengine_pem PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(flowengine_pem
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/flowengine>
)
target_link_libraries(flowengine_pem PUBLIC Threads::Threads)

if(BUILD_TESTING AND NOT WIN32)
    add_executable(test_pem_log tests/test_pem_log.c)
    target_compile_definitions(test_pem_log PRIVATE
        PEM_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    )
    target_link_libraries(test_pem_log PRIVATE FlowEngine::pem)
    add_test(NAME pem_log_protocol COMMAND test_pem_log)

    add_executable(test_pem_runtime tests/test_pem_runtime.c)
    target_link_libraries(test_pem_runtime PRIVATE FlowEngine::pem)
    add_test(NAME pem_runtime_bounded COMMAND test_pem_runtime)
endif()

install(TARGETS flowengine_pem
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/pem_log.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/pem_runtime.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/pem_log_layout.def
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/flowengine
)
