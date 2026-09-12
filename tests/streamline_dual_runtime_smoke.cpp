// Runs both Streamline runtimes through the real OptiScaler loader hooks, without launching a game.
// Build: cl /std:c++20 /EHsc /I external/streamline tests/streamline_dual_runtime_smoke.cpp
//        /link d3d12.lib user32.lib
// Run in an isolated directory with OptiScaler.ini (FGInput=DLSSG, FGOutput=DLSSG, OptiDllPath set):
//   streamline_dual_runtime_smoke.exe <OptiScaler DLL> <game Streamline dir> <private Streamline dir>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <sl.h>
#include <sl_reflex.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

static void Require(bool success, const char* stage)
{
    if (!success)
        throw std::runtime_error(stage);
}

static void Check(HRESULT result, const char* stage)
{
    std::printf("%s: 0x%08x\n", stage, (unsigned) result);
    std::fflush(stdout);
    Require(SUCCEEDED(result), stage);
}

template <class T> static T Export(HMODULE module, const char* name)
{
    auto function = GetProcAddress(module, name);
    Require(function != nullptr, name);
    return reinterpret_cast<T>(function);
}

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try
    {
        Require(argc == 4, "Expected OptiScaler DLL, game Streamline dir, private Streamline dir");
        Require(LoadLibraryW(argv[1]) != nullptr, "Load OptiScaler");
        ID3D12Device* device = nullptr;
        Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "Create device");
        const auto privatePath = std::filesystem::path(argv[3]) / L"sl.interposer.dll";
        auto privateSl = GetModuleHandleW(privatePath.c_str());
        Require(privateSl != nullptr, "OptiScaler must have loaded its private Streamline runtime");
        const auto gamePath = std::filesystem::path(argv[2]) / L"sl.interposer.dll";
        auto gameSl = LoadLibraryW(gamePath.c_str());
        Require(gameSl != nullptr && gameSl != privateSl, "Expected distinct Streamline runtimes");

        const sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
        const wchar_t* directory = argv[2];
        sl::Preferences preferences;
        preferences.applicationId = 4919;
        preferences.engine = sl::EngineType::eUnreal;
        preferences.featuresToLoad = features;
        preferences.numFeaturesToLoad = 3;
        preferences.pathsToPlugins = &directory;
        preferences.numPathsToPlugins = 1;
        preferences.renderAPI = sl::RenderAPI::eD3D12;
        preferences.flags |= sl::PreferenceFlags::eUseDXGIFactoryProxy;
        auto initialized = Export<decltype(&slInit)>(gameSl, "slInit")(preferences, sl::kSDKVersion);
        std::printf("Game slInit: %u\n", (unsigned) initialized);
        std::fflush(stdout);
        Require(initialized == sl::Result::eOk, "Game Streamline initialization");
        Require(Export<decltype(&slSetD3DDevice)>(gameSl, "slSetD3DDevice")(device) == sl::Result::eOk,
                "Game Streamline device initialization");

        void* reflex = nullptr;
        Require(Export<decltype(&slGetFeatureFunction)>(privateSl, "slGetFeatureFunction")(
                    sl::kFeatureReflex, "slReflexSetOptions", reflex) == sl::Result::eOk &&
                    reflex,
                "Private Reflex still resolves after game initialization");
        sl::ReflexOptions options;
        Require(reinterpret_cast<decltype(&slReflexSetOptions)>(reflex)(options) == sl::Result::eOk,
                "Private Reflex still works after game initialization");

        using CreateFactory = HRESULT(WINAPI*)(UINT, REFIID, void**);
        IDXGIFactory2* factory = nullptr;
        Check(Export<CreateFactory>(gameSl, "CreateDXGIFactory2")(0, IID_PPV_ARGS(&factory)), "Create factory");
        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        ID3D12CommandQueue* queue = nullptr;
        Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Create queue");
        HWND window = CreateWindowExW(0, L"STATIC", L"Streamline resize regression", WS_POPUP, 0, 0, 3840, 2160,
                                      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(window != nullptr, "Create hidden test window");
        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Width = 3840;
        desc.Height = 2160;
        desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferCount = 3;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        IDXGISwapChain1* swapchain = nullptr;
        Check(factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &swapchain),
              "Create HDR swapchain");
        Check(swapchain->ResizeBuffers(3, 3840, 2160, desc.Format, desc.Flags), "Resize with game flags 0x802");
        Check(swapchain->ResizeBuffers(3, 1920, 1080, desc.Format, desc.Flags), "Resize to new dimensions");
        std::puts("PASS: dual Streamline initialization, private Reflex, HDR swapchain creation and resize");
        std::fflush(stdout);
        // Let the injected DLL own process teardown; this smoke does not test game shutdown ordering.
        ExitProcess(0);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        std::fflush(stderr);
        ExitProcess(1);
    }
}
