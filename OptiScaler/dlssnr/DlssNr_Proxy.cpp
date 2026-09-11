#include "pch.h"
#include "DlssNr_Proxy.h"

#include <Config.h>
#include <Logger.h>
#include <proxies/NVNGX_Proxy.h>
#include <vector>

namespace
{
// Use the SDK interface so the compiler selects the correct overloaded virtual method.
// Declaration order is not vtable order under the MSVC ABI.
void SetUInt(NVSDK_NGX_Parameter* params, const char* name, unsigned int value) { params->Set(name, value); }

void SetResource(NVSDK_NGX_Parameter* params, const char* name, ID3D12Resource* value) { params->Set(name, value); }

void SetFloat(NVSDK_NGX_Parameter* params, const char* name, float value) { params->Set(name, value); }

struct CreationSettings
{
    unsigned int width, height, preset, style;
    float intensity, localStructure, localTone, skinStructure;
    bool autoMask;

    bool operator==(const CreationSettings&) const = default;
};

CreationSettings ReadSettings(const Config& cfg, unsigned int width, unsigned int height)
{
    return { width,
             height,
             (unsigned int) cfg.DlssNrPreset.value_or_default(),
             (unsigned int) cfg.DlssNrStyle.value_or_default(),
             cfg.DlssNrIntensity.value_or_default(),
             cfg.DlssNrLocalStructure.value_or_default(),
             cfg.DlssNrLocalTone.value_or_default(),
             cfg.DlssNrSkinStructure.value_or_default(),
             cfg.DlssNrAutoMask.value_or_default() };
}

struct ProxyState
{
    NVSDK_NGX_Handle* feature = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;

    CreationSettings settings {};
    ID3D12Device* device = nullptr;
    bool failed = false;
    bool reset = true;
};

struct RetiredState
{
    ProxyState state;
    unsigned int framesLeft = 32;
};

void DestroyState(ProxyState& state)
{
    if (state.feature != nullptr && NVNGXProxy::D3D12_ReleaseFeature() != nullptr)
        NVNGXProxy::D3D12_ReleaseFeature()(state.feature);

    if (state.params != nullptr && NVNGXProxy::D3D12_DestroyParameters() != nullptr)
        NVNGXProxy::D3D12_DestroyParameters()(state.params);

    state = {};
}

// Everything the model reads when the feature is built.
//
// These have to be set before create, not at evaluate. The model reads its tuning once, while
// building the feature; values written only at evaluate are ignored, which is why several of these
// controls appeared to do nothing for a long time.
void SetCreationParameters(NVSDK_NGX_Parameter* params, const Config& cfg, unsigned int width,
                           unsigned int height)
{
    SetUInt(params, "DLSSNR.Enabled", 1u);
    SetUInt(params, "DLSSNR.Width", width);
    SetUInt(params, "DLSSNR.Height", height);
    SetUInt(params, "CreationNodeMask", 1u);
    SetUInt(params, "VisibilityNodeMask", 1u);

    // Set the default preset explicitly, too.
    SetUInt(params, "DLSSNR.Hint.Render.Preset", (unsigned int) cfg.DlssNrPreset.value_or_default());

    SetFloat(params, "DLSSNR.Intensity", cfg.DlssNrIntensity.value_or_default());
    SetUInt(params, "DLSSNR.Style", (unsigned int) cfg.DlssNrStyle.value_or_default());
    SetFloat(params, "DLSSNR.LocalStructureStrength", cfg.DlssNrLocalStructure.value_or_default());
    SetFloat(params, "DLSSNR.LocalToneStrength", cfg.DlssNrLocalTone.value_or_default());
    SetFloat(params, "DLSSNR.SkinStructureStrength", cfg.DlssNrSkinStructure.value_or_default());
    SetUInt(params, "DLSSNR.UseAutoMask", cfg.DlssNrAutoMask.value_or_default() ? 1u : 0u);

    // UI correction at the model's own default: with no UI layer fed to it there is nothing to
    // correct.
    SetUInt(params, "DLSSNR.UICorrection", 1u);
}
} // namespace

namespace DlssNr
{
namespace Proxy
{
struct Context::Impl
{
    ProxyState state;
    std::vector<RetiredState> retiredStates;
    void RetireState();
    void TickRetired();
    void Release();
    unsigned int Run(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, ID3D12Resource* color,
                     ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output, unsigned int width,
                     unsigned int height, unsigned int guideWidth, unsigned int guideHeight,
                     unsigned int motionWidth, unsigned int motionHeight, unsigned int depthBaseX,
                     unsigned int depthBaseY, unsigned int motionBaseX, unsigned int motionBaseY,
                     bool depthInverted, bool reset, float mvScaleX, float mvScaleY, bool* evaluated);
};

void Context::Impl::RetireState()
{
    if (state.feature != nullptr || state.params != nullptr)
        retiredStates.push_back({ state });

    state = {};
}

void Context::Impl::TickRetired()
{
    // Match the existing DLSS-NR resource retirement window. Feature creation/evaluation
    // records GPU work, so replacing a feature must not destroy it on that same frame.
    for (size_t i = 0; i < retiredStates.size();)
    {
        if (--retiredStates[i].framesLeft > 0)
        {
            ++i;
            continue;
        }

        DestroyState(retiredStates[i].state);
        retiredStates.erase(retiredStates.begin() + i);
    }
}

bool Context::Available()
{
    return NVNGXProxy::IsDx12Inited() && NVNGXProxy::D3D12_GetCapabilityParameters() != nullptr &&
           NVNGXProxy::D3D12_DestroyParameters() != nullptr && NVNGXProxy::D3D12_ReleaseFeature() != nullptr &&
           NVNGXProxy::D3D12_CreateFeature() != nullptr && NVNGXProxy::D3D12_EvaluateFeature() != nullptr;
}

void Context::Impl::Release()
{
    DestroyState(state);

    for (auto& retired : retiredStates)
        DestroyState(retired.state);

    retiredStates.clear();
}

void Context::RetryAfterFailure() { _impl->RetireState(); }

unsigned int Context::Impl::Run(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, ID3D12Resource* color,
                                ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output,
                                unsigned int width, unsigned int height, unsigned int guideWidth,
                                unsigned int guideHeight, unsigned int motionWidth, unsigned int motionHeight,
                                unsigned int depthBaseX, unsigned int depthBaseY, unsigned int motionBaseX,
                                unsigned int motionBaseY, bool depthInverted, bool reset, float mvScaleX,
                                float mvScaleY, bool* evaluated)
{
    if (evaluated != nullptr)
        *evaluated = false;

    if (state.failed || cmdList == nullptr || device == nullptr || color == nullptr || depth == nullptr ||
        motion == nullptr || output == nullptr || width == 0 || height == 0 || guideWidth == 0 || guideHeight == 0 ||
        motionWidth == 0 || motionHeight == 0)
        return 0;

    if (!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device))
        return 0;

    if (!Context::Available())
        return 0;

    TickRetired();

    const Config& cfg = *Config::Instance();
    const auto settings = ReadSettings(cfg, width, height);

    if (state.feature != nullptr && (state.settings != settings || state.device != device))
        RetireState();

    if (state.params == nullptr)
    {
        // A dedicated parameter map populated with NGX capabilities. Unlike the deprecated
        // GetParameters API, GetCapabilityParameters transfers ownership to the caller.
        const auto allocated = NVNGXProxy::D3D12_GetCapabilityParameters()(&state.params);
        if (allocated != NVSDK_NGX_Result_Success || state.params == nullptr)
        {
            DestroyState(state);
            state.failed = true;
            LOG_ERROR("DLSS-NR (proxy): the NGX core refused its capability parameters");
            return (unsigned int) (allocated == NVSDK_NGX_Result_Success ? NVSDK_NGX_Result_Fail : allocated);
        }
    }

    if (state.feature == nullptr)
    {
        SetCreationParameters(state.params, cfg, width, height);

        const auto created =
            NVNGXProxy::D3D12_CreateFeature()(cmdList, (NVSDK_NGX_Feature) 18, state.params, &state.feature);

        if (created != NVSDK_NGX_Result_Success || state.feature == nullptr)
        {
            RetireState();
            state.failed = true;
            LOG_ERROR("DLSS-NR (proxy): CreateFeature(18) failed 0x{:X} -- falling back is the "
                      "caller's decision",
                      (unsigned int) created);
            return (unsigned int) (created == NVSDK_NGX_Result_Success ? NVSDK_NGX_Result_Fail : created);
        }

        state.settings = settings;
        state.device = device;
        LOG_INFO("DLSS-NR (proxy): feature created at {}x{} through the driver's nvngx -- no "
                 "forwarder in this path",
                 width, height);

        // As on the direct backend, let creation work be submitted before evaluation.
        return (unsigned int) NVSDK_NGX_Result_Success;
    }

    NVSDK_NGX_Parameter* params = state.params;

    SetResource(params, "DLSSNR.Color", color);
    SetResource(params, "DLSSNR.Depth", depth);
    SetResource(params, "DLSSNR.MVec", motion);
    SetResource(params, "DLSSNR.Output", output);

    SetUInt(params, "DLSSNR.Enabled", 1u);
    SetUInt(params, "DLSSNR.Width", width);
    SetUInt(params, "DLSSNR.Height", height);
    SetUInt(params, "DLSSNR.DepthInverted", depthInverted ? 1u : 0u);
    SetUInt(params, "DLSSNR.Reset", (reset || state.reset) ? 1u : 0u);

    // Colour and output are display resolution; depth and motion come from the game's own DLSS
    // evaluation and may be render resolution, so each resource carries its own subrect.
    SetUInt(params, "DLSSNR.ColorSubrectBaseX", 0u);
    SetUInt(params, "DLSSNR.ColorSubrectBaseY", 0u);
    SetUInt(params, "DLSSNR.ColorSubrectWidth", width);
    SetUInt(params, "DLSSNR.ColorSubrectHeight", height);
    SetUInt(params, "DLSSNR.OutputSubrectBaseX", 0u);
    SetUInt(params, "DLSSNR.OutputSubrectBaseY", 0u);
    SetUInt(params, "DLSSNR.OutputSubrectWidth", width);
    SetUInt(params, "DLSSNR.OutputSubrectHeight", height);
    SetUInt(params, "DLSSNR.DepthSubrectBaseX", depthBaseX);
    SetUInt(params, "DLSSNR.DepthSubrectBaseY", depthBaseY);
    SetUInt(params, "DLSSNR.DepthSubrectWidth", guideWidth);
    SetUInt(params, "DLSSNR.DepthSubrectHeight", guideHeight);
    SetUInt(params, "DLSSNR.MVecSubrectBaseX", motionBaseX);
    SetUInt(params, "DLSSNR.MVecSubrectBaseY", motionBaseY);
    SetUInt(params, "DLSSNR.MVecSubrectWidth", motionWidth);
    SetUInt(params, "DLSSNR.MVecSubrectHeight", motionHeight);

    // The game's own encoding, passed through. Deriving this from the resolutions was a guess, and
    // at native resolution it came out as exactly 1.0 -- so a game using normalised vectors was
    // telling the model that almost nothing had moved.
    SetFloat(params, "DLSSNR.MVecScaleX", mvScaleX);
    SetFloat(params, "DLSSNR.MVecScaleY", mvScaleY);

    SetFloat(params, "DLSSNR.Intensity", cfg.DlssNrIntensity.value_or_default());
    SetUInt(params, "DLSSNR.Style", (unsigned int) cfg.DlssNrStyle.value_or_default());
    SetFloat(params, "DLSSNR.LocalStructureStrength", cfg.DlssNrLocalStructure.value_or_default());
    SetFloat(params, "DLSSNR.LocalToneStrength", cfg.DlssNrLocalTone.value_or_default());
    SetFloat(params, "DLSSNR.SkinStructureStrength", cfg.DlssNrSkinStructure.value_or_default());
    SetUInt(params, "DLSSNR.UseAutoMask", cfg.DlssNrAutoMask.value_or_default() ? 1u : 0u);

    const auto result = NVNGXProxy::D3D12_EvaluateFeature()(cmdList, state.feature, params, nullptr);

    if (result == NVSDK_NGX_Result_Success)
    {
        state.reset = false;
        if (evaluated != nullptr)
            *evaluated = true;
    }
    else
    {
        state.failed = true;
    }

    return (unsigned int) result;
}
Context::Context() : _impl(std::make_unique<Impl>()) {}
Context::~Context() { _impl->Release(); }
void Context::Release() { _impl->Release(); }

unsigned int Context::Run(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, ID3D12Resource* color,
                          ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output, unsigned int width,
                          unsigned int height, unsigned int guideWidth, unsigned int guideHeight,
                          unsigned int motionWidth, unsigned int motionHeight, unsigned int depthBaseX,
                          unsigned int depthBaseY, unsigned int motionBaseX, unsigned int motionBaseY,
                          bool depthInverted, bool reset, float mvScaleX, float mvScaleY, bool* evaluated)
{
    return _impl->Run(cmdList, device, color, depth, motion, output, width, height, guideWidth, guideHeight,
                      motionWidth, motionHeight, depthBaseX, depthBaseY, motionBaseX, motionBaseY,
                      depthInverted, reset, mvScaleX, mvScaleY, evaluated);
}
} // namespace Proxy
} // namespace DlssNr
