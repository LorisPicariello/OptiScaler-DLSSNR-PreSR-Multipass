// Compile the production backend with mock NGX entry points and the real SDK parameter interface.
#include "../../OptiScaler/dlssnr/DlssNr_Proxy.cpp"

int main()
{
    ID3D12Device device;
    ID3D12GraphicsCommandList commands;
    ID3D12Resource color, depth, motion, output;
    bool evaluated = true;
    auto run = [&] {
        return DlssNr::Proxy::Run(&commands, &device, &color, &depth, &motion, &output,
                                  1920, 1080, 1280, 720, true, false, 0.5f, -0.25f, &evaluated);
    };
    auto value = []<typename T>(const char* key) {
        T result {};
        assert(Mock::latest->Get(key, &result) == NVSDK_NGX_Result_Success);
        return result;
    };

    // Creation has its own frame, and setters must preserve SDK types (not raw vtable offsets).
    assert(run() == NVSDK_NGX_Result_Success && !evaluated);
    assert(Mock::creations == 1 && Mock::evaluations == 0);
    assert(value.operator()<unsigned int>("DLSSNR.Width") == 1920);
    assert(value.operator()<unsigned int>("DLSSNR.Hint.Render.Preset") == 0);
    assert(value.operator()<float>("DLSSNR.Intensity") == 0.5f);
    assert(run() == NVSDK_NGX_Result_Success && evaluated);
    assert(value.operator()<ID3D12Resource*>("DLSSNR.Color") == &color);
    assert(value.operator()<ID3D12Resource*>("DLSSNR.Output") == &output);
    assert(value.operator()<unsigned int>("DLSSNR.DepthSubrectWidth") == 1280);
    assert(value.operator()<float>("DLSSNR.MVecScaleY") == -0.25f);
    assert(value.operator()<unsigned int>("DLSSNR.Reset") == 1);
    assert(run() == NVSDK_NGX_Result_Success && evaluated);
    assert(value.operator()<unsigned int>("DLSSNR.Reset") == 0);

    // Creation-time tuning edits rebuild; the previous GPU feature/map survive the retirement window.
    Config::Instance()->DlssNrPreset.value = 2;
    assert(run() == NVSDK_NGX_Result_Success && !evaluated);
    assert(Mock::creations == 2 && Mock::releases == 0 && Mock::destructions == 0);
    assert(value.operator()<unsigned int>("DLSSNR.Hint.Render.Preset") == 2);
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
    DlssNr::Proxy::RetryAfterFailure();
    Mock::evaluateResult = NVSDK_NGX_Result_Success;
    Mock::createResult = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    assert(run() == (unsigned int) Mock::createResult && !evaluated);
    auto creationsBefore = Mock::creations;
    assert(run() == 0 && Mock::creations == creationsBefore);
    DlssNr::Proxy::RetryAfterFailure();
    Mock::createResult = NVSDK_NGX_Result_Success;
    assert(run() == NVSDK_NGX_Result_Success && !evaluated);
    assert(run() == NVSDK_NGX_Result_Success && evaluated);

    DlssNr::Proxy::Release();
    assert(Mock::handles.empty());
    assert(Mock::allocations == Mock::destructions);
    DlssNr::Proxy::Release(); // Idempotent shutdown.
    assert(Mock::allocations == Mock::destructions);
}
