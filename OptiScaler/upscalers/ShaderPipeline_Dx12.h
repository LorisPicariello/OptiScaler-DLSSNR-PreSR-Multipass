#pragma once

#include <d3d12.h>
#include <functional>
#include <cstdint>
#include <vector>
#include <array>
#include <nvsdk_ngx.h>

class DlssNr_Dx12;

// The held frame keeps its sampling/exposure metadata. Original call parameters
// are restored after NR and SR, including failed evaluates.
class NrHoldParameters_Dx12
{
    struct Value
    {
        const char* name;
        float held = 0, live = 0;
        bool valid = false, changed = false;
    };
    std::array<Value, 7> values {{
        { NVSDK_NGX_Parameter_Jitter_Offset_X }, { NVSDK_NGX_Parameter_Jitter_Offset_Y },
        { NVSDK_NGX_Parameter_MV_Scale_X }, { NVSDK_NGX_Parameter_MV_Scale_Y },
        { NVSDK_NGX_Parameter_DLSS_Pre_Exposure }, { NVSDK_NGX_Parameter_DLSS_Exposure_Scale },
        { NVSDK_NGX_Parameter_FrameTimeDeltaInMsec }
    }};
    struct Region
    {
        const char* name;
        unsigned int held = 0, live = 0;
        bool valid = false, changed = false;
    };
    std::array<Region, 8> regions {{
        { NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width },
        { NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height },
        { NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X },
        { NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y },
        { NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X },
        { NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y },
        { NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X },
        { NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y }
    }};
    unsigned int liveReset = 0;
    bool resetChanged = false;

  public:
    void Capture(NVSDK_NGX_Parameter* params)
    {
        for (auto& value : values)
            value.valid = params->Get(value.name, &value.held) == NVSDK_NGX_Result_Success;
        for (auto& region : regions)
            region.valid = params->Get(region.name, &region.held) == NVSDK_NGX_Result_Success;
    }
    void Apply(NVSDK_NGX_Parameter* params, bool frozen = true)
    {
        for (auto& value : values)
        {
            value.changed = frozen && value.valid && params->Get(value.name, &value.live) == NVSDK_NGX_Result_Success;
            if (value.changed)
                params->Set(value.name, value.held);
        }
        liveReset = 0;
        for (auto& region : regions)
        {
            region.changed = frozen && region.valid && params->Get(region.name, &region.live) == NVSDK_NGX_Result_Success;
            if (region.changed)
                params->Set(region.name, region.held);
        }
        params->Get(NVSDK_NGX_Parameter_Reset, &liveReset);
        params->Set(NVSDK_NGX_Parameter_Reset, 1u);
        resetChanged = true;
    }
    void Restore(NVSDK_NGX_Parameter* params)
    {
        for (auto& value : values)
        {
            if (value.changed)
                params->Set(value.name, value.live);
            value.changed = false;
        }
        if (resetChanged)
            params->Set(NVSDK_NGX_Parameter_Reset, liveReset);
        for (auto& region : regions)
        {
            if (region.changed)
                params->Set(region.name, region.live);
            region.changed = false;
        }
        resetChanged = false;
    }
};

inline ID3D12Resource* GetUpscalerResource_Dx12(NVSDK_NGX_Parameter* parameters, const char* name)
{
    ID3D12Resource* resource = nullptr;
    parameters->Get(name, &resource);
    if (resource == nullptr)
    {
        void* untyped = nullptr;
        parameters->Get(name, &untyped);
        resource = static_cast<ID3D12Resource*>(untyped);
    }
    return resource;
}

inline void SetUpscalerResource_Dx12(NVSDK_NGX_Parameter* parameters, const char* name, ID3D12Resource* resource)
{
    // A DX11 bridge can retain the driver's DX11 parameter table. That table rejects
    // DX12-typed access, including Set, but accepts the bridge's resources as void*.
    ID3D12Resource* previous = nullptr;
    if (parameters->Get(name, &previous) == NVSDK_NGX_Result_Success)
        parameters->Set(name, resource);
    else
        parameters->Set(name, static_cast<void*>(resource));
}

// The same resource routing is used before and after the upscaler, including API bridges.
struct ShaderPass_Dx12
{
    std::function<ID3D12Resource*(ID3D12Resource* nextOutput)> Setup;
    std::function<bool(ID3D12Resource* input, ID3D12Resource* output)> Dispatch;
    ID3D12Resource* inputBuffer = nullptr;
    ID3D12Resource* outputBuffer = nullptr;
};

using ShaderPipeline_Dx12 = std::vector<ShaderPass_Dx12>;

inline ID3D12Resource* SetupShaderPipeline(ShaderPipeline_Dx12& pipeline, ID3D12Resource* output)
{
    for (auto it = pipeline.rbegin(); it != pipeline.rend(); ++it)
    {
        it->inputBuffer = it->outputBuffer = nullptr;
        if (auto* input = it->Setup(output))
        {
            it->inputBuffer = input;
            it->outputBuffer = output;
            output = input;
        }
    }
    return output;
}

inline bool DispatchShaderPipeline(ShaderPipeline_Dx12& pipeline)
{
    for (auto& pass : pipeline)
        if (pass.inputBuffer && pass.outputBuffer && !pass.Dispatch(pass.inputBuffer, pass.outputBuffer))
            return false;
    return true;
}

// These adapters only translate NGX inputs and resource states. The shader owns all NR resources/history.
ShaderPass_Dx12 MakeDlssNrPass(DlssNr_Dx12& shader, ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                               NVSDK_NGX_Parameter* parameters, bool beforeUpscale, unsigned int featureFlags,
                               ID3D12CommandQueue* timingQueue = nullptr, bool interop = false,
                               bool rayReconstruction = false, uint64_t submissionEpoch = 0);
ID3D12Resource* PrepareDlssNrInput(DlssNr_Dx12& shader, ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                                   NVSDK_NGX_Parameter* parameters, unsigned int featureFlags,
                                   ID3D12CommandQueue* timingQueue = nullptr, bool interop = false,
                                   bool rayReconstruction = false, uint64_t submissionEpoch = 0);

// Parameter restoration belongs to the upscaler call, regardless of which shader stages ran.
class RestoreUpscalerResources_Dx12
{
    NVSDK_NGX_Parameter* _parameters;
    ID3D12Resource* _color = nullptr;
    ID3D12Resource* _output = nullptr;
    bool _untypedColor = false;
    bool _untypedOutput = false;

  public:
    explicit RestoreUpscalerResources_Dx12(NVSDK_NGX_Parameter* parameters) : _parameters(parameters)
    {
        parameters->Get(NVSDK_NGX_Parameter_Color, &_color);
        parameters->Get(NVSDK_NGX_Parameter_Output, &_output);
        _untypedColor = _color == nullptr;
        _untypedOutput = _output == nullptr;
        _color = GetUpscalerResource_Dx12(parameters, NVSDK_NGX_Parameter_Color);
        _output = GetUpscalerResource_Dx12(parameters, NVSDK_NGX_Parameter_Output);
    }
    ~RestoreUpscalerResources_Dx12()
    {
        if (_untypedColor)
            _parameters->Set(NVSDK_NGX_Parameter_Color, static_cast<void*>(_color));
        else
            _parameters->Set(NVSDK_NGX_Parameter_Color, _color);
        if (_untypedOutput)
            _parameters->Set(NVSDK_NGX_Parameter_Output, static_cast<void*>(_output));
        else
            _parameters->Set(NVSDK_NGX_Parameter_Output, _output);
    }
};
