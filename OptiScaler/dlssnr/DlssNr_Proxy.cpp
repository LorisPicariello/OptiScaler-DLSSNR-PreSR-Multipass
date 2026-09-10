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

ProxyState g_proxy;

struct RetiredState
{
    ProxyState state;
    unsigned int framesLeft = 32;
};

std::vector<RetiredState> g_retired;

void DestroyState(ProxyState& state)
{
    if (state.feature != nullptr && NVNGXProxy::D3D12_ReleaseFeature() != nullptr)
        NVNGXProxy::D3D12_ReleaseFeature()(state.feature);

    if (state.params != nullptr && NVNGXProxy::D3D12_DestroyParameters() != nullptr)
        NVNGXProxy::D3D12_DestroyParameters()(state.params);

    state = {};
}

void RetireState()
{
    if (g_proxy.feature != nullptr || g_proxy.params != nullptr)
        g_retired.push_back({ g_proxy });

    g_proxy = {};
}

void TickRetired()
{
    // Match the existing DLSS-NR resource retirement window. Feature creation/evaluation
    // records GPU work, so replacing a feature must not destroy it on that same frame.
    for (size_t i = 0; i < g_retired.size();)
    {
        if (--g_retired[i].framesLeft > 0)
        {
            ++i;
            continue;
        }

        DestroyState(g_retired[i].state);
        g_retired.erase(g_retired.begin() + i);
    }
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
bool Available()
{
    return NVNGXProxy::IsDx12Inited() && NVNGXProxy::D3D12_GetCapabilityParameters() != nullptr &&
           NVNGXProxy::D3D12_DestroyParameters() != nullptr && NVNGXProxy::D3D12_ReleaseFeature() != nullptr &&
           NVNGXProxy::D3D12_CreateFeature() != nullptr && NVNGXProxy::D3D12_EvaluateFeature() != nullptr;
}

void Release()
{
    DestroyState(g_proxy);

    for (auto& retired : g_retired)
        DestroyState(retired.state);

    g_retired.clear();
}

void RetryAfterFailure() { RetireState(); }

unsigned int Run(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, ID3D12Resource* color, ID3D12Resource* depth,
                 ID3D12Resource* motion, ID3D12Resource* output, unsigned int width, unsigned int height,
                 unsigned int guideWidth, unsigned int guideHeight, bool depthInverted, bool reset, float mvScaleX,
                 float mvScaleY, bool* evaluated)
{
    if (evaluated != nullptr)
        *evaluated = false;

    if (g_proxy.failed || cmdList == nullptr || device == nullptr || color == nullptr || depth == nullptr ||
        motion == nullptr || output == nullptr || width == 0 || height == 0 || guideWidth == 0 || guideHeight == 0)
        return 0;

    if (!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device))
        return 0;

    if (!Available())
        return 0;

    TickRetired();

    const Config& cfg = *Config::Instance();
    const auto settings = ReadSettings(cfg, width, height);

    if (g_proxy.feature != nullptr && (g_proxy.settings != settings || g_proxy.device != device))
        RetireState();

    if (g_proxy.params == nullptr)
    {
        // A dedicated parameter map populated with NGX capabilities. Unlike the deprecated
        // GetParameters API, GetCapabilityParameters transfers ownership to the caller.
        const auto allocated = NVNGXProxy::D3D12_GetCapabilityParameters()(&g_proxy.params);
        if (allocated != NVSDK_NGX_Result_Success || g_proxy.params == nullptr)
        {
            DestroyState(g_proxy);
            g_proxy.failed = true;
            LOG_ERROR("DLSS-NR (proxy): the NGX core refused its capability parameters");
            return (unsigned int) (allocated == NVSDK_NGX_Result_Success ? NVSDK_NGX_Result_Fail : allocated);
        }
    }

    if (g_proxy.feature == nullptr)
    {
        SetCreationParameters(g_proxy.params, cfg, width, height);

        const auto created =
            NVNGXProxy::D3D12_CreateFeature()(cmdList, (NVSDK_NGX_Feature) 18, g_proxy.params, &g_proxy.feature);

        if (created != NVSDK_NGX_Result_Success || g_proxy.feature == nullptr)
        {
            RetireState();
            g_proxy.failed = true;
            LOG_ERROR("DLSS-NR (proxy): CreateFeature(18) failed 0x{:X} -- falling back is the "
                      "caller's decision",
                      (unsigned int) created);
            return (unsigned int) (created == NVSDK_NGX_Result_Success ? NVSDK_NGX_Result_Fail : created);
        }

        g_proxy.settings = settings;
        g_proxy.device = device;
        LOG_INFO("DLSS-NR (proxy): feature created at {}x{} through the driver's nvngx -- no "
                 "forwarder in this path",
                 width, height);

        // As on the direct backend, let creation work be submitted before evaluation.
        return (unsigned int) NVSDK_NGX_Result_Success;
    }

    NVSDK_NGX_Parameter* params = g_proxy.params;

    SetResource(params, "DLSSNR.Color", color);
    SetResource(params, "DLSSNR.Depth", depth);
    SetResource(params, "DLSSNR.MVec", motion);
    SetResource(params, "DLSSNR.Output", output);

    SetUInt(params, "DLSSNR.Enabled", 1u);
    SetUInt(params, "DLSSNR.Width", width);
    SetUInt(params, "DLSSNR.Height", height);
    SetUInt(params, "DLSSNR.DepthInverted", depthInverted ? 1u : 0u);
    SetUInt(params, "DLSSNR.Reset", (reset || g_proxy.reset) ? 1u : 0u);

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
    SetUInt(params, "DLSSNR.DepthSubrectBaseX", 0u);
    SetUInt(params, "DLSSNR.DepthSubrectBaseY", 0u);
    SetUInt(params, "DLSSNR.DepthSubrectWidth", guideWidth);
    SetUInt(params, "DLSSNR.DepthSubrectHeight", guideHeight);
    SetUInt(params, "DLSSNR.MVecSubrectBaseX", 0u);
    SetUInt(params, "DLSSNR.MVecSubrectBaseY", 0u);
    SetUInt(params, "DLSSNR.MVecSubrectWidth", guideWidth);
    SetUInt(params, "DLSSNR.MVecSubrectHeight", guideHeight);

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

    const auto result =
        NVNGXProxy::D3D12_EvaluateFeature()(cmdList, g_proxy.feature, params, nullptr);

    if (result == NVSDK_NGX_Result_Success)
    {
        g_proxy.reset = false;
        if (evaluated != nullptr)
            *evaluated = true;
    }
    else
    {
        g_proxy.failed = true;
    }

    return (unsigned int) result;
}
} // namespace Proxy
} // namespace DlssNr

