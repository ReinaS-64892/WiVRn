# SPDX-License-Identifier: GPL-3.0-or-later
# Build only the runtime components, embedding plugins into one private DSO.
# This avoids discovering incompatible system libsurvive libraries/plugins.
# No C# bindings or opportunistic .NET builds in the runtime dependency.
set(DOTNET "" CACHE FILEPATH "" FORCE)
set(DO_CORE_BUILD ON CACHE BOOL "" FORCE)
set(BUILD_STATIC OFF CACHE BOOL "" FORCE)
set(BUILD_APPLICATIONS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_GATT_SUPPORT OFF CACHE BOOL "" FORCE)
set(USE_HIDAPI OFF CACHE BOOL "" FORCE)
set(USE_EIGEN ON CACHE BOOL "" FORCE)
set(USE_OPENBLAS OFF CACHE BOOL "" FORCE)
set(survive_plugins driver_vive driver_playback driver_global_scene_solver
    disambiguator_statebased poser_mpfit)
foreach(plugin IN LISTS survive_plugins)
    set(ENABLE_${plugin} ON CACHE BOOL "" FORCE)
endforeach()
FetchContent_MakeAvailable(libsurvive)
add_library(wivrn-survive ALIAS survive)
# Consumers need the API headers, but never upstream's private build flags.
target_include_directories(survive PUBLIC
    "${libsurvive_SOURCE_DIR}/include/libsurvive"
    "${libsurvive_SOURCE_DIR}/include"
    "${libsurvive_SOURCE_DIR}/redist")
foreach(plugin IN LISTS survive_plugins)
    get_target_property(plugin_sources ${plugin} SOURCES)
    foreach(source IN LISTS plugin_sources)
        if(NOT IS_ABSOLUTE "${source}")
            set(source "${libsurvive_SOURCE_DIR}/src/${source}")
        endif()
        target_sources(survive PRIVATE "${source}")
    endforeach()
    get_target_property(plugin_libraries ${plugin} LINK_LIBRARIES)
    list(REMOVE_ITEM plugin_libraries survive ${survive_plugins})
    if(plugin_libraries)
        target_link_libraries(survive ${plugin_libraries})
    endif()
endforeach()
target_compile_definitions(survive PRIVATE SURVIVE_DISABLE_PLUGINS)
set_target_properties(survive PROPERTIES OUTPUT_NAME wivrn-survive
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib" INSTALL_RPATH "$ORIGIN")
# Upstream is EXCLUDE_FROM_ALL, so only this private runtime is installed.
install(TARGETS survive LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")

if(WIVRN_BUILD_TEST)
    add_executable(lh-survive-runtime-test tests/runtime.c)
    target_link_libraries(lh-survive-runtime-test PRIVATE wivrn-survive)
    set_target_properties(lh-survive-runtime-test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
