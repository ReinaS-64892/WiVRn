# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Like Monado, the provider is built from a pinned source checkout. Keep its
# optional apps/tests out of the host build; no system SteamVR driver is replaced.
set(BUILD_SHARED_LIBS OFF)
set(LH_BUILD_USB ON CACHE BOOL "" FORCE)
set(LH_BUILD_OPENVR_DRIVER ON CACHE BOOL "" FORCE)
set(LH_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LH_BUILD_VIEWER OFF CACHE BOOL "" FORCE)
set(LH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LH_BUILD_ORACLE_TESTS OFF CACHE BOOL "" FORCE)
set(OPENVR_INCLUDE_DIR "${monado_SOURCE_DIR}/src/external/openvr_includes" CACHE PATH "" FORCE)
find_package(Ceres REQUIRED)
find_package(ZLIB REQUIRED)
FetchContent_MakeAvailable(openhouse)
if(NOT TARGET driver_lighthouse)
    message(FATAL_ERROR "openhouse did not build its OpenVR provider")
endif()

# Reuse upstream's complete frontend, with a unique, directly linked factory.
# A private DSO keeps embedded hidapi/OpenVR globals out of the Monado host.
get_target_property(provider_sources driver_lighthouse SOURCES)
get_target_property(provider_includes driver_lighthouse INCLUDE_DIRECTORIES)
get_target_property(provider_libraries driver_lighthouse LINK_LIBRARIES)
set(absolute_provider_sources)
foreach(source IN LISTS provider_sources)
    if(source MATCHES "^\\$<" OR IS_ABSOLUTE "${source}")
        list(APPEND absolute_provider_sources "${source}")
    else()
        list(APPEND absolute_provider_sources "${openhouse_SOURCE_DIR}/${source}")
    endif()
endforeach()
add_library(wivrn-openhouse SHARED ${absolute_provider_sources})
target_include_directories(wivrn-openhouse PRIVATE ${provider_includes})
target_link_libraries(wivrn-openhouse PRIVATE ${provider_libraries})
target_compile_features(wivrn-openhouse PRIVATE cxx_std_20)
target_compile_definitions(wivrn-openhouse PRIVATE HmdDriverFactory=wivrn_openhouse_factory)
set_target_properties(wivrn-openhouse PROPERTIES CXX_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib" INSTALL_RPATH "$ORIGIN")
target_link_options(wivrn-openhouse PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map" -Wl,--no-undefined)
install(TARGETS wivrn-openhouse LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")

set(adapter "${CMAKE_CURRENT_BINARY_DIR}/adapter")
file(GLOB_RECURSE adapter_inputs CONFIGURE_DEPENDS
    "${monado_SOURCE_DIR}/src/xrt/drivers/steamvr_lh/*.cpp"
    "${monado_SOURCE_DIR}/src/xrt/drivers/steamvr_lh/*.hpp"
    "${monado_SOURCE_DIR}/src/xrt/drivers/steamvr_lh/*.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${adapter_inputs} "${CMAKE_CURRENT_SOURCE_DIR}/prepare_adapter.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_adapter.py"
    "${monado_SOURCE_DIR}/src/xrt/drivers/steamvr_lh" "${adapter}"
    COMMAND_ERROR_IS_FATAL ANY)
file(GLOB_RECURSE adapter_sources "${adapter}/*.cpp")
add_library(drv_openhouse STATIC ${adapter_sources})
target_include_directories(drv_openhouse PUBLIC "${adapter}")
target_link_libraries(drv_openhouse PUBLIC xrt-external-openvr PRIVATE aux_util aux_math aux_vive drv_vp2 drv_includes
    base xrt-interfaces xrt-external-cjson xrt-external-vdf
    wivrn-openhouse ZLIB::ZLIB)

if(WIVRN_BUILD_TEST)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_adapter.py"
        --test "${PROJECT_SOURCE_DIR}/tools/lh-recorder/tests/hotplug.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/openhouse_hotplug.cpp" COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/lh-recorder/tests/hotplug.cpp")
    add_executable(lh-openhouse-adapter-test tests/adapter.cpp)
    target_include_directories(lh-openhouse-adapter-test BEFORE PRIVATE "${adapter}" "${CMAKE_CURRENT_BINARY_DIR}"
        "${PROJECT_SOURCE_DIR}/server" "${monado_SOURCE_DIR}/src/xrt/drivers"
        "${monado_SOURCE_DIR}/src/xrt/base")
    target_link_libraries(lh-openhouse-adapter-test PRIVATE drv_openhouse base aux_util xrt-interfaces
        xrt-external-openvr wivrn-common wivrn-external)
    if(WIVRN_FEATURE_STEAMVR_LIGHTHOUSE)
        target_include_directories(lh-openhouse-adapter-test PRIVATE "${monado_SOURCE_DIR}/src/xrt/drivers/steamvr_lh")
        target_link_libraries(lh-openhouse-adapter-test PRIVATE drv_steamvr_lh)
    endif()
    set_target_properties(lh-openhouse-adapter-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
