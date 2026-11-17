#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>
#include "utils/Logger.h"
#include <cstdlib>
#include <string_view>
#include <sys/sysctl.h>

static bool ShotletImGuiIsAtLeastMacOS14() {
    static bool initialized = false;
    static bool isAtLeast14 = true;
    if (initialized) {
        return isAtLeast14;
    }

    initialized = true;

    size_t size = 0;
    if (sysctlbyname("kern.osproductversion", nullptr, &size, nullptr, 0) != 0 || size == 0) {
        return isAtLeast14;
    }

    std::string version(size, '\0');
    if (sysctlbyname("kern.osproductversion", version.data(), &size, nullptr, 0) != 0 || size == 0) {
        return isAtLeast14;
    }

    if (!version.empty() && version.back() == '\0') {
        version.pop_back();
    }

    const auto majorEnd = version.find('.');
    const auto majorPart = std::string_view(version).substr(0, majorEnd);
    char* parseEnd = nullptr;
    const long major = std::strtol(std::string(majorPart).c_str(), &parseEnd, 10);
    if (parseEnd == nullptr || *parseEnd != '\0') {
        return isAtLeast14;
    }

    isAtLeast14 = major >= 14;
    return isAtLeast14;
}

static void ShotletImGuiLogOnce(bool& flag, const char* message) {
    if (flag) {
        return;
    }
    flag = true;
    shotlet::Logger::Error("{}", message);
}

static SDL_GPUShaderFormat ShotletImGuiPreferMslShaderFormats(SDL_GPUDevice* device) {
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
    static bool loggedNativeFormats = false;
    if (ShotletImGuiIsAtLeastMacOS14()) {
        if (!loggedNativeFormats) {
            loggedNativeFormats = true;
            shotlet::Logger::Info("[Shotlet] ImGui SDL_GPU using native shader formats: 0x{:x}", formats);
        }
        return formats;
    }
    if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        if (!loggedNativeFormats) {
            loggedNativeFormats = true;
            shotlet::Logger::Info(
                "[Shotlet] ImGui SDL_GPU forcing MSL shader format on older macOS; native formats: 0x{:x}",
                formats);
        }
        return SDL_GPU_SHADERFORMAT_MSL;
    }
    return formats;
}

static bool g_ShotletImGuiHasGraphicsPipeline = false;

static SDL_GPUShader* ShotletImGuiCreateGPUShader(SDL_GPUDevice* device,
                                                const SDL_GPUShaderCreateInfo* createInfo) {
    SDL_GPUShader* shader = SDL_CreateGPUShader(device, createInfo);
    if (!shader) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            shotlet::Logger::Error(
                "[Shotlet] ImGui SDL_GPU shader creation failed: driver={} native_formats=0x{:x} stage={} format=0x{:x} code_size={} error={}",
                SDL_GetGPUDeviceDriver(device),
                SDL_GetGPUShaderFormats(device),
                createInfo ? static_cast<int>(createInfo->stage) : -1,
                createInfo ? createInfo->format : SDL_GPU_SHADERFORMAT_INVALID,
                createInfo ? createInfo->code_size : 0,
                SDL_GetError());
        }
    }
    return shader;
}

static SDL_GPUGraphicsPipeline* ShotletImGuiCreateGPUGraphicsPipeline(
    SDL_GPUDevice* device,
    const SDL_GPUGraphicsPipelineCreateInfo* createInfo) {
    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device, createInfo);
    if (!pipeline) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            shotlet::Logger::Error(
                "[Shotlet] ImGui SDL_GPU graphics pipeline creation failed: driver={} vertex_shader={} fragment_shader={} color_format={} samples={} error={}",
                SDL_GetGPUDeviceDriver(device),
                static_cast<const void*>(createInfo ? createInfo->vertex_shader : nullptr),
                static_cast<const void*>(createInfo ? createInfo->fragment_shader : nullptr),
                createInfo && createInfo->target_info.num_color_targets > 0 &&
                        createInfo->target_info.color_target_descriptions
                    ? static_cast<int>(createInfo->target_info.color_target_descriptions[0].format)
                    : -1,
                createInfo ? static_cast<int>(createInfo->multisample_state.sample_count) : -1,
                SDL_GetError());
        }
    }
    return pipeline;
}

static void ShotletImGuiBindGraphicsPipeline(SDL_GPURenderPass* renderPass,
                                           SDL_GPUGraphicsPipeline* pipeline) {
    g_ShotletImGuiHasGraphicsPipeline = pipeline != nullptr;
    if (!pipeline) {
        static bool logged = false;
        ShotletImGuiLogOnce(logged, "[Shotlet] ImGui SDL_GPU render skipped: graphics pipeline is null");
        return;
    }
    SDL_BindGPUGraphicsPipeline(renderPass, pipeline);
}

static void ShotletImGuiDrawGPUIndexedPrimitives(SDL_GPURenderPass* renderPass,
                                               Uint32 numIndices,
                                               Uint32 numInstances,
                                               Uint32 firstIndex,
                                               Sint32 vertexOffset,
                                               Uint32 firstInstance) {
    if (!g_ShotletImGuiHasGraphicsPipeline) {
        return;
    }
    SDL_DrawGPUIndexedPrimitives(renderPass,
                                 numIndices,
                                 numInstances,
                                 firstIndex,
                                 vertexOffset,
                                 firstInstance);
}

#define SDL_GetGPUShaderFormats(device) ShotletImGuiPreferMslShaderFormats(device)
#define SDL_CreateGPUShader(device, create_info) ShotletImGuiCreateGPUShader(device, create_info)
#define SDL_CreateGPUGraphicsPipeline(device, create_info) ShotletImGuiCreateGPUGraphicsPipeline(device, create_info)
#define SDL_BindGPUGraphicsPipeline(render_pass, pipeline) ShotletImGuiBindGraphicsPipeline(render_pass, pipeline)
#define SDL_DrawGPUIndexedPrimitives(render_pass, num_indices, num_instances, first_index, vertex_offset, first_instance) ShotletImGuiDrawGPUIndexedPrimitives(render_pass, num_indices, num_instances, first_index, vertex_offset, first_instance)
#include "../../imgui/backends/imgui_impl_sdlgpu3.cpp"
#undef SDL_DrawGPUIndexedPrimitives
#undef SDL_BindGPUGraphicsPipeline
#undef SDL_CreateGPUGraphicsPipeline
#undef SDL_CreateGPUShader
#undef SDL_GetGPUShaderFormats
