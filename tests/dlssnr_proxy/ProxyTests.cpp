// Compile the production backend with mock NGX entry points and the real SDK parameter interface.
#include "../../OptiScaler/dlssnr/DlssNr_Proxy.cpp"
#include "../../OptiScaler/dlssnr/DlssNr_Status.cpp"
#include "../../OptiScaler/upscalers/ShaderPipeline_Dx12.h"

int main()
{
    DlssNr::Proxy::Context proxy;
    ID3D12Device device;
    ID3D12GraphicsCommandList commands;
    ID3D12Resource color, depth, motion, output;
    bool evaluated = true;
    uint64_t epoch = 0;
    DlssNr::Proxy::Settings settings { 0, 0, 0.5f, 1.0f, 0.0f, -1.0f, true };
    auto run = [&](bool advance = true)
    {
        if (advance)
            ++epoch;
        return proxy.Run(&commands, &device, &color, &depth, &motion, &output, 1920, 1080, 1280, 720, 1920, 1080, 12,
                         24, 32, 48, true, false, 0.5f, -0.25f, settings, epoch, &evaluated);
    };
    auto value = []<typename T>(const char* key)
    {
        T result {};
        assert(Mock::latest->Get(key, &result) == NVSDK_NGX_Result_Success);
        return result;
    };

    // Creation has its own frame, and setters must preserve SDK types (not raw vtable offsets).
    assert(run() == NVSDK_NGX_Result_Success && !evaluated);
    assert(Mock::creations == 1 && Mock::evaluations == 0);
    assert(run(false) == NVSDK_NGX_Result_Success && !evaluated);
    assert(Mock::creations == 1 && Mock::evaluations == 0);
    assert(value.operator()<unsigned int>("DLSSNR.Width") == 1920);
    assert(value.operator()<unsigned int>("DLSSNR.Hint.Render.Preset") == 0);
    assert(value.operator()<float>("DLSSNR.Intensity") == 0.5f);
    assert(run() == NVSDK_NGX_Result_Success && evaluated);
    assert(value.operator()<ID3D12Resource*>("DLSSNR.Color") == &color);
    assert(value.operator()<ID3D12Resource*>("DLSSNR.Output") == &output);
    // High-resolution motion and guide offsets must survive the typed NGX dispatch independently.
    assert(value.operator()<unsigned int>("DLSSNR.DepthSubrectWidth") == 1280);
    assert(value.operator()<unsigned int>("DLSSNR.DepthSubrectHeight") == 720);
    assert(value.operator()<unsigned int>("DLSSNR.DepthSubrectBaseX") == 12);
    assert(value.operator()<unsigned int>("DLSSNR.DepthSubrectBaseY") == 24);
    assert(value.operator()<unsigned int>("DLSSNR.MVecSubrectWidth") == 1920);
    assert(value.operator()<unsigned int>("DLSSNR.MVecSubrectHeight") == 1080);
    assert(value.operator()<unsigned int>("DLSSNR.MVecSubrectBaseX") == 32);
    assert(value.operator()<unsigned int>("DLSSNR.MVecSubrectBaseY") == 48);
    assert(value.operator()<float>("DLSSNR.MVecScaleY") == -0.25f);
    assert(value.operator()<unsigned int>("DLSSNR.Reset") == 1);
    assert(run() == NVSDK_NGX_Result_Success && evaluated);
    assert(value.operator()<unsigned int>("DLSSNR.Reset") == 0);

    // Creation-time tuning edits rebuild; the previous GPU feature/map survive the retirement window.
    settings.preset = 2;
    assert(run() == NVSDK_NGX_Result_Success && !evaluated);
    assert(Mock::creations == 2 && Mock::releases == 0 && Mock::destructions == 0);
    assert(value.operator()<unsigned int>("DLSSNR.Hint.Render.Preset") == 2);
    for (int call = 0; call < 40; ++call)
        assert(run(false) == NVSDK_NGX_Result_Success && !evaluated);
    assert(Mock::releases == 0 && Mock::destructions == 0);
    for (int frame = 0; frame < 31; ++frame)
        assert(run() == NVSDK_NGX_Result_Success && evaluated);
    assert(Mock::releases == 0 && Mock::destructions == 0);
    assert(run() == NVSDK_NGX_Result_Success && evaluated);
    assert(Mock::releases == 1 && Mock::destructions == 1);

    // A real NGX failure reaches the caller and stays latched until an explicit retry.
    Mock::evaluateResult = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    assert(run() == (unsigned int) Mock::evaluateResult && !evaluated);
    auto evaluationsBefore = Mock::evaluations;
    assert(run() == 0 && !evaluated && Mock::evaluations == evaluationsBefore);
    proxy.RetryAfterFailure();
    Mock::evaluateResult = NVSDK_NGX_Result_Success;
    Mock::createResult = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    assert(run() == (unsigned int) Mock::createResult && !evaluated);
    auto creationsBefore = Mock::creations;
    assert(run() == 0 && Mock::creations == creationsBefore);
    proxy.RetryAfterFailure();
    Mock::createResult = NVSDK_NGX_Result_Success;
    assert(run() == NVSDK_NGX_Result_Success && !evaluated);
    assert(run() == NVSDK_NGX_Result_Success && evaluated);

    proxy.Release();
    assert(Mock::handles.empty());
    assert(Mock::allocations == Mock::destructions);
    proxy.Release(); // Idempotent shutdown.
    assert(Mock::allocations == Mock::destructions);

    // Alternating upscalers own independent features, parameter maps, failure latches and teardown.
    {
        DlssNr::Proxy::Context other;
        assert(run() == NVSDK_NGX_Result_Success && !evaluated);
        auto* firstParams = Mock::latest;
        DlssNr::Proxy::Settings otherSettings { 3, 2, 0.75f, 0.4f, 0.3f, 0.2f, false };
        auto runOther = [&]
        {
            return other.Run(&commands, &device, &color, &depth, &motion, &output, 1280, 720, 1280, 720, 1280, 720, 0,
                             0, 0, 0, false, false, 1.0f, 1.0f, otherSettings, ++epoch, &evaluated);
        };
        assert(runOther() == NVSDK_NGX_Result_Success && !evaluated);
        auto* secondParams = Mock::latest;
        assert(value.operator()<unsigned int>("DLSSNR.Hint.Render.Preset") == 3);
        assert(value.operator()<unsigned int>("DLSSNR.Style") == 2);
        assert(value.operator()<float>("DLSSNR.LocalToneStrength") == 0.3f);
        assert(value.operator()<unsigned int>("DLSSNR.UseAutoMask") == 0);
        assert(firstParams != secondParams && Mock::handles.size() == 2);
        auto creations = Mock::creations;
        assert(run() == NVSDK_NGX_Result_Success && evaluated);
        assert(runOther() == NVSDK_NGX_Result_Success && evaluated);
        assert(Mock::creations == creations);
        Mock::evaluateResult = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
        assert(run() == (unsigned int) Mock::evaluateResult && !evaluated);
        Mock::evaluateResult = NVSDK_NGX_Result_Success;
        assert(runOther() == NVSDK_NGX_Result_Success && evaluated);
        proxy.Release();
        assert(Mock::handles.size() == 1);
        assert(runOther() == NVSDK_NGX_Result_Success && evaluated);
    }
    assert(Mock::handles.empty() && Mock::allocations == Mock::destructions);

    // Clearing an older owner must not erase the current shader's menu snapshot.
    DlssNr::StatusSnapshot status;
    status.running = true;
    status.frames = 5;
    DlssNr::PublishStatus(&color, DlssNr::Backend::Dx12, status);
    DlssNr::PublishStatus(&output, DlssNr::Backend::Dx12, status);
    DlssNr::ClearStatus(&color);
    assert(DlssNr::IsRunning());
    DlssNr::ClearStatus(&output);
    assert(!DlssNr::IsRunning());
    const auto requestsBefore = DlssNr::ReadControlRequests();
    DlssNr::RetryAfterFailure();
    DlssNr::RequestCapture(8);
    const auto requestsAfter = DlssNr::ReadControlRequests();
    assert(requestsAfter.retryGeneration == requestsBefore.retryGeneration + 1);
    assert(requestsAfter.captureGeneration == requestsBefore.captureGeneration + 1);
    assert(requestsAfter.captureFrames == 8);

    // The shared runner routes resources backwards, then executes stages forwards.
    ID3D12Resource intermediate;
    std::string order;
    ShaderPipeline_Dx12 pipeline { { [&](ID3D12Resource* target)
                                     {
                                         assert(target == &intermediate);
                                         order += 'a';
                                         return &color;
                                     },
                                     [&](ID3D12Resource* input, ID3D12Resource* target)
                                     {
                                         assert(input == &color && target == &intermediate);
                                         order += 'A';
                                         return true;
                                     } },
                                   { [&](ID3D12Resource* target)
                                     {
                                         assert(target == &output);
                                         order += 'b';
                                         return &intermediate;
                                     },
                                     [&](ID3D12Resource* input, ID3D12Resource* target)
                                     {
                                         assert(input == &intermediate && target == &output);
                                         order += 'B';
                                         return false;
                                     } } };
    assert(SetupShaderPipeline(pipeline, &output) == &color);
    assert(!DispatchShaderPipeline(pipeline));
    assert(order == "baAB");

    // Bridge parameter maps may store void*; restoration preserves both pointer and SDK type.
    Mock::Params parameters;
    parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<void*>(&color));
    parameters.Set(NVSDK_NGX_Parameter_Output, &output);
    {
        RestoreUpscalerResources_Dx12 restore(&parameters);
        parameters.Set(NVSDK_NGX_Parameter_Color, &intermediate);
        parameters.Set(NVSDK_NGX_Parameter_Output, &intermediate);
        assert(!DispatchShaderPipeline(pipeline));
    }
    void* restoredColor = nullptr;
    ID3D12Resource* restoredOutput = nullptr;
    assert(parameters.Get(NVSDK_NGX_Parameter_Color, &restoredColor) == NVSDK_NGX_Result_Success);
    assert(parameters.Get(NVSDK_NGX_Parameter_Output, &restoredOutput) == NVSDK_NGX_Result_Success);
    assert(restoredColor == &color && restoredOutput == &output);
}
