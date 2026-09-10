# Locates the Adobe After Effects SDK.
#
# Search order:
#   1. AE_SDK_ROOT (CMake cache variable or -D on the command line)
#   2. AESDK_ROOT environment variable (the name the Adobe samples use)
#   3. third_party/AfterEffectsSDK inside this repository
#
# Defines: AfterEffectsSDK_FOUND, AE_SDK_ROOT, AE_SDK_INCLUDE_DIRS

set(_ae_sdk_candidates "")
if(AE_SDK_ROOT)
    list(APPEND _ae_sdk_candidates "${AE_SDK_ROOT}")
endif()
if(DEFINED ENV{AESDK_ROOT})
    list(APPEND _ae_sdk_candidates "$ENV{AESDK_ROOT}")
    list(APPEND _ae_sdk_candidates "$ENV{AESDK_ROOT}/Examples")
endif()
list(APPEND _ae_sdk_candidates "${CMAKE_CURRENT_LIST_DIR}/../third_party/AfterEffectsSDK")

set(AfterEffectsSDK_FOUND FALSE)
foreach(_candidate IN LISTS _ae_sdk_candidates)
    if(EXISTS "${_candidate}/Headers/AE_Effect.h")
        get_filename_component(AE_SDK_ROOT "${_candidate}" ABSOLUTE)
        set(AfterEffectsSDK_FOUND TRUE)
        break()
    endif()
endforeach()

if(AfterEffectsSDK_FOUND)
    set(AE_SDK_INCLUDE_DIRS
        "${AE_SDK_ROOT}/Headers"
        "${AE_SDK_ROOT}/Headers/SP"
        "${AE_SDK_ROOT}/Util"
        "${AE_SDK_ROOT}/Resources")
    # Only Headers and Util are mandatory; drop the rest if a slim copy is used.
    set(_ae_existing "")
    foreach(_dir IN LISTS AE_SDK_INCLUDE_DIRS)
        if(EXISTS "${_dir}")
            list(APPEND _ae_existing "${_dir}")
        endif()
    endforeach()
    set(AE_SDK_INCLUDE_DIRS "${_ae_existing}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(AfterEffectsSDK
    REQUIRED_VARS AE_SDK_ROOT AE_SDK_INCLUDE_DIRS
    FAIL_MESSAGE "After Effects SDK not found. Set -DAE_SDK_ROOT=<path to the SDK Examples folder> or the AESDK_ROOT environment variable.")
