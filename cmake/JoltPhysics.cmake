# cmake/JoltPhysics.cmake — Standalone headless Jolt Physics integration
# Builds static -fPIC libJolt.a from third_party/JoltPhysics/Build with
# double-precision world coordinates (ENU 50km+) and zero render/viewer bloat.

if(NOT DEFINED FLOW_ROOT)
  set(FLOW_ROOT "${CMAKE_SOURCE_DIR}")
endif()

function(flow_add_jolt)
  if(TARGET Jolt)
    set(FLOW_HAS_JOLT ON PARENT_SCOPE)
    return()
  endif()

  set(_JOLT_ROOT "${FLOW_ROOT}/third_party/JoltPhysics")
  if(NOT EXISTS "${_JOLT_ROOT}/Build/CMakeLists.txt")
    message(WARNING "JoltPhysics not found at ${_JOLT_ROOT} — jolt3d physics backend disabled")
    set(FLOW_HAS_JOLT OFF PARENT_SCOPE)
    return()
  endif()

  # Force consistent headless, -fPIC, double-precision ABI across both CMake trees
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
  set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
  set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
  set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
  set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
  set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
  set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
  set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
  set(DEBUG_RENDERER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
  set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
  set(PROFILER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
  set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
  set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
  set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
  set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
  set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

  if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.25")
    add_subdirectory("${_JOLT_ROOT}/Build" "${CMAKE_BINARY_DIR}/_deps/jolt-build" EXCLUDE_FROM_ALL SYSTEM)
  else()
    add_subdirectory("${_JOLT_ROOT}/Build" "${CMAKE_BINARY_DIR}/_deps/jolt-build" EXCLUDE_FROM_ALL)
  endif()

  if(TARGET Jolt)
    set_target_properties(Jolt PROPERTIES
      POSITION_INDEPENDENT_CODE ON
      CXX_STANDARD 17
      CXX_STANDARD_REQUIRED ON
    )
    if(NOT MSVC)
      target_compile_options(Jolt PRIVATE -fno-fast-math)
    endif()
    set(FLOW_HAS_JOLT ON PARENT_SCOPE)
    message(STATUS "JoltPhysics enabled (v5.7.0, DOUBLE_PRECISION=ON, source=${_JOLT_ROOT})")
  else()
    set(FLOW_HAS_JOLT OFF PARENT_SCOPE)
    message(WARNING "JoltPhysics build directory did not define target 'Jolt'")
  endif()
endfunction()
