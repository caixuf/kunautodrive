if(NOT DEFINED FLOW_ROOT)
  set(FLOW_ROOT "${CMAKE_SOURCE_DIR}")
endif()

# 显式开关：默认 OFF。ON 时若本地找不到 Lanelet2 源码，则 configure 期联网拉取
# （本地想验证 flowsim 的"真 lanelet::Id"路径时才开；CI 一律不开 —— 见 kun_add_lanelet2 注释）。
option(FLOWENGINE_FETCH_LANELET2
       "Fetch Lanelet2 source from GitHub at configure time (needs network)" OFF)

# cmake/Lanelet2Standalone.cmake — Lanelet2 without ROS/mrt (D2-03 Prefer A)
# Builds lanelet2_core + lanelet2_io + lanelet2_projection from source tree.
# Candidates: third_party/lanelet2 (if fully checked out) → build/_deps/lanelet2-src.
# matching needs traffic_rules — not linked in this thin knife (see PR out-of-scope).

function(kun_find_lanelet2_src out_var)
  set(_cands
    "${FLOW_ROOT}/third_party/lanelet2"
    "${CMAKE_BINARY_DIR}/_deps/lanelet2-src"
    "${FLOW_ROOT}/build/_deps/lanelet2-src"
  )
  foreach(_c IN LISTS _cands)
    if(EXISTS "${_c}/lanelet2_core/include/lanelet2_core/LaneletMap.h")
      set(${out_var} "${_c}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out_var} "" PARENT_SCOPE)
endfunction()

function(kun_add_lanelet2)
  if(TARGET kun_lanelet2_core)
    return()
  endif()
  kun_find_lanelet2_src(_LL2_SRC)

  # 第三级兜底：显式开关下联网拉源码（文档里的候选链第三级）。
  # 默认 OFF —— configure 期联网是本仓库明令避免的失败形态：2026-10-22 三个
  # CI job 因 libeigen3-dev 漏装退到联网下载、网络一抖整片红（见 ci-eigen-gate）。
  # 本地想开真 ID 路径：cmake -DFLOWENGINE_FETCH_LANELET2=ON ...
  # 只 Populate 源码、不 add_subdirectory —— Lanelet2 自己的 CMakeLists 要
  # mrt_cmake_modules，本文件是按源文件直接编的（见下方 add_library）。
  if(NOT _LL2_SRC AND FLOWENGINE_FETCH_LANELET2)
    message(STATUS "Lanelet2 source not local — fetching master (FLOWENGINE_FETCH_LANELET2=ON)")
    include(FetchContent)
    FetchContent_Declare(lanelet2
      GIT_REPOSITORY https://github.com/fzi-forschungszentrum-informatik/Lanelet2.git
      GIT_TAG        master
      GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(lanelet2)
    if(NOT lanelet2_POPULATED)
      FetchContent_Populate(lanelet2)
    endif()
    if(EXISTS "${lanelet2_SOURCE_DIR}/lanelet2_core/include/lanelet2_core/LaneletMap.h")
      set(_LL2_SRC "${lanelet2_SOURCE_DIR}")
    endif()
  endif()

  if(NOT _LL2_SRC)
    message(WARNING
      "Lanelet2 source not found — flowsim real-ID path disabled at link time.\n"
      "  想启用：cmake -DFLOWENGINE_FETCH_LANELET2=ON ...（联网拉 master，需 libpugixml-dev + libgeographiclib-dev）\n"
      "  或把源码放到 third_party/lanelet2（须含 lanelet2_core/include/lanelet2_core/LaneletMap.h）")
    set(KUN_HAS_LANELET2 OFF PARENT_SCOPE)
    return()
  endif()
  message(STATUS "Lanelet2 source: ${_LL2_SRC}")
  set(KUN_HAS_LANELET2 ON PARENT_SCOPE)
  set(KUN_LANELET2_SRC "${_LL2_SRC}" PARENT_SCOPE)

  find_package(Boost REQUIRED COMPONENTS filesystem)
  find_package(Eigen3 3.3 QUIET NO_MODULE)
  find_library(PUGIXML_LIB pugixml)
  find_path(PUGIXML_INC pugixml.hpp)
  if(NOT PUGIXML_LIB OR NOT PUGIXML_INC)
    message(FATAL_ERROR "pugixml required for Lanelet2 io (apt: libpugixml-dev)")
  endif()
  find_library(GEOGRAPHICLIB_LIB GeographicLib Geographic)
  find_path(GEOGRAPHICLIB_INC GeographicLib/Config.h)
  if(NOT GEOGRAPHICLIB_LIB OR NOT GEOGRAPHICLIB_INC)
    message(FATAL_ERROR "GeographicLib required for Lanelet2 projection (apt: libgeographiclib-dev)")
  endif()

  set(_EIGEN_INC "")
  if(NOT TARGET Eigen3::Eigen)
    if(EXISTS "/usr/include/eigen3/Eigen/Core")
      set(_EIGEN_INC "/usr/include/eigen3")
    endif()
  endif()

  # ── core ──
  file(GLOB _LL2_CORE_SRC "${_LL2_SRC}/lanelet2_core/src/*.cpp")
  add_library(kun_lanelet2_core STATIC ${_LL2_CORE_SRC})
  target_include_directories(kun_lanelet2_core PUBLIC
    "${_LL2_SRC}/lanelet2_core/include"
  )
  if(_EIGEN_INC)
    target_include_directories(kun_lanelet2_core PUBLIC ${_EIGEN_INC})
  endif()
  if(TARGET Eigen3::Eigen)
    target_link_libraries(kun_lanelet2_core PUBLIC Eigen3::Eigen)
  endif()
  target_link_libraries(kun_lanelet2_core PUBLIC Boost::boost)
  target_compile_features(kun_lanelet2_core PUBLIC cxx_std_17)
  set_target_properties(kun_lanelet2_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # ── io (no Register* TUs — those live in kun_lanelet2_io_plugins) ──
  set(_LL2_IO_SRC
    ${_LL2_SRC}/lanelet2_io/src/Io.cpp
    ${_LL2_SRC}/lanelet2_io/src/OsmFile.cpp
    ${_LL2_SRC}/lanelet2_io/src/Factory.cpp
  )
  add_library(kun_lanelet2_io STATIC ${_LL2_IO_SRC})
  target_include_directories(kun_lanelet2_io PUBLIC
    "${_LL2_SRC}/lanelet2_io/include"
    ${PUGIXML_INC}
  )
  target_link_libraries(kun_lanelet2_io PUBLIC kun_lanelet2_core ${PUGIXML_LIB} Boost::boost Boost::filesystem)
  target_compile_features(kun_lanelet2_io PUBLIC cxx_std_17)
  set_target_properties(kun_lanelet2_io PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # Handlers with file-scope RegisterParser/RegisterWriter — must be WHOLE_ARCHIVE
  # linked (see INTERFACE below) or the ".osm" extension never registers.
  set(_LL2_IO_PLUGIN_SRC
    ${_LL2_SRC}/lanelet2_io/src/OsmHandlerLoad.cpp
    ${_LL2_SRC}/lanelet2_io/src/OsmHandlerWrite.cpp
  )
  add_library(kun_lanelet2_io_plugins STATIC ${_LL2_IO_PLUGIN_SRC})
  target_link_libraries(kun_lanelet2_io_plugins PUBLIC kun_lanelet2_io)
  target_compile_features(kun_lanelet2_io_plugins PUBLIC cxx_std_17)
  set_target_properties(kun_lanelet2_io_plugins PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # ── projection ──
  file(GLOB _LL2_PROJ_SRC "${_LL2_SRC}/lanelet2_projection/src/*.cpp")
  add_library(kun_lanelet2_projection STATIC ${_LL2_PROJ_SRC})
  target_include_directories(kun_lanelet2_projection PUBLIC
    "${_LL2_SRC}/lanelet2_projection/include"
    ${GEOGRAPHICLIB_INC}
  )
  target_link_libraries(kun_lanelet2_projection PUBLIC kun_lanelet2_io ${GEOGRAPHICLIB_LIB})
  target_compile_features(kun_lanelet2_projection PUBLIC cxx_std_17)
  set_target_properties(kun_lanelet2_projection PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # Convenience INTERFACE: Prefer A subset (core/io/projection + osm plugins).
  # Plugins linked WHOLE_ARCHIVE only here (not via projection) to avoid CMake
  # "feature already occurred" errors while keeping RegisterParser ctors.
  add_library(kun_lanelet2 INTERFACE)
  target_link_libraries(kun_lanelet2 INTERFACE
    kun_lanelet2_core
    kun_lanelet2_io
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,kun_lanelet2_io_plugins>"
    kun_lanelet2_projection)
  target_compile_definitions(kun_lanelet2 INTERFACE KUN_HAS_LANELET2=1)
endfunction()
