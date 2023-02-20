OPTION(BUILD_KOI_FROM_SOURCE OFF)

function(get_best_compatible_koi_version KOI_CUDA)
    if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64")
        # Koi only provides binaries for 11.4 when targeting aarch64
        set(SUPPORTED_VERSIONS 11.4)
    else()
        set(SUPPORTED_VERSIONS 12.0 11.8 11.7 11.3)
    endif()

    list(SORT SUPPORTED_VERSIONS COMPARE NATURAL ORDER DESCENDING)
    foreach(SUPPORTED_VERSION IN LISTS SUPPORTED_VERSIONS)
        if (${CUDAToolkit_VERSION} VERSION_GREATER_EQUAL ${SUPPORTED_VERSION})
            set(${KOI_CUDA} ${SUPPORTED_VERSION} PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "Unsupported CUDA toolkit version: ${CUDAToolkit_VERSION}")
endfunction()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32)

    if(BUILD_KOI_FROM_SOURCE)
        message(STATUS "Building Koi from source")
        set(KOI_DIR "${DORADO_3RD_PARTY}/koi")

        if(NOT EXISTS ${KOI_DIR})
            if(DEFINED GITLAB_CI_TOKEN)
                message("Cloning Koi using CI token")
                execute_process(COMMAND git clone https://gitlab-ci-token:${GITLAB_CI_TOKEN}@git.oxfordnanolabs.local/machine-learning/koi.git ${KOI_DIR})
            else()
                message("Cloning Koi using ssh")
                execute_process(COMMAND git clone git@git.oxfordnanolabs.local:machine-learning/koi.git ${KOI_DIR})
            endif()
            execute_process(COMMAND git checkout 8d4d505610dce10a13e589109ebef908d0a2981e WORKING_DIRECTORY ${KOI_DIR})
            execute_process(COMMAND git submodule update --init --checkout WORKING_DIRECTORY ${KOI_DIR})
        endif()
        add_subdirectory(${KOI_DIR}/koi/lib)

        set(KOI_INCLUDE ${KOI_DIR}/koi/lib)
        set(KOI_LIBRARIES koi)
    else()

        set(KOI_VERSION 0.2.1)
        find_package(CUDAToolkit REQUIRED)
        get_best_compatible_koi_version(KOI_CUDA)
        set(KOI_DIR libkoi-${KOI_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-cuda-${KOI_CUDA})
        set(KOI_CDN_URL "https://cdn.oxfordnanoportal.com/software/analysis")

        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            download_and_extract(${KOI_CDN_URL}/${KOI_DIR}.tar.gz ${KOI_DIR})
            set(KOI_LIBRARIES ${DORADO_3RD_PARTY}/${KOI_DIR}/${KOI_DIR}/lib/libkoi.a)
        elseif(WIN32)
            download_and_extract(${KOI_CDN_URL}/${KOI_DIR}.zip ${KOI_DIR})
            set(KOI_LIBRARIES ${DORADO_3RD_PARTY}/${KOI_DIR}/${KOI_DIR}/lib/koi.lib)
        endif()
        set(KOI_INCLUDE ${DORADO_3RD_PARTY}/${KOI_DIR}/${KOI_DIR}/include)

    endif()
endif()
