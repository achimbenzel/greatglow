cmake_minimum_required(VERSION 3.20)

project(AbGlow VERSION 1.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static CRT: an .aex should not depend on a redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MINGW)
    # Self-contained binaries: the .aex and the test tools must not depend on
    # the mingw runtime DLLs.
    add_link_options(-static-libgcc -static-libstdc++ -static)
endif()

option(ABGLOW_BUILD_PLUGIN "Build the After Effects .aex plug-in" ${WIN32})
option(ABGLOW_BUILD_TESTS "Build the core tests and the preview tool" ON)
option(ABGLOW_ENABLE_AVX2 "Compile the image core with AVX2" OFF)

set(ABGLOW_INSTALL_DIR "" CACHE PATH
    "Where 'cmake --install' copies AbGlow.aex (defaults to the shared Adobe plug-in folder on Windows)")

# ---------------------------------------------------------------------------
# Image processing core: no After Effects dependency, so it can be unit tested
# on any platform.
# ---------------------------------------------------------------------------
add_library(abglow_core STATIC
    src/core/Blur.cpp
    src/core/GlowPipeline.cpp
    src/core/GlowPlan.cpp
    src/core/Resample.cpp
    src/core/Transfer.cpp)

target_include_directories(abglow_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(MSVC)
    target_compile_options(abglow_core PRIVATE /W4 /permissive- /Zc:__cplusplus)
    if(ABGLOW_ENABLE_AVX2)
        target_compile_options(abglow_core PRIVATE /arch:AVX2)
    endif()
else()
    target_compile_options(abglow_core PRIVATE -Wall -Wextra)
    if(ABGLOW_ENABLE_AVX2)
        target_compile_options(abglow_core PRIVATE -mavx2 -mfma)
    endif()
endif()

set_target_properties(abglow_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# After Effects plug-in
# ---------------------------------------------------------------------------
if(ABGLOW_BUILD_PLUGIN)
    find_package(AfterEffectsSDK REQUIRED)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # Stamped into the plug-in's UI so the loaded build can be identified.
    set(ABGLOW_BUILD_ID "local")
    find_package(Git QUIET)
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _abglow_git_sha
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_abglow_git_sha)
            set(ABGLOW_BUILD_ID "${_abglow_git_sha}")
        endif()
    endif()
    message(STATUS "AB Glow build id: ${ABGLOW_BUILD_ID}")

    set(ABGLOW_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(ABGLOW_PIPL_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_pipl.py")

    # The PiPL only changes when the plug-in's identity or flags change, so it is
    # generated at configure time and regenerated whenever the script changes.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ABGLOW_PIPL_SCRIPT}")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${ABGLOW_PIPL_SCRIPT}"
                --sdk-root "${AE_SDK_ROOT}"
                --output-dir "${ABGLOW_GENERATED_DIR}"
                --version-major ${PROJECT_VERSION_MAJOR}
                --version-minor ${PROJECT_VERSION_MINOR}
                --version-bug ${PROJECT_VERSION_PATCH}
        RESULT_VARIABLE _pipl_result
        OUTPUT_VARIABLE _pipl_output
        ERROR_VARIABLE _pipl_error)
    if(NOT _pipl_result EQUAL 0)
        message(FATAL_ERROR "generate_pipl.py failed: ${_pipl_error}")
    endif()
    message(STATUS "AB Glow ${_pipl_output}")

    add_library(AbGlow MODULE
        src/plugin/AbGlowEntry.cpp
        src/plugin/AbGlowParams.cpp
        src/plugin/AbGlowRender.cpp
        src/plugin/AeAdapters.cpp
        src/plugin/Diagnostics.cpp
        "${ABGLOW_GENERATED_DIR}/AbGlow_PiPL.rc")

    target_link_libraries(AbGlow PRIVATE abglow_core)
    target_include_directories(AbGlow PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin"
        "${ABGLOW_GENERATED_DIR}")
    target_include_directories(AbGlow SYSTEM PRIVATE ${AE_SDK_INCLUDE_DIRS})
    target_compile_definitions(AbGlow PRIVATE AB_GLOW_BUILD_ID="${ABGLOW_BUILD_ID}")

    # Writes a per-render log to %TEMP%/AbGlow-diag.log. Off by default; it is
    # how a real After Effects session can be observed when the mock host in
    # tests/ cannot reproduce a report.
    option(ABGLOW_DIAGNOSTICS "Log what the host asks the plug-in for" OFF)
    if(ABGLOW_DIAGNOSTICS)
        target_compile_definitions(AbGlow PRIVATE ABGLOW_DIAGNOSTICS)
        message(STATUS "AB Glow diagnostics: ON")
    endif()

    set_target_properties(AbGlow PROPERTIES
        PREFIX ""
        OUTPUT_NAME "AbGlow"
        SUFFIX ".aex")

    if(WIN32)
        # The SDK headers pick their platform from these.
        target_compile_definitions(AbGlow PRIVATE WIN_ENV _WINDOWS MSWindows NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()

    if(MSVC)
        target_compile_options(AbGlow PRIVATE /W4 /permissive- /Zc:__cplusplus)
        # Keep SDK header warnings out of our build log.
        target_compile_options(AbGlow PRIVATE /external:W0)
    else()
        target_compile_options(AbGlow PRIVATE -Wall -Wextra)
    endif()

    if(ABGLOW_INSTALL_DIR)
        set(_abglow_install_dir "${ABGLOW_INSTALL_DIR}")
    elseif(WIN32)
        set(_abglow_install_dir "C:/Program Files/Adobe/Common/Plug-ins/7.0/MediaCore/AB Tools")
    else()
        set(_abglow_install_dir "${CMAKE_INSTALL_PREFIX}")
    endif()
    install(TARGETS AbGlow RUNTIME DESTINATION "${_abglow_install_dir}"
                           LIBRARY DESTINATION "${_abglow_install_dir}")
endif()

# ---------------------------------------------------------------------------
# Tests and the offline preview tool
# ---------------------------------------------------------------------------
if(ABGLOW_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)

    add_library(abglow_test_support STATIC tests/TestSupport.cpp)
    target_include_directories(abglow_test_support PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(abglow_test_support PUBLIC abglow_core Threads::Threads)

    add_executable(abglow_tests tests/CoreTests.cpp)
    target_link_libraries(abglow_tests PRIVATE abglow_test_support)
    add_test(NAME core COMMAND abglow_tests)

    add_executable(abglow_preview tests/PreviewTool.cpp)
    target_link_libraries(abglow_preview PRIVATE abglow_test_support)

    # Loads the built .aex and drives the After Effects command sequence.
    if(ABGLOW_BUILD_PLUGIN AND WIN32)
        add_executable(abglow_mock_host tests/MockHost.cpp)
        target_link_libraries(abglow_mock_host PRIVATE abglow_test_support)
        target_include_directories(abglow_mock_host SYSTEM PRIVATE ${AE_SDK_INCLUDE_DIRS})
        target_compile_definitions(abglow_mock_host PRIVATE WIN_ENV _WINDOWS MSWindows NOMINMAX WIN32_LEAN_AND_MEAN)
        add_dependencies(abglow_mock_host AbGlow)
        add_test(NAME plugin COMMAND abglow_mock_host $<TARGET_FILE:AbGlow>)
    endif()
endif()
