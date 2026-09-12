#pragma once

#include <dlssnr/DlssNr_Upscaler.h>
#include <NVNGX_Parameter.h>
#include <proxies/FfxApi_Proxy.h>
#include <proxies/XeSS_Proxy.h>
#include <fsr2/ffx_fsr2.h>
#include <fsr2/dx12/ffx_fsr2_dx12.h>

namespace DlssNr
{
// Only the private NR carrier uses this adapter. Reuse the existing runtime loaders and linked
// FSR2 backend without entering IFeature's game settings, post-processing, overlay or NR hooks.
// The owning Generation must wait for its GPU completion marker before destroying this object.
class PrivateUpscalerDx12
{
    PrivateUpscaler backend;
    FfxFsr2Context fsr2 {};
    void* scratch = nullptr;
    bool fsr2Ready = false;
    ffxContext ffx = nullptr;
    xess_context_handle_t xess = nullptr;
    unsigned width = 0, height = 0, outWidth = 0, outHeight = 0;

    static unsigned UInt(NVSDK_NGX_Parameter* p, const char* key)
    {
        unsigned value = 0;
        p->Get(key, &value);
        return value;
    }
    static float Float(NVSDK_NGX_Parameter* p, const char* key, float fallback)
    {
        float value = fallback;
        p->Get(key, &value);
        return std::isfinite(value) ? value : fallback;
    }
    static ID3D12Resource* Resource(NVSDK_NGX_Parameter* p, const char* key)
    {
        ID3D12Resource* value = nullptr;
        p->Get(key, &value);
        return value;
    }
    static void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after)
    {
        if (before == after)
            return;
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        cmd->ResourceBarrier(1, &b);
    }

    // FSR inputs already use FSR's near/far convention. NGX inputs use the existing OptiScaler
    // camera defaults when guides are absent; reverse those defaults for inverted depth.
    template <class Dispatch> void FrameParameters(Dispatch& d, NVSDK_NGX_Parameter* p)
    {
        d.jitterOffset = { Float(p, NVSDK_NGX_Parameter_Jitter_Offset_X, 0),
                           Float(p, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0) };
        d.motionVectorScale = { Float(p, NVSDK_NGX_Parameter_MV_Scale_X, 1),
                                Float(p, NVSDK_NGX_Parameter_MV_Scale_Y, 1) };
        d.renderSize = { width, height };
        d.frameTimeDelta = std::max(Float(p, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 16.67f), 0.01f);
        d.preExposure = 1.0f;
        d.reset = UInt(p, NVSDK_NGX_Parameter_Reset) != 0;
        d.cameraNear = Float(p, "FSR.cameraNear", 0.1f);
        d.cameraFar = Float(p, "FSR.cameraFar", 1000.0f);
        d.cameraFovAngleVertical = Float(p, OptiKeys::FSR_CameraFovVertical, 1.04719755f);
        d.viewSpaceToMetersFactor = Float(p, "FSR.viewSpaceToMetersFactor", 1.0f);
        // Zero initialization leaves sharpening, reactive masks and auto exposure disabled.
    }

  public:
    explicit PrivateUpscalerDx12(PrivateUpscaler selected) : backend(selected) {}
    PrivateUpscalerDx12(const PrivateUpscalerDx12&) = delete;
    PrivateUpscalerDx12& operator=(const PrivateUpscalerDx12&) = delete;
    ~PrivateUpscalerDx12()
    {
        if (fsr2Ready)
            ffxFsr2ContextDestroy(&fsr2);
        free(scratch);
        if (ffx)
            FfxApiProxy::D3D12_DestroyContext(&ffx, nullptr);
        if (xess)
            XeSSProxy::DestroyContext()(xess);
    }

    bool Init(ID3D12Device* device, NVSDK_NGX_Parameter* p)
    {
        width = UInt(p, NVSDK_NGX_Parameter_Width);
        height = UInt(p, NVSDK_NGX_Parameter_Height);
        outWidth = UInt(p, NVSDK_NGX_Parameter_OutWidth);
        outHeight = UInt(p, NVSDK_NGX_Parameter_OutHeight);
        const auto flags = UInt(p, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags);
        const bool inverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
        const bool jittered = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
        const bool highMv = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) == 0;
        ScopedSkipSpoofingGlobal skipSpoofing {};
        ScopedSkipHeapCapture skipCapture {};
        if (backend == PrivateUpscaler::FSR22)
        {
            FfxFsr2ContextDescription d {};
            const auto size = ffxFsr2GetScratchMemorySizeDX12();
            scratch = calloc(size, 1);
            if (!scratch || ffxFsr2GetInterfaceDX12(&d.callbacks, device, scratch, size) != FFX_OK)
                return false;
            d.device = ffxGetDeviceDX12(device);
            d.maxRenderSize = { width, height };
            d.displaySize = { outWidth, outHeight };
            if (inverted)
                d.flags |= FFX_FSR2_ENABLE_DEPTH_INVERTED;
            if (jittered)
                d.flags |= FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
            if (highMv)
                d.flags |= FFX_FSR2_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
            fsr2Ready = ffxFsr2ContextCreate(&fsr2, &d) == FFX_OK;
            return fsr2Ready;
        }
        if (backend == PrivateUpscaler::FFX)
        {
            FfxApiProxy::InitFfxDx12();
            if (!FfxApiProxy::IsSRReady(false))
                return false;
            ffxCreateBackendDX12Desc dx {};
            dx.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
            dx.device = device;
            ffxCreateContextDescUpscale d {};
            d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            d.header.pNext = &dx.header;
            d.maxRenderSize = { width, height };
            d.maxUpscaleSize = { outWidth, outHeight };
            if (inverted)
                d.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
            if (jittered)
                d.flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
            if (highMv)
                d.flags |= FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
            // Let the loaded runtime choose its supported provider. Do not change the game's
            // FfxUpscalerIndex or provider list, and do not promise FSR4 on unsupported hardware.
            return FfxApiProxy::D3D12_CreateContext(&ffx, &d.header, nullptr) == FFX_API_RETURN_OK && ffx;
        }
        if (backend == PrivateUpscaler::XeSS)
        {
            XeSSProxy::InitXeSS();
            if (!XeSSProxy::D3D12CreateContext() || !XeSSProxy::D3D12Init() || !XeSSProxy::D3D12Execute() ||
                !XeSSProxy::DestroyContext() || !XeSSProxy::SetVelocityScale() ||
                !XeSSProxy::GetOptimalInputResolution())
                return false;
            if (XeSSProxy::D3D12CreateContext()(device, &xess) != XESS_RESULT_SUCCESS || !xess)
                return false;
            xess_d3d12_init_params_t d {};
            d.outputResolution = { outWidth, outHeight };
            d.initFlags = XESS_INIT_FLAG_LDR_INPUT_COLOR | XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE;
            if (inverted)
                d.initFlags |= XESS_INIT_FLAG_INVERTED_DEPTH;
            if (jittered)
                d.initFlags |= XESS_INIT_FLAG_JITTERED_MV;
            if (highMv)
                d.initFlags |= XESS_INIT_FLAG_HIGH_RES_MV;
            // Select the closest supported quality mode whose dynamic input range includes the
            // actual carrier size. NGX quality enums and XeSS ratios are not interchangeable.
            bool found = false;
            unsigned long long best = ~0ull;
            for (auto quality :
                 { XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS, XESS_QUALITY_SETTING_ULTRA_QUALITY,
                   XESS_QUALITY_SETTING_QUALITY, XESS_QUALITY_SETTING_BALANCED, XESS_QUALITY_SETTING_PERFORMANCE,
                   XESS_QUALITY_SETTING_ULTRA_PERFORMANCE })
            {
                xess_2d_t optimal {}, minimum {}, maximum {};
                if (XeSSProxy::GetOptimalInputResolution()(xess, &d.outputResolution, quality, &optimal, &minimum,
                                                           &maximum) != XESS_RESULT_SUCCESS)
                    continue;
                if (width < minimum.x || height < minimum.y || width > maximum.x || height > maximum.y)
                    continue;
                auto distance = static_cast<unsigned long long>(std::abs((long long) optimal.x - width) +
                                                                std::abs((long long) optimal.y - height));
                if (distance < best)
                {
                    best = distance;
                    d.qualitySetting = quality;
                    found = true;
                }
            }
            return found && XeSSProxy::D3D12Init()(xess, &d) == XESS_RESULT_SUCCESS;
        }
        return false;
    }

    // Snapshot camera values at the before seam, before the main upscaler can rewrite its table.
    static void CopyCamera(NVSDK_NGX_Parameter* source, NVSDK_NGX_Parameter* dest, unsigned flags, unsigned outWidth,
                           unsigned outHeight)
    {
        const auto& cfg = *Config::Instance();
        const bool inverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
        float nearPlane = cfg.FsrCameraNear.value_or_default(), farPlane = cfg.FsrCameraFar.value_or_default();
        if (inverted)
            std::swap(nearPlane, farPlane);
        dest->Set("FSR.cameraNear", Float(source, "FSR.cameraNear", nearPlane));
        dest->Set("FSR.cameraFar", Float(source, "FSR.cameraFar", farPlane));
        constexpr float radians = 0.01745329252f;
        float fov = cfg.FsrVerticalFov.has_value() ? cfg.FsrVerticalFov.value() * radians
                    : cfg.FsrHorizontalFov.value_or_default() > 0.0f
                        ? 2.0f * std::atan(std::tan(cfg.FsrHorizontalFov.value() * radians * 0.5f) * (float) outHeight /
                                           outWidth)
                        : 60.0f * radians;
        dest->Set(OptiKeys::FSR_CameraFovVertical, Float(source, OptiKeys::FSR_CameraFovVertical, fov));
        dest->Set("FSR.viewSpaceToMetersFactor", Float(source, "FSR.viewSpaceToMetersFactor", 1.0f));
    }

    bool Evaluate(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* p)
    {
        auto* color = Resource(p, NVSDK_NGX_Parameter_Color);
        auto* output = Resource(p, NVSDK_NGX_Parameter_Output);
        auto* depth = Resource(p, NVSDK_NGX_Parameter_Depth);
        auto* motion = Resource(p, NVSDK_NGX_Parameter_MotionVectors);
        auto* exposure = Resource(p, NVSDK_NGX_Parameter_ExposureTexture);
        if (!color || !output || !depth || !motion || !exposure)
            return false;
        // Private textures have known states. Game guides use the same arrival-state contract as
        // OptiScaler's normal backends, and are restored even when evaluation fails.
        const auto& cfg = *Config::Instance();
        const auto depthState =
            (D3D12_RESOURCE_STATES) cfg.DepthResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const auto motionState =
            (D3D12_RESOURCE_STATES) cfg.MVResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, depth, depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, motion, motionState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        bool result = false;
        if (backend == PrivateUpscaler::FSR22 && fsr2Ready)
        {
            FfxFsr2DispatchDescription d {};
            d.commandList = ffxGetCommandListDX12(cmd);
            d.color = ffxGetResourceDX12(&fsr2, color);
            d.depth = ffxGetResourceDX12(&fsr2, depth);
            d.motionVectors = ffxGetResourceDX12(&fsr2, motion);
            d.exposure = ffxGetResourceDX12(&fsr2, exposure);
            d.output = ffxGetResourceDX12(&fsr2, output, nullptr, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            FrameParameters(d, p);
            result = ffxFsr2ContextDispatch(&fsr2, &d) == FFX_OK;
        }
        else if (backend == PrivateUpscaler::FFX && ffx)
        {
            ffxDispatchDescUpscale d {};
            d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            d.commandList = cmd;
            d.color = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            d.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            d.motionVectors = ffxApiGetResourceDX12(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            d.exposure = ffxApiGetResourceDX12(exposure, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            d.output = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            d.upscaleSize = { outWidth, outHeight };
            FrameParameters(d, p);
            result = FfxApiProxy::D3D12_Dispatch(&ffx, &d.header) == FFX_API_RETURN_OK;
        }
        else if (backend == PrivateUpscaler::XeSS && xess)
        {
            xess_d3d12_execute_params_t d {};
            d.pColorTexture = color;
            d.pOutputTexture = output;
            d.pDepthTexture = depth;
            d.pVelocityTexture = motion;
            d.pExposureScaleTexture = exposure;
            d.inputWidth = width;
            d.inputHeight = height;
            d.jitterOffsetX = Float(p, NVSDK_NGX_Parameter_Jitter_Offset_X, 0);
            d.jitterOffsetY = Float(p, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0);
            d.resetHistory = UInt(p, NVSDK_NGX_Parameter_Reset);
            d.exposureScale = 1.0f;
            result =
                XeSSProxy::SetVelocityScale()(xess, Float(p, NVSDK_NGX_Parameter_MV_Scale_X, 1),
                                              Float(p, NVSDK_NGX_Parameter_MV_Scale_Y, 1)) == XESS_RESULT_SUCCESS &&
                XeSSProxy::D3D12Execute()(xess, cmd, &d) == XESS_RESULT_SUCCESS;
        }
        Barrier(cmd, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, motionState);
        Barrier(cmd, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depthState);
        return result;
    }
};
} // namespace DlssNr
