# SPDX-FileCopyrightText: Copyright 2026 GR2port
# SPDX-License-Identifier: GPL-2.0-or-later

option(ENABLE_GR2_FSR3 "Build the Gravity Rush 2 FSR 3.1 and FSR 4 upscaler module" ON)

if (ENABLE_GR2_FSR3 AND NOT APPLE)
    set(GR2_FSR3_DIR ${CMAKE_CURRENT_LIST_DIR})
    set(GR2_FFX_ROOT ${PROJECT_SOURCE_DIR}/third_party/fsr-vulkan)
    set(GR2_FFX_SDK ${GR2_FFX_ROOT}/upstream/ffx-1.1.4/sdk)

    # The vendored FidelityFX SDK 1.1.4 with the FSR 3.1 upscaler and FSR 3.1's frame generation
    # (frame interpolation and optical flow), its Vulkan backend and the portable C API. The
    # effects' shaders are the SPIR-V compiled ahead of time in the generated folders.
    add_library(gr2_ffx_fsr3_host STATIC
        ${GR2_FFX_SDK}/src/shared/ffx_assert.cpp
        ${GR2_FFX_SDK}/src/shared/ffx_message.cpp
        ${GR2_FFX_SDK}/src/shared/ffx_object_management.cpp
        ${GR2_FFX_SDK}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
        ${GR2_FFX_SDK}/src/components/opticalflow/ffx_opticalflow.cpp
        ${GR2_FFX_SDK}/src/components/frameinterpolation/ffx_frameinterpolation.cpp)
    target_include_directories(gr2_ffx_fsr3_host
        PUBLIC ${GR2_FFX_SDK}/include
        PRIVATE ${GR2_FFX_SDK}/src/shared ${GR2_FFX_SDK}/src/components)
    target_compile_definitions(gr2_ffx_fsr3_host PRIVATE DISABLE_FFX_DEBUG_BREAK)

    add_library(gr2_ffx_fsr3_vk STATIC
        ${GR2_FFX_SDK}/src/backends/vk/ffx_vk.cpp
        ${GR2_FFX_SDK}/src/backends/shared/ffx_shader_blobs.cpp
        ${GR2_FFX_SDK}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
        ${GR2_FFX_SDK}/src/backends/shared/blob_accessors/ffx_frameinterpolation_shaderblobs.cpp
        ${GR2_FFX_SDK}/src/backends/shared/blob_accessors/ffx_opticalflow_shaderblobs.cpp
        ${GR2_FFX_SDK}/src/shared/ffx_breadcrumbs_list.cpp)
    target_include_directories(gr2_ffx_fsr3_vk PRIVATE
        ${GR2_FFX_SDK}/src/shared
        ${GR2_FFX_SDK}/src/components
        ${GR2_FFX_SDK}/src/backends/shared
        ${GR2_FFX_SDK}/src/backends/shared/blob_accessors
        ${GR2_FFX_ROOT}/generated/ffx-1.1.4/vk/fsr3upscaler
        ${GR2_FFX_ROOT}/generated/ffx-1.1.4/vk/fsr3framegeneration)
    target_compile_definitions(gr2_ffx_fsr3_vk PRIVATE
        FFX_FSR3UPSCALER=1 FFX_FI=1 FFX_OF=1 DISABLE_FFX_DEBUG_BREAK)
    target_link_libraries(gr2_ffx_fsr3_vk PUBLIC gr2_ffx_fsr3_host Vulkan::Headers)

    # With the module's SPIR-V reflection, pipelines and descriptor sets, which its bridge to the
    # SDK 2.3.0 effects uses.
    add_library(gr2_ffx_portable STATIC
        ${GR2_FFX_ROOT}/src/ffx_vk_portable.c
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr3_portable.cpp
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr3_3_1_5_reflection.c
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr3_3_1_5_catalog.c
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr3_3_1_5_pipeline.c
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr3_3_1_5_bundle.c
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr3_3_1_5_descriptor.c)
    set_target_properties(gr2_ffx_portable PROPERTIES C_STANDARD 11)
    target_include_directories(gr2_ffx_portable
        PUBLIC ${GR2_FFX_ROOT}/include
        PRIVATE ${GR2_FFX_SDK}/include ${GR2_FFX_ROOT}/generated/ffx-2.3.0/vk/fsr3upscaler-q2-v2)
    target_compile_definitions(gr2_ffx_portable PRIVATE
        FFX_VK_PORTABLE_HAS_FSR3_UPSCALER=1
        DISABLE_FFX_DEBUG_BREAK)
    target_link_libraries(gr2_ffx_portable PUBLIC Vulkan::Headers PRIVATE gr2_ffx_fsr3_vk)

    # FSR 3.1.6's frame generation (frame interpolation 3.1.6 and optical flow) from the vendored
    # FidelityFX SDK 2.3.0, with the module's Vulkan bridge; FSR 3.1.4's above runs where the GPU
    # lacks what it needs. Its shaders are the SPIR-V compiled ahead of time in the generated
    # folder. SDK 2.3.0 exports the C names SDK 1.1.4 does, so its sources take the private names
    # of the module's own 3.1.6 target.
    set(GR2_FFX_SDK_2_3_0 ${GR2_FFX_ROOT}/upstream/ffx-2.3.0/Kits/FidelityFX)
    add_library(gr2_ffx_framegen_3_1_6 STATIC
        ${GR2_FFX_SDK_2_3_0}/api/internal/ffx_assert.cpp
        ${GR2_FFX_SDK_2_3_0}/api/internal/ffx_message.cpp
        ${GR2_FFX_SDK_2_3_0}/api/internal/ffx_object_management.cpp
        ${GR2_FFX_SDK_2_3_0}/framegeneration/fsr3/internal/ffx_frameinterpolation.cpp
        ${GR2_FFX_SDK_2_3_0}/framegeneration/fsr3/internal/ffx_opticalflow.cpp
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr3_3_1_6_framegen_shaderblobs.cpp
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr3_3_1_5_bridge.cpp
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr3_3_1_6_framegen.cpp)
    target_include_directories(gr2_ffx_framegen_3_1_6 PRIVATE
        ${GR2_FFX_SDK_2_3_0}/api/include
        ${GR2_FFX_SDK_2_3_0}/api/internal
        ${GR2_FFX_SDK_2_3_0}/framegeneration/fsr3/include
        ${GR2_FFX_SDK_2_3_0}/framegeneration/fsr3/internal
        ${GR2_FFX_ROOT}/generated/ffx-2.3.0/vk/fsr3framegeneration-q2-v1)
    target_compile_definitions(gr2_ffx_framegen_3_1_6 PRIVATE
        DISABLE_FFX_DEBUG_BREAK
        ffxAssertReport=ffxVk315AssertReport
        ffxAssertSetPrintingCallback=ffxVk315AssertSetPrintingCallback
        ffxFsr3UpscalerContextCreate=ffxVk315Fsr3UpscalerContextCreate
        ffxFsr3UpscalerContextDestroy=ffxVk315Fsr3UpscalerContextDestroy
        ffxFsr3UpscalerContextDispatch=ffxVk315Fsr3UpscalerContextDispatch
        ffxFsr3UpscalerContextGenerateReactiveMask=ffxVk315Fsr3UpscalerContextGenerateReactiveMask
        ffxFsr3UpscalerContextGetGpuMemoryUsage=ffxVk315Fsr3UpscalerContextGetGpuMemoryUsage
        ffxFsr3UpscalerGetGpuMemoryUsage=ffxVk315Fsr3UpscalerGetGpuMemoryUsage
        ffxFsr3UpscalerGetJitterOffset=ffxVk315Fsr3UpscalerGetJitterOffset
        ffxFsr3UpscalerGetJitterPhaseCount=ffxVk315Fsr3UpscalerGetJitterPhaseCount
        ffxFsr3UpscalerGetRenderResolutionFromQualityMode=ffxVk315Fsr3UpscalerGetRenderResolutionFromQualityMode
        ffxFsr3UpscalerGetSharedResourceDescriptions=ffxVk315Fsr3UpscalerGetSharedResourceDescriptions
        ffxFsr3UpscalerGetUpscaleRatioFromQualityMode=ffxVk315Fsr3UpscalerGetUpscaleRatioFromQualityMode
        ffxFsr3UpscalerResourceIsNull=ffxVk315Fsr3UpscalerResourceIsNull
        ffxFsr3UpscalerSetConstant=ffxVk315Fsr3UpscalerSetConstant
        ffxFsr3UpscalerSetGlobalDebugMessage=ffxVk315Fsr3UpscalerSetGlobalDebugMessage
        frameInterpolationGetPermutationBlobByIndex=ffxVk316FrameInterpolationGetPermutationBlobByIndex
        frameInterpolationIsWave64=ffxVk316FrameInterpolationIsWave64
        opticalflowGetPermutationBlobByIndex=ffxVk316OpticalflowGetPermutationBlobByIndex
        opticalflowIsWave64=ffxVk316OpticalflowIsWave64
        ffxFrameInterpolationContextCreate=ffxVk316FrameInterpolationContextCreate
        ffxFrameInterpolationContextDestroy=ffxVk316FrameInterpolationContextDestroy
        ffxFrameInterpolationContextEnqueueRefreshPipelineRequest=ffxVk316FrameInterpolationContextEnqueueRefreshPipelineRequest
        ffxFrameInterpolationContextGetGpuMemoryUsage=ffxVk316FrameInterpolationContextGetGpuMemoryUsage
        ffxFrameInterpolationDispatch=ffxVk316FrameInterpolationDispatch
        ffxFrameInterpolationGetGpuMemoryUsage=ffxVk316FrameInterpolationGetGpuMemoryUsage
        ffxFrameInterpolationGetSharedResourceDescriptions=ffxVk316FrameInterpolationGetSharedResourceDescriptions
        ffxFrameInterpolationPrepare=ffxVk316FrameInterpolationPrepare
        ffxFrameInterpolationResourceIsNull=ffxVk316FrameInterpolationResourceIsNull
        ffxFrameInterpolationSetGlobalDebugMessage=ffxVk316FrameInterpolationSetGlobalDebugMessage
        ffxOpticalflowContextCreate=ffxVk316OpticalflowContextCreate
        ffxOpticalflowContextDestroy=ffxVk316OpticalflowContextDestroy
        ffxOpticalflowContextDispatch=ffxVk316OpticalflowContextDispatch
        ffxOpticalflowContextGetGpuMemoryUsage=ffxVk316OpticalflowContextGetGpuMemoryUsage
        ffxOpticalflowGetGpuMemoryUsage=ffxVk316OpticalflowGetGpuMemoryUsage
        ffxOpticalflowGetSharedResourceDescriptions=ffxVk316OpticalflowGetSharedResourceDescriptions
        ffxOpticalflowResourceIsNull=ffxVk316OpticalflowResourceIsNull
        ffxSharedContextGetGpuMemoryUsage=ffxVk316SharedContextGetGpuMemoryUsage
        ffxPrintMessage=ffxVk315PrintMessage
        ffxSafeReleaseCopyResource=ffxVk315SafeReleaseCopyResource
        ffxSafeReleasePipeline=ffxVk315SafeReleasePipeline
        ffxSafeReleaseResource=ffxVk315SafeReleaseResource
        ffxSetPrintMessageCallback=ffxVk315SetPrintMessageCallback)
    target_link_libraries(gr2_ffx_framegen_3_1_6 PUBLIC gr2_ffx_portable)

    if (MSVC)
        set(GR2_FFX_NO_WARNINGS /w)
    else()
        set(GR2_FFX_NO_WARNINGS -w)
    endif()
    foreach(GR2_FFX_TARGET gr2_ffx_fsr3_host gr2_ffx_fsr3_vk gr2_ffx_portable)
        set_target_properties(${GR2_FFX_TARGET} PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        target_compile_options(${GR2_FFX_TARGET} PRIVATE ${GR2_FFX_NO_WARNINGS})
    endforeach()
    # SDK 2.3.0 is C++20.
    set_target_properties(gr2_ffx_framegen_3_1_6 PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_compile_options(gr2_ffx_framegen_3_1_6 PRIVATE ${GR2_FFX_NO_WARNINGS})

    # The SDK sources call MSVC CRT functions, which this header supplies on other platforms.
    if (NOT WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        foreach(GR2_FFX_TARGET gr2_ffx_fsr3_host gr2_ffx_fsr3_vk gr2_ffx_framegen_3_1_6)
            target_compile_options(${GR2_FFX_TARGET} PRIVATE
                "SHELL:-include \"${GR2_FFX_ROOT}/src/ffx_1_1_4_portability.hpp\"")
        endforeach()
    endif()

    # The FSR 4.1.2 Lite provider and the module's C bridge to it. Only C sources include
    # the provider headers: their FFX type names collide with the SDK headers of FSR 3.1.
    add_library(gr2_ffx_fsr4_lite STATIC
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr4_lite.c
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr4_lite_assets.c
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr4_lite_provider.c
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr4_lite_schedule.c)
    target_include_directories(gr2_ffx_fsr4_lite PUBLIC ${GR2_FFX_ROOT}/include)
    # Other threads own queues of the device, and the module destroys a context only after the
    # GPU has finished its work.
    target_compile_definitions(gr2_ffx_fsr4_lite PRIVATE FFX_FSR4_VK_HOST_RETIRES_WORK=1)
    target_compile_options(gr2_ffx_fsr4_lite PRIVATE ${GR2_FFX_NO_WARNINGS})
    target_link_libraries(gr2_ffx_fsr4_lite PUBLIC Vulkan::Headers)
    if (UNIX)
        target_link_libraries(gr2_ffx_fsr4_lite PRIVATE m)
    endif()

    add_library(gr2_fsr4_bridge STATIC ${GR2_FSR3_DIR}/fsr4_lite_bridge.c)
    target_link_libraries(gr2_fsr4_bridge PUBLIC Vulkan::Headers PRIVATE gr2_ffx_fsr4_lite)
    set_target_properties(gr2_ffx_fsr4_lite gr2_fsr4_bridge PROPERTIES C_STANDARD 11)

    # Helper shaders, compiled like the host shaders.
    if (TARGET glslang-standalone)
        set(GR2_FSR3_GLSLANG glslang-standalone)
    elseif (TARGET glslang::glslang-standalone)
        set(GR2_FSR3_GLSLANG glslang::glslang-standalone)
    endif()
    set(GR2_FSR3_HEADER_SCRIPT
        ${PROJECT_SOURCE_DIR}/src/graphics/host_shaders/AddShaderHeaderInclude.cmake)
    set(GR2_FSR3_SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/gr2_fsr3_shaders/include)
    set(GR2_FSR3_SHADER_DIR ${GR2_FSR3_SHADER_INCLUDE}/graphics/gr2_fsr3/shaders)
    set(GR2_FSR3_SHADER_HEADERS "")
    # Arguments after the name are macros the shader is built with.
    function(gr2_fsr3_add_shader FILENAME SHADER_NAME)
        set(HEADER_FILE ${GR2_FSR3_SHADER_DIR}/${SHADER_NAME}.h)
        set(DEFINES "")
        foreach(DEFINE ${ARGN})
            list(APPEND DEFINES -D${DEFINE})
        endforeach()
        set(DEPFILE ${HEADER_FILE}.d)
        string(TOUPPER ${SHADER_NAME} SYMBOL)
        add_custom_command(
            OUTPUT ${HEADER_FILE}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${GR2_FSR3_SHADER_DIR}
            COMMAND ${GR2_FSR3_GLSLANG} --target-env vulkan1.3 ${DEFINES}
                    -o ${HEADER_FILE} --vn ${SYMBOL} --depfile ${DEPFILE}
                    ${GR2_FSR3_DIR}/shaders/${FILENAME}
            COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${HEADER_FILE}" -P ${GR2_FSR3_HEADER_SCRIPT}
            DEPENDS ${GR2_FSR3_GLSLANG} ${GR2_FSR3_DIR}/shaders/${FILENAME}
                    ${GR2_FSR3_HEADER_SCRIPT}
            DEPFILE ${DEPFILE}
            BYPRODUCTS ${DEPFILE}
            COMMENT "Building GR2 FSR3 shader ${FILENAME}"
            VERBATIM)
        set(GR2_FSR3_SHADER_HEADERS ${GR2_FSR3_SHADER_HEADERS} ${HEADER_FILE} PARENT_SCOPE)
    endfunction()
    gr2_fsr3_add_shader(fsr4_inputs.comp fsr4_inputs_comp)
    gr2_fsr3_add_shader(fsr4_main_1080_pass0.comp fsr4_main_1080_pass0_comp)
    gr2_fsr3_add_shader(fsr4_main_1080_pass2.comp fsr4_main_1080_pass2_comp)
    gr2_fsr3_add_shader(fsr4_main_1080_pass5.comp fsr4_main_1080_pass5_comp)
    gr2_fsr3_add_shader(fsr4_main_1080_pass7.comp fsr4_main_1080_pass7_comp)
    gr2_fsr3_add_shader(fsr4_main_1080_pass9.comp fsr4_main_1080_pass9_comp)
    gr2_fsr3_add_shader(fsr4_main_1080_pass9.comp fsr4_main_1080_pass9_srgb_comp SRGB)
    gr2_fsr3_add_shader(fsr4_main_1080_pass2_fma.comp fsr4_main_1080_pass2_fma_comp)
    gr2_fsr3_add_shader(fsr4_main_1080_pass5_fma.comp fsr4_main_1080_pass5_fma_comp)
    gr2_fsr3_add_shader(fsr4_main_1080_pass7_fma.comp fsr4_main_1080_pass7_fma_comp)
    gr2_fsr3_add_shader(fsr4_main_1080_pass9_fma.comp fsr4_main_1080_pass9_fma_comp)
    add_custom_target(gr2_fsr3_shaders
        DEPENDS ${GR2_FSR3_SHADER_HEADERS}
        SOURCES
            ${GR2_FSR3_DIR}/shaders/fsr4_inputs.comp
            ${GR2_FSR3_DIR}/shaders/fsr4_main_1080_pass0.comp
            ${GR2_FSR3_DIR}/shaders/fsr4_main_1080_pass2.comp
            ${GR2_FSR3_DIR}/shaders/fsr4_main_1080_pass5.comp
            ${GR2_FSR3_DIR}/shaders/fsr4_main_1080_pass7.comp
            ${GR2_FSR3_DIR}/shaders/fsr4_main_1080_pass9.comp
            ${GR2_FSR3_DIR}/shaders/fsr4_main_1080_pass2_fma.comp
            ${GR2_FSR3_DIR}/shaders/fsr4_main_1080_pass5_fma.comp
            ${GR2_FSR3_DIR}/shaders/fsr4_main_1080_pass7_fma.comp
            ${GR2_FSR3_DIR}/shaders/fsr4_main_1080_pass9_fma.comp)

    # vk_trampolines.cpp defines the Vulkan commands that the FidelityFX libraries and the FSR 4
    # provider call.
    target_sources(gr2port PRIVATE
        ${GR2_FSR3_DIR}/dlss_ngx.cpp
        ${GR2_FSR3_DIR}/dlss_ngx.h
        ${GR2_FSR3_DIR}/fsr4_assets.cpp
        ${GR2_FSR3_DIR}/fsr4_assets.h
        ${GR2_FSR3_DIR}/fsr4_context.h
        ${GR2_FSR3_DIR}/fsr4_lite_bridge.h
        ${GR2_FSR3_DIR}/vk_trampolines.cpp
        ${GR2_FSR3_DIR}/vk_trampolines.h
        ${GR2_FSR3_DIR}/xess_vulkan.h)
    # The NGX headers and NGX's static library of the DLSS SDK 310.9.16, which loads the driver's NGX
    # core; the DLSS library loads at run time from the dlss folder of the user folder.
    set(GR2_DLSS_ROOT ${PROJECT_SOURCE_DIR}/third_party/dlss)
    target_include_directories(gr2port PRIVATE ${GR2_DLSS_ROOT}/include)
    if (WIN32)
        target_link_libraries(gr2port PRIVATE ${GR2_DLSS_ROOT}/lib/Windows_x86_64/nvsdk_ngx_d.lib)
    else()
        target_link_libraries(gr2port PRIVATE
            ${GR2_DLSS_ROOT}/lib/Linux_x86_64/libnvsdk_ngx.a ${CMAKE_DL_LIBS})
    endif()
    # XeSS's MIT headers; libxess.dll loads at run time from the xess folder of the user folder.
    if (WIN32)
        target_sources(gr2port PRIVATE ${GR2_FSR3_DIR}/xess_vulkan.cpp)
        target_include_directories(gr2port PRIVATE ${PROJECT_SOURCE_DIR}/third_party/xess/include)
    endif()
    target_include_directories(gr2port PRIVATE ${GR2_FSR3_SHADER_INCLUDE})
    add_dependencies(gr2port gr2_fsr3_shaders)
    target_link_libraries(gr2port PRIVATE gr2_ffx_portable gr2_ffx_framegen_3_1_6 gr2_fsr4_bridge)
    target_compile_definitions(gr2port PRIVATE ENABLE_GR2_FSR3=1)
endif()
