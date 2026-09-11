#include <pch.h>

#include <functional>
#include <vector>

#include "IFeature_Dx12.h"
#include "State.h"
#include <dlssnr/DlssNr_ExposureScan.h>
#include <shaders/dlssnr/DlssNr_ActiveColor.h>

namespace
{
bool HasSupportedNrSubrects(NVSDK_NGX_Parameter* parameters, bool beforeUpscale)
{
    const char* offsets[] { NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
                            NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y };
    for (const auto* name : offsets)
    {
        unsigned int offset = 0;
        if (parameters->Get(name, &offset) == NVSDK_NGX_Result_Success && offset != 0)
            return false;
    }
    if (beforeUpscale)
    {
        unsigned int x = 0, y = 0;
        parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &x);
        parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &y);
        if (x != 0 || y != 0)
            return false;
    }
    return true;
}

ID3D12Resource* NrResource(NVSDK_NGX_Parameter* parameters, const char* name, const char* fallback)
{
    ID3D12Resource* resource = GetUpscalerResource_Dx12(parameters, name);
    if (resource == nullptr)
        resource = GetUpscalerResource_Dx12(parameters, fallback);
    return resource;
}

bool CanRunNrBeforeUpscale(NVSDK_NGX_Parameter* parameters)
{
    auto* color = NrResource(parameters, NVSDK_NGX_Parameter_Color, "DLSSD.Color");
    if (color == nullptr || !HasSupportedNrSubrects(parameters, true))
        return false;
    unsigned int width = 0, height = 0;
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &height);
    const auto desc = color->GetDesc();
    return desc.MipLevels == 1 && DlssNr::PreSrColorExtent(desc, width, height).has_value();
}

void NrBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
               D3D12_RESOURCE_STATES after)
{
    if (resource == nullptr || before == after)
        return;
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
    commandList->ResourceBarrier(1, &barrier);
}

struct NrInputStates
{
    D3D12_RESOURCE_STATES color;
    D3D12_RESOURCE_STATES depth;
    D3D12_RESOURCE_STATES motion;
    D3D12_RESOURCE_STATES exposure;
};

NrInputStates NrStates(bool interop)
{
    constexpr auto readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (interop)
        return { readable, readable, readable, readable };

    const auto& cfg = *Config::Instance();
    const bool unreal = State::Instance().NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
                        State::Instance().gameEngine == GameEngineType::Unreal ||
                        (State::Instance().gameQuirks & GameQuirk::ForceUnrealEngine);
    return { static_cast<D3D12_RESOURCE_STATES>(
                 cfg.ColorResourceBarrier.value_or(unreal ? D3D12_RESOURCE_STATE_RENDER_TARGET : readable)),
             static_cast<D3D12_RESOURCE_STATES>(cfg.DepthResourceBarrier.value_or(readable)),
             static_cast<D3D12_RESOURCE_STATES>(
                 cfg.MVResourceBarrier.value_or(unreal ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : readable)),
             static_cast<D3D12_RESOURCE_STATES>(cfg.ExposureResourceBarrier.value_or(readable)) };
}
} // namespace

ShaderPass_Dx12 MakeDlssNrPass(DlssNr_Dx12& shader, ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                               NVSDK_NGX_Parameter* parameters, bool beforeUpscale, unsigned int featureFlags,
                               ID3D12CommandQueue* timingQueue, bool interop, bool rayReconstruction,
                               uint64_t submissionEpoch)
{
    auto* color = NrResource(parameters, NVSDK_NGX_Parameter_Color, "DLSSD.Color");
    auto* depth = NrResource(parameters, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    auto* motion = NrResource(parameters, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");
    auto* exposure = NrResource(parameters, NVSDK_NGX_Parameter_ExposureTexture, "DLSSD.ExposureTexture");
    const auto states = NrStates(interop);
    const bool supportedSubrects = HasSupportedNrSubrects(parameters, beforeUpscale);

    DlssNrFrameInfo frame {};
    frame.BeforeUpscale = beforeUpscale;
    frame.PrivateColorCopy = beforeUpscale;
    frame.IndependentCommands = interop;
    frame.RayReconstruction = rayReconstruction;
    frame.SubmissionEpoch = submissionEpoch;
    frame.OutputArrivalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    frame.DepthInverted = (featureFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.MotionVectorsLowResolution = (featureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    frame.ColourIsLinearHdr = (featureFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
    auto* finalOutput = NrResource(parameters, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    auto* colourAuthority = finalOutput != nullptr ? finalOutput : color;
    if (colourAuthority != nullptr)
    {
        switch (colourAuthority->GetDesc().Format)
        {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
            break;
        default:
            frame.ColourIsLinearHdr = false;
        }
    }
    if (finalOutput != nullptr)
    {
        frame.OutputWidth = static_cast<unsigned int>(finalOutput->GetDesc().Width);
        frame.OutputHeight = finalOutput->GetDesc().Height;
    }
    unsigned int outputWidth = 0, outputHeight = 0;
    parameters->Get(NVSDK_NGX_Parameter_OutWidth, &outputWidth);
    parameters->Get(NVSDK_NGX_Parameter_OutHeight, &outputHeight);
    if (outputWidth != 0 && outputHeight != 0)
    {
        frame.OutputWidth = outputWidth;
        frame.OutputHeight = outputHeight;
    }
    unsigned int reset = 0;
    parameters->Get(NVSDK_NGX_Parameter_Reset, &reset);
    frame.Reset = reset != 0;
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX);
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &frame.PreExposure);
    if (frame.PreExposure <= 1e-6f)
        frame.PreExposure = 1.0f;
    frame.ExposureTexture = exposure;
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &frame.DepthSubrectBaseX);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &frame.DepthSubrectBaseY);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &frame.MotionSubrectBaseX);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &frame.MotionSubrectBaseY);

    return {
        [=, &shader](ID3D12Resource* nextOutput) -> ID3D12Resource*
        {
            if (!supportedSubrects || !Config::Instance()->DlssNrEnabled.value_or_default() || !shader.IsInit() ||
                depth == nullptr || motion == nullptr || nextOutput == nullptr)
                return nullptr;
            if (beforeUpscale)
                return color;
            if (!shader.CreateBufferResource(device, nextOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                return nullptr;
            shader.SetBufferState(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return shader.Buffer();
        },
        [=, &shader](ID3D12Resource* input, ID3D12Resource* output) -> bool
        {
            // Every guide is returned to the upscaler's input state, including failed NR evaluations.
            struct RestoreInputs
            {
                ID3D12GraphicsCommandList* commandList;
                std::vector<std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>> resources;
                void Read(ID3D12Resource* resource, D3D12_RESOURCE_STATES state)
                {
                    if (resource == nullptr ||
                        std::any_of(resources.begin(), resources.end(),
                                    [resource](const auto& entry) { return entry.first == resource; }))
                        return;
                    resources.emplace_back(resource, state);
                    NrBarrier(commandList, resource, state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
                ~RestoreInputs()
                {
                    for (auto it = resources.rbegin(); it != resources.rend(); ++it)
                        NrBarrier(commandList, it->first, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, it->second);
                }
            } restore { commandList };

            if (beforeUpscale)
            {
                restore.Read(input, states.color);
                shader.SetBufferState(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            else
                shader.SetBufferState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            restore.Read(depth, states.depth);
            restore.Read(motion, states.motion);
            restore.Read(exposure, states.exposure);

            const bool result = shader.Dispatch(commandList, input, depth, motion, output, frame, timingQueue);
            if (beforeUpscale)
            {
                shader.SetBufferState(commandList, states.color);
                return result;
            }
            if (!result)
            {
                // A disabled/failed optional pass must still provide the next stage with the original frame.
                shader.SetBufferState(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
                NrBarrier(commandList, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                commandList->CopyResource(output, input);
                NrBarrier(commandList, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            return true;
        }
    };
}

ID3D12Resource* PrepareDlssNrInput(DlssNr_Dx12& shader, ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                                   NVSDK_NGX_Parameter* parameters, unsigned int featureFlags,
                                   ID3D12CommandQueue* timingQueue, bool interop, bool rayReconstruction,
                                   uint64_t submissionEpoch)
{
    auto* color = NrResource(parameters, NVSDK_NGX_Parameter_Color, "DLSSD.Color");
    if (color == nullptr || !shader.IsInit() || !CanRunNrBeforeUpscale(parameters) ||
        !Config::Instance()->DlssNrEnabled.value_or_default())
        return nullptr;
    const auto desc = color->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
        !shader.CreateBufferResource(device, color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        return nullptr;

    ShaderPipeline_Dx12 pipeline;
    pipeline.push_back(MakeDlssNrPass(shader, device, commandList, parameters, true, featureFlags, timingQueue, interop,
                                      rayReconstruction, submissionEpoch));
    SetupShaderPipeline(pipeline, shader.Buffer());
    if (pipeline.front().inputBuffer != nullptr && DispatchShaderPipeline(pipeline))
        return shader.Buffer();
    return nullptr;
}

void IFeature_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                                    D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState) const
{
    if (InBeforeState == InAfterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = InResource;
    barrier.Transition.StateBefore = InBeforeState;
    barrier.Transition.StateAfter = InAfterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    InCommandList->ResourceBarrier(1, &barrier);
}

bool IFeature_Dx12::Init(ID3D12Device* InDevice, ID3D12GraphicsCommandList* InCommandList,
                         NVSDK_NGX_Parameter* InParameters)
{
    Device = InDevice;

    auto result = InitInternal(InCommandList, InParameters);

    if (result)
    {
        if (!Config::Instance()->OverlayMenu.value_or_default() && (Imgui == nullptr || Imgui.get() == nullptr))
            Imgui = std::make_unique<Menu_Dx12>(Util::GetProcessWindow(), InDevice);

        OutputScaler = std::make_unique<OS_Dx12>("Output Scaling", InDevice, (TargetWidth() < DisplayWidth()));
        RCAS = std::make_unique<RCAS_Dx12>("RCAS", InDevice);
        Bias = std::make_unique<Bias_Dx12>("Bias", InDevice); // TODO: not needed on DLSS/DLSSD
        Magnifier = std::make_unique<Magnifier_Dx12>("Magnifier", InDevice);
        NeuralRendering = std::make_unique<DlssNr_Dx12>("Neural Rendering", InDevice);

        UpscalerTime = std::make_unique<GpuTime_Dx12>(InDevice);
    }

    return result;
}

bool IFeature_Dx12::Evaluate(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters,
                             ID3D12CommandQueue* timingQueue, uint64_t submissionEpoch, bool sourceRayReconstruction)
{
    const bool interop = timingQueue != nullptr;
    if (!interop)
        submissionEpoch = State::Instance().frameCount;
    if (timingQueue == nullptr)
        timingQueue = State::Instance().currentCommandQueue;
    if (!IsInited())
    {
        LOG_ERROR("Not inited!");
        return false;
    }

    if (Config::Instance()->OverrideSharpness.value_or_default())
        _sharpness = Config::Instance()->Sharpness.value_or_default();
    else
        _sharpness = GetSharpness(InParameters);

    if (_sharpness > 1.0f)
        _sharpness = 1.0f;

    // Those upcalers don't have their own sharpness so always need to use RCAS when sharpness is set
    auto upscaler = GetUpscalerType();
    bool useRcas = upscaler == Upscaler::XeSS ||
                   (upscaler == Upscaler::DLSS && Version() >= feature_version(2, 5, 1)) || upscaler == Upscaler::DLSSD;

    if (!useRcas)
        useRcas = Config::Instance()->RcasEnabled.value_or_default();

    if (_sharpness == 0.0f)
        useRcas = false;

    // Need RCAS for MAS
    if (!useRcas && (Config::Instance()->MotionSharpnessEnabled.value_or_default() &&
                     Config::Instance()->MotionSharpness.value_or_default() > 0.0f))
    {
        useRcas = true;
    }

    if (!RCAS->IsInit())
        useRcas = false;

    bool useOutputScaling =
        Config::Instance()->OutputScalingEnabled.value_or_default() && (LowResMV() || RenderWidth() == DisplayWidth());

    if (!OutputScaler->IsInit())
        useOutputScaling = false;

    auto* paramOutput = GetUpscalerResource_Dx12(InParameters, NVSDK_NGX_Parameter_Output);
    auto* paramMotion = GetUpscalerResource_Dx12(InParameters, NVSDK_NGX_Parameter_MotionVectors);
    auto* paramDepth = GetUpscalerResource_Dx12(InParameters, NVSDK_NGX_Parameter_Depth);

    RestoreUpscalerResources_Dx12 restoreResources(InParameters);

    const bool rayReconstruction = sourceRayReconstruction || upscaler == Upscaler::DLSSD;
    // Specialized schedules own the two seams but keep the same per-feature shader/history lifetime.
    const bool specializedNr = NeuralRendering->ProcessSeam(
        InCommandList, InParameters, true, timingQueue, rayReconstruction, submissionEpoch, interop, GetFeatureFlags());
    const bool nrBeforeUpscale = !specializedNr && Config::Instance()->DlssNrRunBeforeSr.value_or_default() &&
                                 CanRunNrBeforeUpscale(InParameters);

    // Order is important as that's the order of shader dispatch
    ShaderPipeline_Dx12 pipeline;

    if (useOutputScaling)
    {
        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  if (OutputScaler->CreateBufferResource(Device, nextOutput, TargetWidth(), TargetHeight(),
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                  {
                      OutputScaler->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                      return OutputScaler->Buffer();
                  }
                  return nullptr;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  LOG_DEBUG("Scaling output...");
                  OutputScaler->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                  if (!OutputScaler->Dispatch(InCommandList, input, output))
                  {
                      Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
                      State::Instance().changeBackend[Handle()->Id] = true;
                      return false;
                  }
                  return true;
              } });
    }

    _actualSharpness = _sharpness;
    if (useRcas)
    {
        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  // Disable any built-in sharpness shaders
                  InParameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
                  _sharpness = 0.0f;

                  if (RCAS->CreateBufferResource(Device, nextOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                  {
                      RCAS->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                      return RCAS->Buffer();
                  }
                  return nullptr;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  if (!RCAS->CanRender() || !paramMotion || !paramOutput)
                      return true;

                  RCAS->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                  RcasConstants rcasConstants {};

                  rcasConstants.Sharpness = _actualSharpness.value_or(_sharpness);
                  rcasConstants.DepthIsLinear = DepthLinear();
                  rcasConstants.DepthIsReversed = DepthInverted();
                  rcasConstants.IsHdr = IsHdr();

                  // Restore value
                  _sharpness = _actualSharpness.value_or(_sharpness);
                  _actualSharpness.reset();

                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &rcasConstants.MvScaleX);
                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &rcasConstants.MvScaleY);

                  float nearPlane = 0.0f;
                  float farPlane = 0.0f;

                  // We need camera near and far for DLSSD
                  // We passthrough those values from the DLSSG params onto the upscaler's params
                  if (InParameters->Get("DLSSG.CameraNear", &nearPlane) == NVSDK_NGX_Result_Success &&
                      InParameters->Get("DLSSG.CameraFar", &farPlane) == NVSDK_NGX_Result_Success)
                  {
                      rcasConstants.CameraNear = nearPlane;
                      rcasConstants.CameraFar = farPlane;
                  }
                  else
                  {
                      rcasConstants.CameraNear = Config::Instance()->FsrCameraNear.value_or_default();
                      rcasConstants.CameraFar = Config::Instance()->FsrCameraFar.value_or_default();
                  }

                  if (!RCAS->Dispatch(InCommandList, input, paramMotion, rcasConstants, output, paramDepth))
                  {
                      Config::Instance()->RcasEnabled.set_volatile_value(false);
                      return false;
                  }
                  return true;
              } });
    }

    if (!specializedNr && !nrBeforeUpscale && Config::Instance()->DlssNrEnabled.value_or_default())
    {
        pipeline.push_back(MakeDlssNrPass(*NeuralRendering, Device, InCommandList, InParameters, false,
                                          GetFeatureFlags(), timingQueue, interop, rayReconstruction, submissionEpoch));
    }

    if (Magnifier->ShouldRun())
    {
        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  if (Magnifier->CreateBufferResource(Device, nextOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                  {
                      Magnifier->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                      return Magnifier->Buffer();
                  }

                  return nullptr;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  if (!Magnifier->CanRender() || !paramMotion || !paramOutput)
                      return true;

                  Magnifier->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                  return Magnifier->Dispatch(InCommandList, input, output);
              } });
    }

    // Post-seam scheduling sees the same final output identity as the pre-seam, after all ordinary passes.
    pipeline.push_back({ [](ID3D12Resource* output) { return output; },
                         [&](ID3D12Resource*, ID3D12Resource* output)
                         {
                             auto* previousOutput = GetUpscalerResource_Dx12(InParameters, NVSDK_NGX_Parameter_Output);
                             SetUpscalerResource_Dx12(InParameters, NVSDK_NGX_Parameter_Output, output);
                             NeuralRendering->ProcessSeam(InCommandList, InParameters, false, timingQueue,
                                                          rayReconstruction, submissionEpoch, interop,
                                                          GetFeatureFlags());
                             SetUpscalerResource_Dx12(InParameters, NVSDK_NGX_Parameter_Output, previousOutput);
                             return true;
                         } });

    // Upscaler will write to the first active shader, or just output
    auto* currentTarget = SetupShaderPipeline(pipeline, paramOutput);
    SetUpscalerResource_Dx12(InParameters, NVSDK_NGX_Parameter_Output, currentTarget);
    auto* originalColor = GetUpscalerResource_Dx12(InParameters, NVSDK_NGX_Parameter_Color);
    if (nrBeforeUpscale)
    {
        if (auto* nrInput = PrepareDlssNrInput(*NeuralRendering, Device, InCommandList, InParameters, GetFeatureFlags(),
                                               timingQueue, interop, rayReconstruction, submissionEpoch))
            SetUpscalerResource_Dx12(InParameters, NVSDK_NGX_Parameter_Color, nrInput);
    }
    UpscalerTime->Start(InCommandList);
    const bool evalResult = EvaluateInternal(InCommandList, InParameters);
    UpscalerTime->End(InCommandList);
    SetUpscalerResource_Dx12(InParameters, NVSDK_NGX_Parameter_Color, originalColor);

    if (!evalResult)
        return false;

    if (!DispatchShaderPipeline(pipeline))
        return true;

    // imgui
    if (!Config::Instance()->OverlayMenu.value_or_default() && _frameCount > 30)
    {
        if (Imgui != nullptr && Imgui.get() != nullptr)
        {
            if (Imgui->IsHandleDifferent())
            {
                Imgui.reset();
            }
            else
                Imgui->Render(InCommandList, paramOutput);
        }
        else
        {
            if (Imgui == nullptr || Imgui.get() == nullptr)
                Imgui = std::make_unique<Menu_Dx12>(GetForegroundWindow(), Device);
        }
    }

    return evalResult;
}

std::optional<double> IFeature_Dx12::ReadUpscalerTime(void* commandQueueVoid)
{
    ID3D12CommandQueue* commandQueue = (ID3D12CommandQueue*) commandQueueVoid;

    lastUpscalerTime = UpscalerTime->ReadGpuTime(commandQueue);
    lastRcasTime = RCAS->ReadGpuTime(commandQueue);
    lastOutputScalingTime = OutputScaler->ReadGpuTime(commandQueue);

    return sumOpts(lastUpscalerTime, lastRcasTime, lastOutputScalingTime);
}

void IFeature_Dx12::ReadDetailedGpuTimes(void* commandQueueVoid, std::vector<DetailedGpuTime>& detailedGpuTimes)
{
    ID3D12CommandQueue* commandQueue = (ID3D12CommandQueue*) commandQueueVoid;

    detailedGpuTimes.clear();

    // Do not call ReadGpuTime twice for shaders
    if (lastUpscalerTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { ShortName(), lastUpscalerTime.value(), true });

    if (lastRcasTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { RCAS->Name(), lastRcasTime.value(), true });

    if (lastOutputScalingTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { OutputScaler->Name(), lastOutputScalingTime.value(), true });

    auto magnifierTime = Magnifier->ReadGpuTime(commandQueue);

    if (magnifierTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { Magnifier->Name(), magnifierTime.value(), false });
}

IFeature_Dx12::IFeature_Dx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters) {}

IFeature_Dx12::~IFeature_Dx12()
{
    DlssNr::ExposureScan::ReleaseTrackedResources();
    if (State::Instance().isShuttingDown)
        return;

    Imgui.reset();
    OutputScaler.reset();
    RCAS.reset();
    Bias.reset();
    NeuralRendering.reset();
}
