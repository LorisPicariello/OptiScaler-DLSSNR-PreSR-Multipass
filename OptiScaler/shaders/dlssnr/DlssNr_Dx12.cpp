#include "pch.h"
#include <dlssnr/PassProfiles.h>

#include <set>
#include <wrl/client.h>
#include <resource_tracking/ResTrack_Dx12.h>

#include <dlssnr/DlssNr.h>
#include <dlssnr/ResidualFg.h>
#include <DirectXMath.h>

#include <dlssnr/DlssNr_Capture.h>
#include <dlssnr/DlssNr_Proxy.h>
#include <dlssnr/DlssNr_ExposureScan.h>

#include "DlssNr_Dx12.h"
#include "DlssNr_ActiveColor.h"
#include "DlssNr_Guides.h"
#include "DlssNr_SeamClock.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <proxies/NVNGX_Proxy.h>
#include <hooks/D3D12_Hooks.h>
#include <gpu_time/GpuTime_Dx12.h>
#include "DlssNr_GpuTime.h"

#include <mutex>
#include <algorithm>
#include <cstring>
#include "precompile/DlssNr_Shader.h"
#include "precompile/dlssnr_residual_Shader.h"
#include "precompile/dlssnr_finished_color_Shader.h"
#include "DlssNr_ResidualPair.h"
#include "../output_scaling/OS_Dx12.h"

using DlssNr::Profiles::NrPassTuning;
using DlssNr::Profiles::PassPreset;
using DlssNr::Profiles::PassStyle;
using DlssNr::Profiles::PassTuning;

using DlssNr::CalibrationReading;
using DlssNr::ResidualFg;
using DlssNr::ResidualFgApi;
using DlssNr::ResidualFgCamera;

namespace
{
std::recursive_mutex nrOwnersMutex;
std::vector<DlssNr_Dx12*> nrOwners;
DlssNr_Dx12* activeNrOwner = nullptr;
void ActivateNrOwner(DlssNr_Dx12* owner)
{
    if (std::find(nrOwners.begin(), nrOwners.end(), owner) == nrOwners.end())
        nrOwners.push_back(owner);
    activeNrOwner = owner;
}
} // namespace

struct DlssNr_Dx12::State
{

    // NGX result codes, by name.
    //
    // A user's log recently read "init 0x-452FFFFF", which is an int formatted as hex and is
    // undiagnosable by anyone. It was 0xBAD00001, FeatureNotSupported -- a complete answer, printed as
    // noise. Names cost nothing and turn a bug report into a diagnosis.
    const char* NgxResultName(unsigned int r)
    {
        switch (r)
        {
        case 0x1:
            return "Success";
        case 0xBAD00001:
            return "FAIL_FeatureNotSupported";
        case 0xBAD00002:
            return "FAIL_PlatformError";
        case 0xBAD00003:
            return "FAIL_FeatureAlreadyExists";
        case 0xBAD00004:
            return "FAIL_FeatureNotFound";
        case 0xBAD00005:
            return "FAIL_InvalidParameter";
        case 0xBAD00006:
            return "FAIL_ScratchBufferTooSmall";
        case 0xBAD00007:
            return "FAIL_NotInitialized";
        case 0xBAD00008:
            return "FAIL_UnsupportedInputFormat";
        case 0xBAD00009:
            return "FAIL_RWFlagMissing";
        case 0xBAD0000A:
            return "FAIL_MissingInput";
        case 0xBAD0000B:
            return "FAIL_UnableToInitializeFeature";
        case 0xBAD0000C:
            return "FAIL_OutOfDate";
        case 0xBAD0000D:
            return "FAIL_OutOfGPUMemory";
        case 0xBAD0000E:
            return "FAIL_UnsupportedFormat";
        case 0xBAD0000F:
            return "FAIL_UnableToWriteToAppDataPath";
        case 0xBAD00010:
            return "FAIL_UnsupportedParameter";
        case 0xBAD00011:
            return "FAIL_Denied";
        case 0xBAD00012:
            return "FAIL_NotImplemented";
        default:
            return "unknown";
        }
    }

    // Does the driver's own nvngx.dll dispatch Neural Rendering?
    //
    // The trick is that correct parameters are not needed to find out, because the KIND of failure is
    // the answer. A dispatcher that has never heard of feature 18 rejects it before looking at anything:
    //
    //   FeatureNotFound / FeatureNotSupported / NotImplemented -- the driver does not route it, and the
    //       forwarder is necessary rather than merely tolerated.
    //   MissingInput / InvalidParameter / UnsupportedParameter -- the driver DOES route it. It reached
    //       the feature, which then complained about the arguments. That is the win: it means the whole
    //       forwarder, and the per-game copy of the model, can go.
    //   Success -- better still, though not expected from an empty parameter block.
    //
    // Once per session, and only when asked for.
    void ProbeProxyDispatch(ID3D12GraphicsCommandList* cmdList)
    {

        if (done)
            return;

        done = true;

        if (!NVNGXProxy::IsDx12Inited())
        {
            LOG_INFO("DLSS-NR proxy probe: the driver's nvngx is not initialised here, nothing to ask");
            return;
        }

        const auto allocate = NVNGXProxy::D3D12_AllocateParameters();
        const auto destroy = NVNGXProxy::D3D12_DestroyParameters();
        const auto create = NVNGXProxy::D3D12_CreateFeature();
        const auto release = NVNGXProxy::D3D12_ReleaseFeature();

        if (allocate == nullptr || create == nullptr)
        {
            LOG_INFO("DLSS-NR proxy probe: the driver's nvngx does not export what the probe needs");
            return;
        }

        NVSDK_NGX_Parameter* params = nullptr;

        if (allocate(&params) != NVSDK_NGX_Result_Success || params == nullptr)
        {
            LOG_INFO("DLSS-NR proxy probe: could not allocate a parameter block");
            return;
        }

        // Feature 18, and a feature that certainly does not exist, asked the same way.
        //
        // A single result cannot answer this. "UnableToInitializeFeature" for 18 looks like the
        // dispatcher having found the feature and failed to start it on an empty parameter block -- but
        // it might equally be what this dispatcher says about anything it cannot set up. The control
        // settles it: if a nonsense id comes back differently, the difference is knowledge of feature
        // 18. If both come back the same, the first result meant nothing.
        NVSDK_NGX_Handle* handle = nullptr;
        const auto result = (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 18, params, &handle);

        if (handle != nullptr && release != nullptr)
            release(handle);

        NVSDK_NGX_Handle* controlHandle = nullptr;
        const auto control = (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 200, params, &controlHandle);

        if (controlHandle != nullptr && release != nullptr)
            release(controlHandle);

        LOG_INFO("DLSS-NR proxy probe: feature 18 -> 0x{:X} ({}), control feature 200 -> 0x{:X} ({})", result,
                 NgxResultName(result), control, NgxResultName(control));

        const bool rejectedOutright = result == 0xBAD00004 || result == 0xBAD00001 || result == 0xBAD00012;

        if (result == control)
            LOG_INFO("DLSS-NR proxy probe: both answers identical, so this says nothing about feature 18 "
                     "-- the driver treats it exactly as it treats a feature that does not exist");
        else if (rejectedOutright)
            LOG_INFO("DLSS-NR proxy probe: feature 18 is rejected outright -- the driver does not route "
                     "it and the forwarder is required");
        else
            LOG_INFO("DLSS-NR proxy probe: feature 18 answers differently from a nonexistent one, so the "
                     "driver knows it -- the forwarder and the per-game model copy could both go");

        if (destroy != nullptr)
            destroy(params);
    }

    // Everything the model is reached through. The snippet refuses callers whose module path does not
    // contain "nvngx.dll", so the calls are made from a small library named for exactly that reason and
    // shipped beside OptiScaler; see nvngx.dll_dlssnr.dll.
    using PFN_NrCreate = void*(__cdecl*) (const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*,
                                          void*, unsigned int, unsigned int, int, float, int, float, float, float, int,
                                          int);
    using PFN_NrEvaluate = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*,
                                         ID3D12Resource*, ID3D12Resource*, unsigned int, unsigned int, unsigned int,
                                         unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,
                                         unsigned int, unsigned int, int, int, float, int, float, float, float, int,
                                         float, float);
    using PFN_NrRelease = void(__cdecl*)(void*);
    using PFN_NrSetExtras = void(__cdecl*)(void*, float, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                                           unsigned int, unsigned int, unsigned int, unsigned int);
    using PFN_NrSetFloatSlot = void(__cdecl*)(int);
    using PFN_NrProbeFloat = void(__cdecl*)(void*, const char*, float, int);

    // One per back buffer, so an allocator is never reset while its frame is still in flight.

    struct NrState
    {
        unsigned long long successfulDispatches = 0;
        HMODULE forwarder = nullptr;
        PFN_NrCreate create = nullptr;
        PFN_NrEvaluate evaluate = nullptr;
        PFN_NrRelease release = nullptr;
        PFN_NrSetExtras setExtras = nullptr;
        PFN_NrSetFloatSlot setFloatSlot = nullptr;
        PFN_NrProbeFloat probeFloat = nullptr;
        bool floatSlotKnown = false;

        // The scaling-ratio probe, resolved alongside the other forwarder entry points.
        int (*queryRatio)(const wchar_t*, void*, unsigned int, float*) = nullptr;
        const int* lastRatioStage = nullptr;
        int* lastInit = nullptr;
        int* lastCreate = nullptr;
        const char* (*lastModelError)() = nullptr;
        std::string modelError;

        NVSDK_NGX_Parameter* capabilityParams = nullptr;
        void* feature = nullptr;
        bool featurePendingSubmission = false;
        unsigned long long featureCreateEpoch = 0;

        // A feature per extra pass, each with its own temporal history.
        //
        // One feature run three times in a frame is told three frames passed with nothing moving between
        // them, so its history fights every pass after the first -- which is what "loses detail on later
        // passes" was. Separate features each see one frame per frame, which is the contract they were
        // built for.
        //
        // It is also the only reading that fits the one clue we have about how this is done elsewhere:
        // that implementation's memory grows with the pass count, and reusing a single feature cannot do
        // that. A feature apiece can, because each carries its own history.
        //
        // Indexed by pass, so [0] is unused and the first extra pass is [1]. Wasting one pointer keeps
        // every index here equal to the pass number it belongs to. Extra features are created on a
        // build-only invocation and first evaluated on a later command list.
        void* passFeature[DlssNr::MaxPassCount] = {};
        bool passNeedsReset[DlssNr::MaxPassCount] = {};
        bool passCreateFailed[DlssNr::MaxPassCount] = {};
        bool passPendingSubmission[DlssNr::MaxPassCount] = {};
        unsigned long long passCreateEpoch[DlssNr::MaxPassCount] = {};

        // The model cannot read and write one resource, so the frame is staged through these.
        ID3D12Resource* colorCopy = nullptr;
        ID3D12Resource* output = nullptr;

        // The second half of the model-output ping-pong. The base proxy stays immutable: pass 0 writes
        // output (A), pass 1 writes this (B), and pass 2 writes A again. Only the final answer is composed.
        ID3D12Resource* passScratch = nullptr;
        bool passScratchFailed = false;

        // The frame as the upscaler wrote it. The resolve adds the model's edit to this rather than
        // reconstructing it by inverting the tone curve, which is what turned every light in the frame into
        // a string of coloured cells.
        ID3D12Resource* hdrCopy = nullptr;

        // Compact origin-zero pre-SR image, only needed when Color has allocation padding. All codec,
        // hold and capture paths then see the real raster. UAV at rest, retired with the scratch set.
        ID3D12Resource* activeColor = nullptr;

        // The frame shrunk for the model, when it is working below full resolution.
        ID3D12Resource* colorSmall = nullptr;

        // Supersampling (working scale > 1): the Output Scaling upsampler used to enlarge the proxy to the
        // model's larger-than-native working size with a real filter instead of the box minifier. Created
        // lazily on the first super-native frame, released in Shutdown; sizes from the resources each call,
        // so a resolution change needs no rebuild.
        OS_Dx12* superUp = nullptr;

        // Supersampling down-leg: the native-sized buffer the Nx model answer is averaged into, and the
        // downscaler that does it. With superUp this lands the super-native answer at native for a 1:1
        // composite (no aliased minify). nrScaler is the filter both were built with, so a changed
        // DlssNrScalingDownscaler rebuilds them.
        ID3D12Resource* outputNative = nullptr;
        OS_Dx12* superDown = nullptr;
        Scaler nrScaler = Scaler::Count;

        // ResidualAcrossRR v2 (design/pre-sr-multipass.md "Across-RR residual"). With RunBeforeSR + the
        // game's Ray Reconstruction both on, the pre-SR seam runs the model but leaves Color untouched:
        // the resolve writes residualEdited instead of the game buffer
        // and dlssnr_residual.hlsl's Accumulate samples the game's active motion-vector rectangle
        // layer by those MVs and blends (residualEdited - hdrCopy) in at ResidualBlend -> the new
        // residualHistory. The per-frame ray-trace noise term of that delta averages to zero; the
        // enhancement persists. The post-SR shader samples the signed history at output resolution.
        // residualPair binds the result to its command list, parameters and output; every new pre
        // seam invalidates the old result. residualHistoryIndex ping-pongs the two history textures.
        ID3D12Resource* residualEdited = nullptr;   // resolve output on the pre-SR seam (render size)
        ID3D12Resource* residualHistory[2] = {};    // accumulated enhancement layer, ping-pong (render size)
        ID3D12Resource* residualComposed = nullptr; // Apply output = RR frame + delta (output size)
        unsigned int residualHistoryIndex = 0;      // which residualHistory holds the latest layer
        bool residualHistoryPrimed = false;         // false -> take the current delta whole (post-reset)
        bool residualStoreValid = false;
        bool residualModeActive = false;
        unsigned residualOutputWidth = 0, residualOutputHeight = 0;
        DXGI_FORMAT residualOutputFormat = DXGI_FORMAT_UNKNOWN;
        DlssNrResidualPair residualPair;

        // Frame hold (design/frame-hold.md): a persistent copy of the output taken on hold-on and restored
        // over the live output before the encode reads it while held, so a setting change re-renders the
        // same frame. heldWhitePoint is the snapshot used while held -- measurement is suspended.
        ID3D12Resource* heldColor = nullptr;
        bool heldActive = false;
        unsigned int heldWidth = 0;
        unsigned int heldHeight = 0;
        DXGI_FORMAT heldFormat = DXGI_FORMAT_UNKNOWN;
        float heldWhitePoint = 1.0f;

        unsigned int workWidth = 0;
        unsigned int workHeight = 0;

        // The white point meter.
        //
        // A 64x64 grid of tile luminances, copied to a readback buffer and looked at a few frames later.
        // Four buffers deep rather than one: the copy is recorded into the game's own command list and
        // there is no fence here to wait on, so the only thing making a read safe is that the frame it
        // came from is long retired. Three frames of distance is what the meter this replaces used.
        //
        // A stale read costs a slightly wrong float that the average below absorbs. A read of a buffer
        // still being written would cost the same, which is why the value is smoothed rather than used
        // raw.
        ID3D12Resource* meter = nullptr;
        ID3D12Resource* meterReadback[4] = {};

        // The calibration grid: what scale the game's buffer is on, measured from the untouched copy.
        // Its own surface and ring rather than sharing the meter's, because the two run at different
        // sizes -- the meter fetches one texel and this reads the whole frame.
        ID3D12Resource* calib = nullptr;
        ID3D12Resource* calibReadback[4] = {};
        unsigned long long calibFrames = 0;

        // The last few answers, so the menu can say how settled the number is. A suggestion taken during
        // a fade or a loading screen is worth less than one taken while standing still, and the spread
        // across recent frames is what tells them apart.
        static constexpr unsigned int kCalibHistory = 32;
        float calibHistory[kCalibHistory] = {};
        unsigned int calibCount = 0;
        float calibSuggestion = 0.0f;
        float calibSteadiness = 0.0f;
        bool calibUsable = false;
        const char* calibWhy = "measuring...";
        bool calibPassthrough = false;

        // Whether the frame that filled each readback slot actually had an exposure texture bound.
        //
        // The meter writes tile 0 from whatever sits in the exposure slot, and DispatchPass substitutes
        // the source picture when nothing is bound -- so without this the "exposure" read back is the red
        // channel of the frame's top-left pixel. In Cyberpunk, which supplies no exposure texture, that
        // pixel is scene content: it moved by up to 272x between consecutive frames and drove the white
        // point from 0.18 to 74. That is the whole frame flashing in luminance.
        //
        // The grid is read three frames after it is written, so the flag has to travel with the slot
        // rather than being asked of the current frame.
        bool meterExposureValid[4] = {};
        unsigned int meterSlot = 0;
        unsigned long long meterFrames = 0;

        // Whether the setting was on last frame, so the off->on edge can be caught.
        //
        // Deliberately the SETTING and not `wantExposure`: the texture itself comes and goes between
        // frames and holding the last good value across those gaps is the whole point of the field below.
        // Only the user turning the option back on means "anything held is from an unknown time ago".
        bool exposureSettingWasOn = false;

        // The game's exposure, as last read back, and the pre-exposure that goes with it. Held rather
        // than defaulted: the texture comes and goes between frames and a fallback to 1.0 on the gaps
        // would be a flicker source.
        float gameExposure = 0.0f;
        float gamePreExposure = 1.0f;

        // What the game OFFERS, as opposed to what has been read. Recorded from the parameter block every
        // frame whether or not the setting is on, and deliberately so: the menu has to be able to answer
        // "would this do anything here?" before the user turns it on, and reading a pointer for null costs
        // nothing. Whether it was ever offered is kept separately from whether it was offered this frame,
        // because games drop it on transitions -- GTA V dropped it three times in one session -- and one
        // absent frame is not the same answer as never.
        bool exposureOfferedNow = false;
        bool exposureEverOffered = false;
        unsigned long long exposureFrames = 0;

        // Cloned unconditionally when running at present, and only for typeless formats otherwise.
        ID3D12Resource* depthClone = nullptr;
        ID3D12Resource* motionClone = nullptr;

        // The constant-depth probe's surface. Separate from depthClone on purpose: it is defined by
        // never having been written, and sharing a surface with a mode that writes would destroy that.
        ID3D12Resource* depthConstant = nullptr;

        unsigned int width = 0;
        unsigned int height = 0;
        bool beforeUpscale = false;
        bool rayReconstruction = false;
        bool reset = true;

        // Dimensions of the guides as the upscaler handed them over, kept for the present path, which runs
        // long after that call has returned.
        unsigned int guideWidth = 0;
        unsigned int guideHeight = 0;

        // How the game encodes its guides, as the game itself reports it. Captured with the guides, since
        // the finished-frame path runs long after the upscaler's call has returned.
        bool guideDepthInverted = false;
        float guideMvScaleX = 1.0f;
        float guideMvScaleY = 1.0f;

        // The values each live feature was created with. Preset and style may differ per layer; the
        // remaining strengths are intentionally shared by the stack.
        unsigned int builtPreset[DlssNr::MaxPassCount] = {};
        float builtIntensity = 0.0f;
        NrPassTuning builtPassTuning[DlssNr::MaxPassCount] {};
        unsigned int builtStyle[DlssNr::MaxPassCount] = {};
        float builtLocalStructure = 0.0f;
        float builtLocalTone = 0.0f;
        float builtSkinStructure = 0.0f;
        bool builtAutoMask = false;
        unsigned long long settledAt = 0;

        // Once something fails there is no recovering it mid-session, and retrying every frame turns a
        // failure into a crash. It stays off and says why.
        bool failed = false;
        const char* reason = "";
    };

    NrState nr;
    DlssNr_Dx12& shader;

    // What the pass costs on the GPU, for the breakdown in the overlay.
    std::unique_ptr<DlssNrGpuTime> gpuTime;

    // A second timer, around the model's evaluate and nothing else.
    //
    // The first one brackets the whole pass, which is the number the menu shows and the right one for
    // "what does this feature cost". It is the wrong number for deciding what to optimise: the 4.10 ms at
    // full model resolution and 2.24 ms at half were both whole-pass, and both included this pass's own
    // encode and resolve at DISPLAY resolution plus the guide copies, none of which move when the model's
    // resolution does. Fitting a fixed term to those two points therefore attributes our own unchanging
    // work to NGX overhead.
    //
    // Splitting them says how much of the pass is the model and how much is ours -- and ours is the half
    // we can actually do something about.
    std::unique_ptr<DlssNrGpuTime> ngxTime;
    std::optional<double> lastNgxTime;
    std::optional<double> lastGpuTime;

    // Writes matched before/after frames on request, so comparisons stop depending on video.
    capture::FrameCapture captureFrames;

    // One capture happens on its own each session, so there is always a fresh sample without anyone having
    // to remember to ask. Started after the scene has had a moment to settle: the first frames after a
    // feature is built carry its reset, and are not representative of anything.
    static constexpr unsigned long long kAutoCaptureAfterFrames = 180;
    bool autoCaptureDone = false;

    // Cleared once per run, so a session's captures are its own and nothing accumulates across launches.

    unsigned long long frames = 0;

    // Logical frame identity for deferred pairing; feature readiness keeps the raw submission counter.
    DlssNrSeamClock seamClock;

    // A capture requested from outside the game: when the render path has no fence of its own, the write
    // waits until this frame count, by which point the GPU is certainly past the copies.
    unsigned long long captureWriteAtFrame = 0;

    // Dropping a file named dlssnr-capture.trigger beside OptiScaler requests a capture, so a session can
    // be asked for one from outside the game -- no alt-tab, no menu. Checked once a second, effectively.
    void CheckCaptureTrigger()
    {
        if ((frames % 60) != 0)
            return;

        std::error_code ec;
        const auto trigger = Util::DllPath().remove_filename() / "dlssnr-capture.trigger";

        if (std::filesystem::exists(trigger, ec))
        {
            std::filesystem::remove(trigger, ec);
            captureFrames.request(capture::kMaxFrames);
            LOG_INFO("DLSS-NR capture requested by trigger file");
        }
    }

    // The encoded mean is aimed here. Mid-grey rather than anything brighter: the model has to see both the
    // shadow detail it might lift and the highlights it must not blow out.
    static constexpr float kTargetEncodedMean = 0.45f;

    // How fast the derived value follows the scene. Readings arrive a few times a second, and an exposure
    // that lunges at every cut is worse than one that arrives a moment late.
    static constexpr float kWhitePointBlend = 0.25f;

    // Recomputes the white point from a measured mean. Inverting the encode for the white point that puts
    // that mean at the target gives wp = mean * (1 - t^g) / t^g.
    float WhitePointForMean(float meanLuma)
    {
        const float encoded = powf(kTargetEncodedMean, 2.2f);
        const float ratio = encoded / (1.0f - encoded);
        const float wp = meanLuma / ratio;
        // A black frame between scenes would otherwise drive this to zero and divide the next frame by it.
        return wp < 0.01f ? 0.01f : (wp > 10000.0f ? 10000.0f : wp);
    }

    std::filesystem::path dllDir;

    std::optional<std::filesystem::path> FindNvidiaModel()
    {
        auto path = Util::FindFilePath(dllDir, "nvngx_dlssnr.dll");
        if (!path.has_value())
            path = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");
        return path;
    }

    // Loads the forwarder that owns the calls into the snippet.
    bool EnsureForwarder()
    {
        if (nr.forwarder != nullptr)
            return nr.create != nullptr && nr.evaluate != nullptr;

        if (dllDir.empty())
            dllDir = Util::DllPath().remove_filename();

        // Beside OptiScaler first, then beside the executable: someone dropping this into a game folder may
        // reasonably put it in either place.
        auto found = Util::FindFilePath(dllDir, "nvngx.dll_dlssnr.dll");

        if (!found.has_value())
            found = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

        if (!found.has_value())
        {
            LOG_ERROR("nvngx.dll_dlssnr.dll not found beside OptiScaler ({}) or the game executable", dllDir.string());
            nr.reason = "nvngx.dll_dlssnr.dll is missing";
            return false;
        }

        // FindFilePath hands back the file itself, not the directory holding it.
        const auto path = found.value();
        nr.forwarder = LoadLibraryW(path.wstring().c_str());

        if (nr.forwarder == nullptr)
        {
            LOG_ERROR("nvngx.dll_dlssnr.dll found at {} but would not load, error {}", path.string(), GetLastError());
            nr.reason = "nvngx.dll_dlssnr.dll would not load";
            return false;
        }

        nr.queryRatio = (int (*)(const wchar_t*, void*, unsigned int, float*)) GetProcAddress(
            nr.forwarder, "dlssnr_query_scaling_ratio");
        nr.lastRatioStage = (const int*) GetProcAddress(nr.forwarder, "dlssnr_last_ratio_stage");

        nr.create = (PFN_NrCreate) GetProcAddress(nr.forwarder, "dlssnr_call_create");
        nr.evaluate = (PFN_NrEvaluate) GetProcAddress(nr.forwarder, "dlssnr_call_evaluate_v2");
        nr.release = (PFN_NrRelease) GetProcAddress(nr.forwarder, "dlssnr_call_release");
        // Optional: an older forwarder simply lacks it, and the model runs as before.
        nr.setExtras = (PFN_NrSetExtras) GetProcAddress(nr.forwarder, "dlssnr_call_set_extras");
        nr.setFloatSlot = (PFN_NrSetFloatSlot) GetProcAddress(nr.forwarder, "dlssnr_call_set_float_slot");
        nr.probeFloat = (PFN_NrProbeFloat) GetProcAddress(nr.forwarder, "dlssnr_call_probe_float");
        nr.lastInit = (int*) GetProcAddress(nr.forwarder, "dlssnr_call_last_init");
        nr.lastCreate = (int*) GetProcAddress(nr.forwarder, "dlssnr_call_last_create");
        nr.lastModelError = (const char* (*) ()) GetProcAddress(nr.forwarder, "dlssnr_call_error");

        if (nr.create == nullptr || nr.evaluate == nullptr)
        {
            nr.reason = "Update nvngx.dll_dlssnr.dll from the complete release (NR v2 exports required)";
            return false;
        }

        LOG_INFO("DLSS-NR forwarder loaded from {}", path.string());
        return true;
    }

    // The model needs the driver core's own capability block: it carries the snippet and preset callbacks a
    // feature expects at create time, which a freshly allocated block does not have.

    bool EnsureCapabilityParams(ID3D12Device* device)
    {
        if (nr.capabilityParams != nullptr)
            return true;

        if (!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device))
        {
            nr.reason = "the NGX core would not initialise";
            return false;
        }

        if (NVNGXProxy::D3D12_GetCapabilityParameters() == nullptr)
        {
            nr.reason = "the NGX core has no capability parameters";
            return false;
        }

        if (NVNGXProxy::D3D12_GetCapabilityParameters()(&nr.capabilityParams) != NVSDK_NGX_Result_Success ||
            nr.capabilityParams == nullptr)
        {
            nr.capabilityParams = nullptr;
            nr.reason = "the NGX core refused its capability parameters";
            return false;
        }

        // Before anything is written to it, work out where this block keeps floats.
        DiscoverFloatSlot(nr.capabilityParams);

        // Ask the model what scaling ratio it wants, once, for every quality level it might accept.
        //
        // Read-only and answered before any feature exists. The point is to find out whether NVIDIA's own
        // performance mode for this model is reachable: the snippet has ComputeScalingRatioCommon and the
        // kernel table has _ds, _upsample and _upsample_tilesync variants of every fused Swin block, which
        // together suggest the model can run its interior below display resolution natively -- rather than
        // being handed a picture we shrank ourselves, which costs an extra resample of the edit on the way
        // back and quantises the Swin grid to a lattice we chose rather than the one it was trained on.
        ReportScalingRatios();
        return true;
    }

    // What the model says it wants to run at, per quality level. Logged once, used for nothing yet.
    //
    // Answered by the snippet's own callback rather than chosen by us. If it answers, NVIDIA ships a
    // performance mode for Neural Rendering and the resolution slider is a worse hand-rolled version of
    // it. If it does not, the slider is all there is and that is worth knowing too.

    void ReportScalingRatios()
    {
        if (!nr.queryRatio || !nr.capabilityParams)
            return;
        auto snippet = Util::FindFilePath(dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            return;

        static const char* kNames[] = {
            "MaxPerf", "Balanced", "MaxQuality", "UltraPerformance", "UltraQuality", "DLAA"
        };

        char line[512] = {};
        size_t used = 0;
        bool any = false;

        for (unsigned int q = 0; q < 6; ++q)
        {
            float ratio = -1.0f;
            const int rc = nr.queryRatio(snippet->wstring().c_str(), nr.capabilityParams, q, &ratio);
            int written = 0;

            if (rc == 1)
            {
                any = true;
                written = snprintf(line + used, sizeof(line) - used, "%s=%.4f ", kNames[q], ratio);
            }
            else if (rc == -1)
            {
                written = snprintf(line + used, sizeof(line) - used, "%s=refused ", kNames[q]);
            }

            if (written > 0)
                used += (size_t) written;
        }

        if (any)
            LOG_INFO("DLSS-NR the model's own scaling ratios: {}", line);
        else
            LOG_INFO("DLSS-NR scaling ratio callback not published by this snippet (stage {})",
                     nr.lastRatioStage != nullptr ? *nr.lastRatioStage : -1);
    }

    // Works out which vtable slot this parameter block keeps floats in, by writing a known value through
    // each candidate and asking for it back through the header's typed getter. Only a slot that returns the
    // value it was given is accepted.
    //
    // Slot 1 is where the public header declares the float overload, so it is tried first and wins wherever
    // that assumption holds. It does not hold for the driver's own block: every float written there reads
    // back as FAIL_UnsupportedParameter while every uint lands, which is why intensity, local structure,
    // local tone and skin structure never did anything.

    void DiscoverFloatSlot(NVSDK_NGX_Parameter* params)
    {
        if (nr.floatSlotKnown || !params || !nr.probeFloat || !nr.setFloatSlot)
            return;
        nr.floatSlotKnown = true;

        static const char* kProbeKey = "DLSSNR.OptiScalerFloatProbe";
        static const int kCandidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
        const float expected = 0.375f; // exact in binary, so the round trip is exact or it is wrong

        for (int slot : kCandidates)
        {
            float readBack = 0.0f;
            nr.probeFloat(params, kProbeKey, expected, slot);

            if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
            {
                nr.setFloatSlot(slot);
                LOG_INFO("DLSS-NR float parameters go through vtable slot {}", slot);
                return;
            }
        }

        LOG_ERROR("DLSS-NR could not find the float setter: intensity, local structure, local tone and skin "
                  "structure will have no effect. The uint parameters still apply.");
    }

    // Switching inject points changes the surface format underneath the scratch set: the finished frame
    // works in the swapchain's format, the pre-frame-generation path in the upscaler's. A stale set either
    // clamps linear HDR into an 8-bit texture -- wrong brightness until something forces a rebuild -- or
    // hands CopyResource mismatched formats, which fails silently and makes the whole pass appear to do
    // nothing. So the set is torn down whenever the format it was built for is not the format needed now.
    // Retired model features and surfaces are parked and freed a comfortable number of evaluates later.
    // Releasing them immediately was the device hang: with frame generation the GPU runs several frames
    // behind, this work rides the game's own queue that no module fence covers, and an NGX feature or
    // scratch texture freed under in-flight work kills the device.
    struct NrRetired
    {
        void* feature = nullptr;
        ID3D12Resource* resource = nullptr;
        int framesLeft = 32;
    };

    std::vector<NrRetired> retired;

    void ParkNrFeature(void*& feature)
    {
        if (feature == nullptr)
            return;

        NrRetired r;
        r.feature = feature;
        feature = nullptr;
        retired.push_back(r);
    }

    void ParkNrResource(ID3D12Resource*& res)
    {
        if (res == nullptr)
            return;

        NrRetired r;
        r.resource = res;
        res = nullptr;
        retired.push_back(r);
    }

    void TickNrRetired()
    {
        for (size_t i = 0; i < retired.size();)
        {
            if (--retired[i].framesLeft > 0)
            {
                ++i;
                continue;
            }

            if (retired[i].feature != nullptr && nr.release != nullptr)
                nr.release(retired[i].feature);

            if (retired[i].resource != nullptr)
                retired[i].resource->Release();

            retired.erase(retired.begin() + i);
        }
    }

    // The inject point decides which buffer is being measured -- the upscaler's linear output or the
    // finished frame in swapchain format -- so a reading taken before a change describes a different
    // picture to one taken after. Everything else that depends on the format is invalidated here.
    void ForgetCalibration()
    {
        nr.calibCount = 0;
        nr.calibSuggestion = 0.0f;
        nr.calibSteadiness = 0.0f;
        nr.calibUsable = false;
        nr.calibWhy = "measuring...";
    }

    void ReleaseSurfacesIfFormatChanged(DXGI_FORMAT needed)
    {
        if (nr.output == nullptr || nr.output->GetDesc().Format == needed)
            return;

        LOG_INFO("DLSS-NR rebuilding surfaces: format {} -> {} (inject point changed)",
                 (int) nr.output->GetDesc().Format, (int) needed);

        ForgetCalibration();

        ParkNrFeature(nr.feature);
        nr.featurePendingSubmission = false;

        // The extras go with it: they were built for this raster and this tuning too.
        for (unsigned int i = 1; i < DlssNr::MaxPassCount; ++i)
        {
            ParkNrFeature(nr.passFeature[i]);
            nr.passNeedsReset[i] = false;
            nr.passCreateFailed[i] = false;
            nr.passPendingSubmission[i] = false;
        }

        for (ID3D12Resource** r : { &nr.output, &nr.passScratch, &nr.colorCopy, &nr.hdrCopy, &nr.colorSmall,
                                    &nr.outputNative, &nr.activeColor, &nr.residualEdited, &nr.residualHistory[0],
                                    &nr.residualHistory[1], &nr.residualComposed })
            ParkNrResource(*r);
        nr.residualStoreValid = false;
        nr.residualHistoryPrimed = false;
        nr.residualHistoryIndex = 0;

        nr.passScratchFailed = false;

        nr.reset = true;
    }

    // The meter's grid is R32_FLOAT, which makes a row exactly 64 * 4 = 256 bytes -- the alignment a
    // texture-to-buffer copy demands, met without padding, so the readback is a flat array of floats.
    static constexpr unsigned int kMeterRowBytes = kDlssNrMeterGrid * sizeof(float);
    static constexpr unsigned int kMeterBytes = kMeterRowBytes * kDlssNrMeterGrid;

    // Records the copy of this frame's grid into whichever readback buffer is furthest from being read.
    // Same shape as the meter's copy, against the calibration surface and its own ring.
    void CopyCalibrationToReadback(ID3D12GraphicsCommandList* cmdList)
    {
        const unsigned int slot = (unsigned int) (nr.calibFrames % 4);

        if (nr.calibReadback[slot] == nullptr || nr.calib == nullptr)
            return;

        D3D12_TEXTURE_COPY_LOCATION srcLoc {};
        srcLoc.pResource = nr.calib;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = nr.calibReadback[slot];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
        dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
        dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

        Barrier(cmdList, nr.calib, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
        Barrier(cmdList, nr.calib, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        nr.calibFrames++;
    }

    void CopyMeterToReadback(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, bool exposureBound)
    {
        const unsigned int slot = (unsigned int) (nr.meterFrames % 4);

        if (nr.meterReadback[slot] == nullptr)
            return;

        // Travels with the grid: read back three frames from now, alongside the tiles it describes.
        nr.meterExposureValid[slot] = exposureBound;

        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = nr.meter;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = nr.meterReadback[slot];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
        dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
        dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

        Barrier(cmdList, nr.meter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(cmdList, nr.meter, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        nr.meterFrames++;
    }

    // Takes the game's exposure out of tile 0 of the grid recorded three frames ago.
    //
    // Only tile 0 is written now. The frame-statistics meter this served was removed: a divisor measured
    // off a frame this pass writes is a feedback loop rather than a measurement. What is left is a
    // courier -- the game's exposure is a 1x1 texture in a resource state this pass did not set and must
    // not transition, so the shader reads it as an SRV and it rides home on a readback that exists.
    // Reads the calibration grid written four frames ago and turns it into one number.
    //
    // A high percentile of tile peaks, not the maximum: the maximum is a sun or a specular hit and would
    // normalise the whole picture into the dark. The 90th percentile is high enough to sit at the top of
    // the real range and common enough that no single highlight decides it.
    void ConsumeCalibrationReadback()
    {
        if (nr.calibFrames < 4)
            return;

        const unsigned int slot = (unsigned int) (nr.calibFrames % 4);
        ID3D12Resource* buffer = nr.calibReadback[slot];

        if (buffer == nullptr)
            return;

        void* mapped = nullptr;
        D3D12_RANGE range { 0, kMeterBytes };

        if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
            return;

        const float* src = (const float*) mapped;

        std::vector<float> tiles;
        tiles.reserve(kDlssNrMeterGrid * kDlssNrMeterGrid);

        for (unsigned int i = 0; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i)
        {
            if (std::isfinite(src[i]) && src[i] > 1e-6f)
                tiles.push_back(src[i]);
        }

        D3D12_RANGE nothingWritten { 0, 0 };
        buffer->Unmap(0, &nothingWritten);

        if (tiles.size() < 16)
            return;

        const size_t nth = (size_t) ((float) (tiles.size() - 1) * 0.90f);
        std::nth_element(tiles.begin(), tiles.begin() + nth, tiles.end());

        // How much of the frame carries light, measured against its own brightest tile rather than an
        // absolute threshold -- the units here are the game's and there is no absolute scale.
        //
        // This is what separates "the buffer is scaled by 240" from "I am standing in a dark cave". A
        // percentile of tile peaks is a statement about scene content; it only describes the buffer when
        // enough of the picture is lit for the top of the range to actually appear in it.
        float brightest = 0.0f;

        for (float v : tiles)
            brightest = std::max(brightest, v);

        unsigned int lit = 0;

        for (float v : tiles)
        {
            if (v > brightest * 0.10f)
                ++lit;
        }

        const float litFraction = tiles.empty() ? 0.0f : (float) lit / (float) tiles.size();

        // A torn readback survives isfinite and would clamp to exactly the ceiling, which since the
        // ceiling became 2000 is a value the slider can hold -- so a garbage frame could be offered as a
        // real answer. Reject rather than clamp.
        if (!(tiles[nth] > 0.0f) || tiles[nth] >= 1999.0f)
            return;

        const float suggestion = std::clamp(tiles[nth], 0.25f, 1990.0f);

        nr.calibUsable = !nr.calibPassthrough && litFraction > 0.20f;
        nr.calibWhy = nr.calibPassthrough
                          ? "this game hands over a frame it already tone mapped, so there is nothing to normalise"
                      : litFraction <= 0.20f ? "too little of this scene is lit to say where the top of the range is"
                                             : "";

        nr.calibHistory[nr.calibCount % NrState::kCalibHistory] = suggestion;
        nr.calibCount++;
        nr.calibSuggestion = suggestion;

        // Confidence is the spread of recent answers, not their absolute size. A number that has held
        // still for a second is one worth taking; one that is swinging means the scene is changing under
        // the measurement, and no single value would serve anyway.
        const unsigned int have = std::min<unsigned int>(nr.calibCount, NrState::kCalibHistory);

        if (have >= 8)
        {
            float lo = nr.calibHistory[0];
            float hi = nr.calibHistory[0];

            for (unsigned int i = 0; i < have; ++i)
            {
                lo = std::min(lo, nr.calibHistory[i]);
                hi = std::max(hi, nr.calibHistory[i]);
            }

            // A spread of 1.0x is perfect agreement and 2x or worse is none.
            const float spread = hi / lo;
            nr.calibSteadiness = std::clamp(1.0f - (spread - 1.0f), 0.0f, 1.0f);
        }
    }

    void ConsumeMeterReadback()
    {
        if (nr.meterFrames < 4)
            return;

        const unsigned int slot = (unsigned int) (nr.meterFrames % 4);
        ID3D12Resource* buffer = nr.meterReadback[slot];

        if (buffer == nullptr)
            return;

        void* mapped = nullptr;
        D3D12_RANGE range { 0, sizeof(float) };

        if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
            return;

        const float* src = (const float*) mapped;

        // Only believed when the frame that wrote this grid actually had an exposure texture bound. With
        // nothing bound DispatchPass substitutes the source picture, and tile 0 is then a scene pixel
        // rather than an exposure -- believing it made the white point follow the top-left corner of the
        // screen, which in Cyberpunk moved by up to 272x between frames and flashed the whole picture.
        //
        // When it is not believed gameExposure keeps its last good value, or stays 0 and lets
        // ResolveWhitePoint fall back to the slider, which is what a game supplying none should get.
        if (nr.meterExposureValid[slot] && std::isfinite(src[0]) && src[0] > 0.0f)
            nr.gameExposure = src[0];

        D3D12_RANGE nothingWritten { 0, 0 };
        buffer->Unmap(0, &nothingWritten);
    }

    // Forget everything the meter knows, so nothing read before this moment can be believed after it.
    //
    // The exposure is written only inside the block that dispatches the meter, and that block does not
    // run while the option is off. Nothing used to clear any of this when it stopped, so the reading
    // simply froze: switching the option back on returned the value from whenever it was switched off,
    // and ResolveWhitePoint took it as current because a held value is exactly what it expects to see.
    // GTA V's exposure spans 0.127 to 0.511 in one session, so re-enabling in different light handed the
    // encode a white point up to 4x wrong -- which trips the soft knee, scales the model's answer away
    // and leaves its hue behind. That is the colour cast, and it looked random because it depends on the
    // light at the moment of the PREVIOUS switch-off, which nothing on screen shows.
    //
    // The readback ring made it worse. `meterFrames` also only advances inside that block, so the four
    // slots kept their contents and their valid flags across the gap, and the first frames after
    // re-enabling consumed buffers written before it as though they had just arrived.
    //
    // Zero is not a fallback value here, it is the absence of one: ResolveWhitePoint's `> 1e-6f` guard
    // fails and the manual slider is used, which is what a game supplying no exposure already gets.
    void InvalidateExposureMeter()
    {
        nr.gameExposure = 0.0f;

        for (bool& valid : nr.meterExposureValid)
            valid = false;

        // Re-arms the `< 4` guard in ConsumeMeterReadback, so nothing is read back until four frames
        // have genuinely been queued since this point.
        nr.meterFrames = 0;
    }

    // Turns what the meter saw into the divisor the encode uses, or falls back to the slider.
    //
    // `cut` says the exposure may jump rather than drift, and it is the difference between this working
    // and not. GTA V's character switch pulls the camera up through the sky: a linear HDR buffer's sky is
    // tens of times brighter than the ground, the proxy clips to flat white, and the frame blows out until
    // the camera comes back down. Easing across that at two percent a frame takes three and a half
    // seconds, which is longer than the transition -- so a meter that only eases would lag through the
    // whole thing and fix nothing.
    //
    // So a cut snaps and a drift eases. Walking out of a cave is a drift; a camera cut is not, and
    // pretending otherwise to avoid pumping just moves the failure somewhere more visible.
    float ResolveWhitePoint(const Config& cfg, bool isHdrBuffer)
    {
        const float slider = cfg.DlssNrWhitePointScale.value_or_default();

        // A frame the game already tone mapped is display-referred: white is at 1 by definition and there
        // is nothing to measure. The slider stays available as a manual exposure on that path.
        if (!isHdrBuffer)
            return slider;

        // The game's own exposure, where it supplies one.
        //
        // Exposure is the step that makes a cave and a field comparable: the renderer works in arbitrary
        // scene-referred units and multiplies by this before tone mapping, which is precisely why one
        // fixed paper white cannot serve both. FSR spells the relationship out -- frame / preExposure *
        // exposure -- so undoing it gives the divisor this pass wants, and paper white becomes a constant
        // on top rather than a value chasing the scene.
        //
        // Unlike anything measured off the frame this cannot be moved by what the pass writes, which is
        // what killed the statistical meter. It is the game's number, decided upstream.
        //
        // Held across the frames where the texture is absent -- GTA V dropped it three times in one
        // session -- because falling back to a default on those frames is a flicker, not a fallback.
        // The scan's anchor, where the game supplies no exposure of its own.
        //
        // Only ratios are used, so the units of the buffer never have to be known -- which is the whole
        // reason this is anchored rather than absolute. The anchor is the user's own white point at the
        // moment they pressed the button; everything after that is the scan moving it.
        //
        // Deliberately below the exposure texture in priority and mutually exclusive with it in the
        // menu. A game that hands over a real exposure has no business being driven by a buffer found by
        // its shape, and two sources fighting over one number is the class of bug worth making
        // unreachable rather than merely unlikely.
        if (cfg.DlssNrWhitePointSource.value_or_default() == 2)
        {
            // Multi-point: one or more calibration points the user placed, interpolated in log space by
            // the current scan value. One point is the original ratio law; more fit the buffer's actual
            // relationship so the white point holds across the whole range, not only near one anchor.
            const float w = DlssNr::ExposureScan::AnchoredWhitePoint(DlssNr::ExposureScan::BestValue(),
                                                                     cfg.DlssNrScanInverted.value_or_default(),
                                                                     cfg.DlssNrScanTrim.value_or_default());

            if (w > 0.0f)
                return w;
        }

        if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && nr.gameExposure > 1e-6f)
        {
            // Its own setting, not the manual divisor. See Config: they are different quantities with
            // different units and different sensible ranges, and sharing one value meant adjusting the
            // trim destroyed the divisor somebody had found by hand.
            //
            // Still bounded at the point of use rather than only in the menu that draws it.
            //
            // Bounding it at the slider would have been cosmetic: someone who found 64 by hand on the
            // manual path and then switched the exposure source on keeps that 64 in their ini, and the
            // composition would go on reading it until they happened to touch the control. The picture
            // would be wrong for a reason the menu was no longer showing.
            //
            // Their value is left in the config untouched, so switching back to manual restores the
            // number they arrived at. It is only what this path consumes that is limited.
            const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);

            return std::clamp(nr.gamePreExposure / nr.gameExposure * trim, 0.01f, 4096.0f);
        }

        // Otherwise the slider, and only the slider.
        //
        // Measuring white from the frame was tried and removed. It could not be made to work because the
        // pass writes the frame it measures: in Enshrouded one session walked the divisor from 0.010 to
        // 97.910, and toggling NR at a fixed spot read 41.31 off and 0.46 on. Two attempts to damp it --
        // a relative lit threshold, then a rate limit with a cut snap -- both treated a coupled system as
        // a noisy one and neither held. A constant cannot do that, which is the whole argument for it,
        // and is what RenoDX has always done.
        return slider;
    }

    ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width, unsigned int height)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        // The model writes its result, so the destination has to be a UAV.
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ID3D12Resource* res = nullptr;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        nullptr, IID_PPV_ARGS(&res));
        return res;
    }

    void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
                 D3D12_RESOURCE_STATES to)
    {
        if (from == to)
            return;
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        cmdList->ResourceBarrier(1, &b);
    }

    // A typeless resource cannot be viewed, and NGX builds its own views with nothing to tell it which
    // format to use. Depth is very often declared typeless, so the typed member of the same family is
    // substituted; CopyResource accepts that as a destination for the typeless original.
    DXGI_FORMAT TypedGuideFormat(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R32_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
            return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R24G8_TYPELESS:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R32G32_TYPELESS:
            return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16_TYPELESS:
            return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return f;
        }
    }

    bool IsTypeless(DXGI_FORMAT f) { return TypedGuideFormat(f) != f; }

    // Creates a typed twin of a guide buffer, matching everything but the format.
    ID3D12Resource* CreateGuideClone(ID3D12Device* device, ID3D12Resource* source)
    {
        D3D12_RESOURCE_DESC desc = source->GetDesc();
        desc.Format = TypedGuideFormat(desc.Format);
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        ID3D12Resource* res = nullptr;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&res));
        return res;
    }

    // Hands back something the model can actually read: the guide itself when it is typed, or a typed copy
    // of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time, which is
    // a documented contract rather than a guess about any one game's frame graph, so that is the state
    // transitioned away from and back to here.
    // Freezing is a diagnostic, and it reuses this function because the clone it already keeps is
    // exactly the thing a frozen guide is: a private copy the model reads instead of the live resource.
    // Freezing is then not a new mechanism but the absence of one -- stop refreshing the copy.
    //
    // A frozen guide is valid data that is wrong for this frame, which is a far better probe than a
    // constant would be. A constant is degenerate and a model may special-case it; stale depth is
    // ordinary depth that simply disagrees with the picture, and anything reading it has to notice.
    // Hands back something the model can actually read: the guide itself when it is typed, or a typed
    // copy of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time,
    // which is a documented contract rather than a guess about any one game's frame graph, so that is
    // the state transitioned away from and back to here.
    ID3D12Resource* ReadableGuide(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                                  ID3D12Resource** clone)
    {
        if (source == nullptr || !IsTypeless(source->GetDesc().Format))
            return source;

        // A dynamic-resolution game reallocates its depth and motion vectors as the render size moves, so
        // the clone made for the old size no longer matches -- and CopyResource demands identical
        // dimensions. Copying a 1970x1108 source into a 984x554 clone is undefined and removes the device,
        // which is the DRS crash. Rebuild the clone whenever the source's shape has changed under it.
        if (*clone != nullptr)
        {
            const D3D12_RESOURCE_DESC have = (*clone)->GetDesc();
            const D3D12_RESOURCE_DESC want = source->GetDesc();

            if (have.Width != want.Width || have.Height != want.Height || have.Format != TypedGuideFormat(want.Format))
            {
                // Retired, not released: the previous copy may still be in flight on the game's queue.
                ParkNrResource(*clone);
            }
        }

        if (*clone == nullptr)
        {
            *clone = CreateGuideClone(device, source);

            if (*clone == nullptr)
                return nullptr;

            LOG_DEBUG("DLSS-NR cloned a typeless guide as format {}", (int) TypedGuideFormat(source->GetDesc().Format));
        }

        Barrier(cmdList, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->CopyResource(*clone, source);
        Barrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmdList, *clone, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return *clone;
    }

    // The upscaler's own names differ between super resolution and ray reconstruction, and only one set is
    // present on any given block.
    // Whether a surface can physically hold linear HDR.
    //
    // Only a float format can: linear light is open-ended and runs far past 1.0, which a normalised
    // integer surface cannot represent. An 8-bit UNORM frame is finished, display-referred output, and
    // so is a 10-bit one -- HDR10 is PQ-encoded, which is display-referred too.
    //
    // The game's IsHDR flag is a statement of intent that is not always true, and believing it over a
    // format that cannot hold linear light means encoding an already-encoded frame a second time.
    bool FormatCanHoldLinearHdr(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return true;
        default:
            return false;
        }
    }

    ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* a, const char* b)
    {
        ID3D12Resource* res = nullptr;

        if (params->Get(a, &res) == NVSDK_NGX_Result_Success && res != nullptr)
            return res;

        res = nullptr;

        if (params->Get(b, &res) == NVSDK_NGX_Result_Success && res != nullptr)
            return res;

        // The same key again, as a plain pointer.
        //
        // NVSDK_NGX_Parameter has a typed setter per resource kind and an untyped one, and on a real NGX
        // parameter block those are separate slots: what goes in through Set(name, void*) does not come
        // back out of Get(name, ID3D12Resource**). A game running its own D3D12 upscaler sets these
        // typed, so the typed read above is enough and always was.
        //
        // Both of OptiScaler's bridges write them untyped. IFeature_Dx11wDx12 and IFeature_VkwDx12 turn
        // the game's D3D11 textures or Vulkan images into D3D12 resources and hand them over with
        // Set(name, (void*) resource) -- so the typed read came back null a few lines after the resource
        // had been written, and the pass quietly did nothing. That is the whole reason this never ran in
        // a DirectX 11 or Vulkan game.
        void* untyped = nullptr;

        if (params->Get(a, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
            return static_cast<ID3D12Resource*>(untyped);

        untyped = nullptr;

        if (params->Get(b, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
            return static_cast<ID3D12Resource*>(untyped);

        return nullptr;
    }

    // A change has to hold still before it is acted on: a slider being dragged reports a new value every
    // frame, and each one would otherwise mean a new model.
    static constexpr unsigned long long kSettleFrames = 30;

    // The extras the official integration sets: global tone (read at create) and the interface inputs.
    // Written before every create and evaluate, nulls included, so nothing stale ever sits in the block.
    void SetExtras(const Config& cfg, ID3D12Resource* ui, ID3D12Resource* backbuffer, unsigned int uiWidth,
                   unsigned int uiHeight, unsigned int bbWidth, unsigned int bbHeight)
    {
        if (nr.setExtras == nullptr || nr.capabilityParams == nullptr)
            return;

        // Global tone is written at the model's own default: the control that exposed it changed nothing
        // that could be seen, and the block persists, so a value still has to be put there.
        nr.setExtras(nr.capabilityParams, 1.0f, ui, ui, backbuffer, uiWidth, uiHeight, bbWidth, bbHeight);
    }

    bool TuningMatchesFeature(const Config& cfg, unsigned int requestedPasses)
    {
        for (unsigned int pass = 0; pass < requestedPasses; ++pass)
        {
            // A profile cannot be stale until its feature exists. This lets a user prepare pass 2 or 3
            // while running fewer layers without needlessly rebuilding pass 1.
            if (pass > 0 && nr.passFeature[pass] == nullptr)
                continue;

            if (nr.builtPassTuning[pass] != PassTuning(cfg, pass) || nr.builtPreset[pass] != PassPreset(cfg, pass) ||
                nr.builtStyle[pass] != PassStyle(cfg, pass))
                return false;
        }

        return true;
    }

    void RecordBuiltPrimaryTuning(const Config& cfg)
    {
        nr.builtPassTuning[0] = PassTuning(cfg, 0);
        nr.builtPreset[0] = PassPreset(cfg, 0);
        nr.builtIntensity = cfg.DlssNrIntensity.value_or_default();
        nr.builtStyle[0] = PassStyle(cfg, 0);
        nr.builtLocalStructure = cfg.DlssNrLocalStructure.value_or_default();
        nr.builtLocalTone = cfg.DlssNrLocalTone.value_or_default();
        nr.builtSkinStructure = cfg.DlssNrSkinStructure.value_or_default();
        nr.builtAutoMask = cfg.DlssNrAutoMask.value_or_default();
    }

    // Guards the module's state. Every caller is now on the game's render thread, so this is no longer
    // holding two threads apart -- but the D3D11-on-D3D12 bridge enters from its own call site, and the
    // cost is a CPU-side lock on a path that already records command lists.
    std::recursive_mutex mutex;

    // Runs the pass inside the same state envelope every other OptiScaler compute pass runs in.
    //
    // The upscaler's own evaluate is wrapped like this by TryEvaluateOptiFeature: root-signature tracking
    // off so the hooks do not record the pass's binds as the game's, heap capture skipped, and RestoreRoot
    // afterwards to put the game's compute state back. Neural Rendering ran outside that envelope -- after
    // the upscaler had already restored and re-armed -- so it left its own root signature and descriptor
    // heaps bound and captured. On an ordinary engine the game rebinds and never notices. On a bindless
    // engine (007 First Light, Monster Hunter Wilds, and the rest of the RestoreComputeSig* quirks) the
    // game resumes off the pass's bindings and the device is removed.
    //
    // As RAII so every early return from the pass is covered. RestoreRoot is gated internally on the
    // RestoreComputeSignature / RestoreGraphicSignature config, so this is a no-op on games that do not
    // ask for it and only acts where it is needed.
    struct ScopedNrStateEnvelope
    {
        ID3D12GraphicsCommandList* cmd;
        ScopedSkipHeapCapture skipHeap;

        bool tracking;
        explicit ScopedNrStateEnvelope(ID3D12GraphicsCommandList* c)
            : cmd(c), tracking(D3D12Hooks::IsRootSignatureTrackingEnabled())
        {
            D3D12Hooks::SetRootSignatureTracking(false);
        }

        ~ScopedNrStateEnvelope()
        {
            if (tracking)
                D3D12Hooks::RestoreRoot(cmd);
            D3D12Hooks::SetRootSignatureTracking(tracking);
        }
    };

    // Every way out of the pass before it does anything is silent on purpose -- an evaluate that carries
    // no depth is normal and would otherwise print every frame forever. That silence is fine until the
    // pass does nothing at all and the log has no opinion about why.
    //
    // So each distinct reason is reported once. Once, not once per frame.
    void ReportSkipOnce(const char* reason)
    {

        if (seen.insert(reason).second)
            LOG_INFO("DLSS-NR did not run: {}", reason);
    }

    struct DeferredSrContext
    {
        State& owner;
        explicit DeferredSrContext(State& state) : owner(state) {}

        static constexpr unsigned MarkerCount = 16;
        struct HalfRate
        {
            std::unique_ptr<ResidualFg> fg;
            ID3D12Resource *motion = nullptr, *previousMotion = nullptr, *anchorMotion = nullptr,
                           *interpolated = nullptr, *suppression = nullptr, *suppressionTexture = nullptr,
                           *zeroUpload = nullptr, *history[2] {};
            bool ready = false, failed = false, havePrevious = false, previousWasAnchor = false;
            bool motionReadable = false, previousReadable = false, anchorReadable = false;
            bool historyReadable[2] {}, interpolationReadable = false;
            bool suppressionReadable = false;
            unsigned writeIndex = 0;
            float historyScale[2] { 1, 1 };
            unsigned long long lastEpoch = 0, createEpoch = 0, anchorId = 0;
            unsigned long long nrAnchors = 0, skippedNr = 0;
            ResidualFgCamera camera;
            ~HalfRate()
            {
                // Generation's GPU completion markers protect all of these lifetimes.
                fg.reset();
                for (auto* r : { motion, previousMotion, anchorMotion, interpolated, suppression, suppressionTexture,
                                 zeroUpload, history[0], history[1] })
                    if (r)
                        r->Release();
            }
            void Reset()
            {
                havePrevious = false;
                previousWasAnchor = false;
            }
        };
        struct Generation
        {
            ID3D12Device* device = nullptr;
            ID3D12CommandQueue* queue = nullptr; // identity/reference only; no private submissions
            unsigned w = 0, h = 0, outW = 0, outH = 0, flags = 0;
            DXGI_FORMAT inputFormat {}, outputFormat {};
            ID3D12Resource *edited = nullptr, *residualInput = nullptr, *residualOutput = nullptr, *clean = nullptr,
                           *composed = nullptr, *exposure = nullptr, *readback = nullptr;
            ID3D12QueryHeap* queries = nullptr;
            volatile UINT64* completed = nullptr;
            bool occupied[MarkerCount] {};
            unsigned nextMarker = 0, lastMarker = 0;
            bool everRecorded = false, smallReadable = false, reset = true, failed = false;
            NVSDK_NGX_Parameter* parameters = nullptr;
            NVSDK_NGX_Handle* feature = nullptr;
            unsigned long long createEpoch = 0;
            unsigned long long lastBeginEpoch = 0;
            bool began = false;
            std::unique_ptr<DlssNr_Dx12> codec;
            bool halfRequested = false, approximateCamera = false;
            std::string halfStatus;
            bool sampleAndHold = false;
            DlssNrResidualHold hold;
            ID3D12Resource* zeroMotion = nullptr;
            std::unique_ptr<HalfRate> half;

            bool Idle() const { return !everRecorded || completed[lastMarker] != 0; }
            ~Generation()
            {
                half.reset();
                if (zeroMotion)
                    zeroMotion->Release();
                if (feature && NVNGXProxy::D3D12_ReleaseFeature())
                    NVNGXProxy::D3D12_ReleaseFeature()(feature);
                if (parameters && NVNGXProxy::D3D12_DestroyParameters())
                    NVNGXProxy::D3D12_DestroyParameters()(parameters);
                if (readback && completed)
                    readback->Unmap(0, nullptr);
                for (auto* r : { edited, residualInput, residualOutput, clean, composed, exposure, readback })
                    if (r)
                        r->Release();
                if (queries)
                    queries->Release();
                if (queue)
                    queue->Release();
                if (device)
                    device->Release();
            }
        };

        // Record an actual GPU completion marker after EACH seam. A later CPU frame/Present count alone
        // does not prove a resource is no longer in flight. Slots aren't reused until the GPU wrote them.
        struct Use
        {
            Generation& g;
            ID3D12GraphicsCommandList* cmd;
            unsigned slot;
            bool valid;
            Use(Generation& gen, ID3D12GraphicsCommandList* commands) : g(gen), cmd(commands), slot(g.nextMarker)
            {
                valid = !g.occupied[slot] || g.completed[slot] != 0;
                if (!valid)
                    return;
                g.completed[slot] = 0;
                g.occupied[slot] = true;
                g.lastMarker = slot;
                g.everRecorded = true;
                g.nextMarker = (slot + 1) % MarkerCount;
            }
            ~Use()
            {
                if (!valid)
                    return;
                cmd->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, slot);
                cmd->ResolveQueryData(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, slot, 1, g.readback,
                                      slot * sizeof(UINT64));
            }
        };

        std::unique_ptr<Generation> current;
        std::vector<std::unique_ptr<Generation>> retired;
        std::string status = "not started";
        struct Pending
        {
            ID3D12GraphicsCommandList* cmd = nullptr;
            NVSDK_NGX_Parameter* caller = nullptr;
            ID3D12Resource* output = nullptr;
            unsigned long long epoch = 0;
            float scale = 1;
            bool skipNr = false;
            bool half = false;
        } pending;

        void Say(const std::string& text)
        {
            if (status == text)
                return;
            status = text;
            LOG_INFO("DLSS-NR deferred DLSS: {}", text);
        }
        void Cancel()
        {
            pending = {};
            if (current)
            {
                current->reset = true;
                current->hold.Reset();
                if (current->half)
                    current->half->Reset();
            }
        }
        void Collect()
        {
            std::erase_if(retired, [](const auto& g) { return g->Idle(); });
        }

        unsigned UInt(NVSDK_NGX_Parameter* p, const char* key, unsigned fallback = 0)
        {
            unsigned value = fallback;
            p->Get(key, &value);
            return value;
        }
        float Float(NVSDK_NGX_Parameter* p, const char* key, float fallback)
        {
            float value = fallback;
            p->Get(key, &value);
            return std::isfinite(value) ? value : fallback;
        }

        bool Allocate(Generation& g)
        {
            g.edited = owner.CreateScratch(g.device, g.inputFormat, g.w, g.h);
            g.residualInput = owner.CreateScratch(g.device, DXGI_FORMAT_R16G16B16A16_FLOAT, g.w, g.h);
            g.residualOutput = owner.CreateScratch(g.device, DXGI_FORMAT_R16G16B16A16_FLOAT, g.outW, g.outH);
            g.clean = owner.CreateScratch(g.device, g.outputFormat, g.outW, g.outH);
            g.composed = owner.CreateScratch(g.device, g.outputFormat, g.outW, g.outH);
            g.exposure = owner.CreateScratch(g.device, DXGI_FORMAT_R32_FLOAT, 1, 1);
            if (!g.edited || !g.residualInput || !g.residualOutput || !g.clean || !g.composed || !g.exposure)
                return false;
            g.codec = std::make_unique<DlssNr_Dx12>("Deferred NR contribution", g.device);
            if (!g.codec->IsInit())
                return false;
            D3D12_QUERY_HEAP_DESC query {};
            query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            query.Count = MarkerCount;
            if (FAILED(g.device->CreateQueryHeap(&query, IID_PPV_ARGS(&g.queries))))
                return false;
            auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
            auto desc = CD3DX12_RESOURCE_DESC::Buffer(MarkerCount * sizeof(UINT64));
            if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&g.readback))))
                return false;
            void* mapped = nullptr;
            if (FAILED(g.readback->Map(0, nullptr, &mapped)))
                return false;
            g.completed = static_cast<volatile UINT64*>(mapped);
            for (unsigned i = 0; i < MarkerCount; ++i)
                g.completed[i] = 0;
            return true;
        }

        bool CreateHalfRate(Generation& g, ID3D12GraphicsCommandList* cmd, unsigned long long epoch)
        {
            g.half = std::make_unique<HalfRate>();
            auto& h = *g.half;
            h.motion = owner.CreateScratch(g.device, DXGI_FORMAT_R32G32B32A32_FLOAT, g.w, g.h);
            h.previousMotion = owner.CreateScratch(g.device, DXGI_FORMAT_R32G32B32A32_FLOAT, g.w, g.h);
            h.anchorMotion = owner.CreateScratch(g.device, DXGI_FORMAT_R32G32B32A32_FLOAT, g.w, g.h);
            h.interpolated = owner.CreateScratch(g.device, DXGI_FORMAT_R16G16B16A16_FLOAT, g.outW, g.outH);
            h.suppressionTexture = owner.CreateScratch(g.device, DXGI_FORMAT_R8_UNORM, 1, 1);
            for (auto& r : h.history)
                r = owner.CreateScratch(g.device, g.outputFormat, g.outW, g.outH);
            auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            auto desc = CD3DX12_RESOURCE_DESC::Buffer(256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                         IID_PPV_ARGS(&h.suppression))))
                return false;
            heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            desc = CD3DX12_RESOURCE_DESC::Buffer(256);
            if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&h.zeroUpload))))
                return false;
            void* zero = nullptr;
            if (FAILED(h.zeroUpload->Map(0, nullptr, &zero)))
                return false;
            std::memset(zero, 0, 256);
            h.zeroUpload->Unmap(0, nullptr);
            if (!h.motion || !h.previousMotion || !h.anchorMotion || !h.interpolated || !h.suppressionTexture ||
                !h.history[0] || !h.history[1])
                return false;
            ResidualFgApi api { NVNGXProxy::D3D12_GetCapabilityParameters(),
                                NVNGXProxy::D3D12_AllocateParameters(),
                                NVNGXProxy::D3D12_DestroyParameters(),
                                NVNGXProxy::D3D12_CreateFeature(),
                                [](ID3D12GraphicsCommandList* c, const NVSDK_NGX_Handle* f, NVSDK_NGX_Parameter* p,
                                   PFN_NVSDK_NGX_ProgressCallback cb)
                                { return NVNGXProxy::D3D12_EvaluateFeature()(c, f, p, cb); },
                                NVNGXProxy::D3D12_ReleaseFeature() };
            h.fg = std::make_unique<ResidualFg>(api);
            auto result = h.fg->Create(cmd, g.outW, g.outH, g.w, g.h);
            if (result != NVSDK_NGX_Result_Success)
            {
                g.halfStatus = "FG creation failed: " + std::to_string((unsigned) result);
                return false;
            }
            h.createEpoch = epoch;
            h.ready = true;
            return true;
        }

        bool PrepareHalfRate(Generation& g, ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* source,
                             ID3D12Resource* motion, unsigned long long epoch, unsigned long long submittedEpoch)
        {
            if (!g.halfRequested)
                return false;
            if (!(g.flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) ||
                (g.flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered))
            {
                g.halfStatus = "requires low-resolution, non-jittered motion";
                return false;
            }
            // This initial bridge integration explicitly requires opt-in approximate guides.
            // Never mark invented matrices as game-supplied camera data.
            if (!g.approximateCamera)
            {
                g.halfStatus = "explicit approximate-camera opt-in required";
                return false;
            }
            if (!g.half)
            {
                g.halfStatus = "initializing NVIDIA FG";
                if (!CreateHalfRate(g, cmd, submittedEpoch))
                    g.half->failed = true;
                return false;
            }
            auto& h = *g.half;
            if (!h.ready || h.failed || submittedEpoch == h.createEpoch)
                return false;
            if (g.reset || UInt(source, NVSDK_NGX_Parameter_Reset) || epoch != h.lastEpoch + 1)
                h.Reset();
            h.lastEpoch = epoch;
            auto desc = motion->GetDesc();
            if (desc.Width < g.w || desc.Height < g.h || desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
            {
                h.Reset();
                g.halfStatus = "unsupported motion texture";
                return false;
            }
            if (h.motionReadable)
                owner.Barrier(cmd, h.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            DlssNrConstants normalize {};
            normalize.Mode = DlssNrMode_NormalizeMotion;
            normalize.Width = g.w;
            normalize.Height = g.h;
            normalize.MvScaleX = Float(source, NVSDK_NGX_Parameter_MV_Scale_X, 1) / g.w;
            normalize.MvScaleY = Float(source, NVSDK_NGX_Parameter_MV_Scale_Y, 1) / g.h;
            bool ok =
                g.codec->DispatchPass(cmd, normalize, motion, nullptr, nullptr, nullptr, nullptr, h.motion, nullptr);
            owner.Barrier(cmd, h.motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            h.motionReadable = true;
            if (!ok)
            {
                h.Reset();
                g.halfStatus = "motion normalization failed";
                return false;
            }
            if (h.havePrevious)
            {
                if (h.anchorReadable)
                    owner.Barrier(cmd, h.anchorMotion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                normalize.Mode = DlssNrMode_ComposeMotion;
                ok = g.codec->DispatchPass(cmd, normalize, h.motion, h.previousMotion, nullptr, nullptr, nullptr,
                                           h.anchorMotion, nullptr);
                owner.Barrier(cmd, h.anchorMotion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                h.anchorReadable = true;
                if (!ok)
                {
                    h.Reset();
                    g.halfStatus = "motion composition failed";
                    return false;
                }
            }
            using namespace DirectX;
            const auto& cfg = *Config::Instance();
            float nearPlane = cfg.FsrCameraNear.value_or_default(), farPlane = cfg.FsrCameraFar.value_or_default();
            float fov = cfg.FsrVerticalFov.value_or_default() * XM_PI / 180.0f;
            if (!(nearPlane > 0 && farPlane > nearPlane && fov > 0.01f && fov < XM_PI - 0.01f))
            {
                h.Reset();
                g.halfStatus = "invalid approximate camera parameters";
                return false;
            }
            if (g.flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted)
                std::swap(nearPlane, farPlane);
            auto projection = XMMatrixPerspectiveFovRH(fov, (float) g.outW / g.outH, nearPlane, farPlane);
            XMFLOAT4X4 temp;
            XMStoreFloat4x4(&temp, projection);
            std::memcpy(h.camera.viewToClip, &temp, sizeof(temp));
            XMStoreFloat4x4(&temp, XMMatrixInverse(nullptr, projection));
            std::memcpy(h.camera.clipToView, &temp, sizeof(temp));
            XMStoreFloat4x4(&temp, XMMatrixIdentity());
            std::memcpy(h.camera.clipToPrevious, &temp, sizeof(temp));
            std::memcpy(h.camera.previousToClip, &temp, sizeof(temp));
            h.camera.up[1] = h.camera.right[0] = 1;
            h.camera.forward[2] = -1;
            h.camera.nearPlane = nearPlane;
            h.camera.farPlane = farPlane;
            h.camera.fov = fov;
            h.camera.aspect = (float) g.outW / g.outH;
            h.camera.valid = true;
            g.halfStatus.clear();
            return true;
        }

        void Before(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* source, unsigned long long epoch,
                    unsigned long long submittedEpoch, ID3D12CommandQueue* queue, bool privateJob = false)
        {
            if (pending.cmd && current)
            {
                LOG_DEBUG(
                    "DLSS-NR deferred: Before entry with a stale pending (previous After never ran) -> reset. epoch {}",
                    epoch);
                current->reset = true; // abandoned/failed main SR call
            }
            pending = {};
            struct ResetOnGap
            {
                DeferredSrContext& state;
                unsigned long long epoch;
                ~ResetOnGap()
                {
                    if (!state.pending.cmd && state.current)
                    {
                        LOG_DEBUG("DLSS-NR deferred: Before returned without arming a seam -> reset + hold/half "
                                  "cleared. epoch {}",
                                  epoch);
                        state.current->reset = true;
                        state.current->hold.Reset();
                        if (state.current->half)
                            state.current->half->Reset();
                    }
                }
            } resetOnGap { *this, epoch };
            Collect();
            const auto& cfg = *Config::Instance();
            if (cfg.DlssNrUseProxy.value_or_default() || cfg.DlssNrHoldFrame.value_or_default() ||
                cfg.DlssNrDebugView.value_or_default() != 0 || cfg.DlssNrCompare.value_or_default() != 0 ||
                cfg.DlssNrShowSkinMask.value_or_default() ||
                (!cfg.DlssNrApplyModel.value_or_default() && !cfg.DlssNrFinishedPicture.value_or_default()))
            {
                Say("inactive: disable proxy backend, frame hold/debug/compare, and enable Apply model");
                return;
            }
            if (cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
                (!privateJob &&
                 (cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default()) &&
                 !D3D12Hooks::CanRestoreRootSignature(cmd)))
            {
                Say("inactive: requires a direct command list with restorable game state");
                return;
            }
            auto* color = owner.GetResource(source, NVSDK_NGX_Parameter_Color, "DLSSD.Color");
            auto* output = owner.GetResource(source, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
            auto* depth = owner.GetResource(source, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
            auto* motion = owner.GetResource(source, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");
            const bool wantsHalf =
                cfg.DlssNrResidualFg.value_or_default() && !privateJob && !cfg.DlssNrFinishedPicture.value_or_default();
            const bool sampleAndHold = wantsHalf && motion == nullptr;
            if (!color || !output || !depth || (!motion && !sampleAndHold) || color == output)
            {
                Say("inactive: distinct Color/Output, depth and motion are required");
                return;
            }
            for (const char* key :
                 { NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                   NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
                   NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                   NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                   NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
                   NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y })
                if (UInt(source, key) != 0)
                {
                    Say("inactive: non-zero colour/guide/output offsets");
                    return;
                }
            const auto inDesc = color->GetDesc(), outDesc = output->GetDesc();
            const auto active =
                DlssNr::PreSrColorExtent(inDesc, UInt(source, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width),
                                         UInt(source, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height));
            if (!active || !DlssNr::PreSrColorExtent(outDesc, 0, 0) || inDesc.MipLevels != 1 ||
                outDesc.MipLevels != 1 || active->width > outDesc.Width || active->height > outDesc.Height)
            {
                Say("inactive: unsupported active input/output dimensions");
                return;
            }
            ID3D12Device* device = nullptr;
            if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))))
                return;
            auto* ownerQueue = queue ? queue : (ID3D12CommandQueue*) ::State::Instance().currentCommandQueue;
            ID3D12Device* queueDevice = nullptr;
            if (!ownerQueue || ownerQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
                FAILED(ownerQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice != device)
            {
                if (queueDevice)
                    queueDevice->Release();
                device->Release();
                Say("waiting for a same-device direct queue identity");
                return;
            }
            queueDevice->Release();
            unsigned flags = (owner.featureFlags ? owner.featureFlags
                                                 : UInt(source, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags)) &
                             (NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_MVJittered);
            if (sampleAndHold)
                flags = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
            if (current && (current->device != device || current->queue != ownerQueue || current->w != active->width ||
                            current->h != active->height || current->outW != outDesc.Width ||
                            current->outH != outDesc.Height || current->inputFormat != inDesc.Format ||
                            current->outputFormat != outDesc.Format || current->flags != flags ||
                            current->halfRequested != wantsHalf || current->sampleAndHold != sampleAndHold ||
                            current->approximateCamera != cfg.DlssNrResidualFgApproxCamera.value_or_default()))
                retired.push_back(std::move(current));
            if (!current)
            {
                if (retired.size() >= 4)
                {
                    device->Release();
                    Say("waiting for retired GPU work; clean SR frame retained");
                    return;
                }
                current = std::make_unique<Generation>();
                current->device = device; // take the GetDevice reference
                current->queue = ownerQueue;
                ownerQueue->AddRef();
                current->w = active->width;
                current->h = active->height;
                current->outW = (unsigned) outDesc.Width;
                current->outH = outDesc.Height;
                current->inputFormat = inDesc.Format;
                current->outputFormat = outDesc.Format;
                current->flags = flags;
                current->halfRequested = wantsHalf;
                current->sampleAndHold = sampleAndHold;
                current->approximateCamera = cfg.DlssNrResidualFgApproxCamera.value_or_default();
                if (!Allocate(*current))
                {
                    current->failed = true;
                    Say("allocation failed; clean SR frame retained");
                    return;
                }
            }
            else
                device->Release();
            auto& g = *current;
            if (g.failed)
                return;
            // Native seams have a logical per-evaluate identity. Bridges retain the submitted epoch
            // so a second upscale in the same bridge submission is still rejected.
            if (g.began && g.lastBeginEpoch == epoch)
            {
                g.reset = true;
                Say("inactive: more than one upscale in a submission epoch");
                return;
            }
            g.began = true;
            g.lastBeginEpoch = epoch;
            Use use(g, cmd);
            if (!use.valid)
            {
                Say("waiting for GPU completion slots; clean SR frame retained");
                return;
            }
            if (!g.feature)
            {
                ScopedNrStateEnvelope envelope(cmd);
                if (!NVNGXProxy::InitDx12(g.device) || !NVNGXProxy::D3D12_AllocateParameters() ||
                    !NVNGXProxy::D3D12_DestroyParameters() || !NVNGXProxy::D3D12_CreateFeature() ||
                    !NVNGXProxy::D3D12_EvaluateFeature() || !NVNGXProxy::D3D12_ReleaseFeature() ||
                    NVNGXProxy::D3D12_AllocateParameters()(&g.parameters) != NVSDK_NGX_Result_Success || !g.parameters)
                {
                    g.failed = true;
                    Say("NVIDIA DLSS SR runtime unavailable; no alternative upscaler used");
                    return;
                }
                auto* p = g.parameters;
                p->Set(NVSDK_NGX_Parameter_Width, g.w);
                p->Set(NVSDK_NGX_Parameter_Height, g.h);
                p->Set(NVSDK_NGX_Parameter_OutWidth, g.outW);
                p->Set(NVSDK_NGX_Parameter_OutHeight, g.outH);
                p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
                p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
                p->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                       (int) UInt(source, NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_PerfQuality_Value_MaxPerf));
                // LDR biased carrier, constant unit exposure, no auto-exposure/sharpening. No main-game presets
                // or feature handle are overwritten. NGX is called directly, bypassing OptiScaler's NR hooks.
                p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, g.flags);
                const auto result =
                    NVNGXProxy::D3D12_CreateFeature()(cmd, NVSDK_NGX_Feature_SuperSampling, p, &g.feature);
                if (result != NVSDK_NGX_Result_Success || !g.feature)
                {
                    g.failed = true;
                    Say("private DLSS creation failed: " + std::to_string((unsigned) result));
                    return;
                }
                DlssNrConstants unit {};
                unit.Mode = DlssNrMode_UnitExposure;
                unit.Width = unit.Height = 1;
                if (!g.codec->DispatchPass(cmd, unit, g.edited, nullptr, nullptr, nullptr, nullptr, g.exposure,
                                           nullptr))
                {
                    g.failed = true;
                    Say("private exposure initialization failed");
                    return;
                }
                owner.Barrier(cmd, g.exposure, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (g.sampleAndHold)
                {
                    g.zeroMotion = owner.CreateScratch(g.device, DXGI_FORMAT_R16G16_FLOAT, g.w, g.h);
                    unit.Mode = DlssNrMode_ZeroMotion;
                    unit.Width = g.w;
                    unit.Height = g.h;
                    if (!g.zeroMotion || !g.codec->DispatchPass(cmd, unit, g.edited, nullptr, nullptr, nullptr, nullptr,
                                                                g.zeroMotion, nullptr))
                    {
                        g.failed = true;
                        Say("sample-and-hold guide initialization failed");
                        return;
                    }
                    owner.Barrier(cmd, g.zeroMotion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
                g.createEpoch = submittedEpoch;
                Say("private DLSS created; waiting for a later submission epoch");
                return;
            }
            // Synthetic seam ticks cannot prove that a feature's creation commands were submitted.
            if (submittedEpoch == g.createEpoch)
            {
                LOG_DEBUG("DLSS-NR deferred: waiting after feature creation at submitted epoch {}", submittedEpoch);
                return;
            }

            if (g.sampleAndHold)
            {
                if (g.reset || UInt(source, NVSDK_NGX_Parameter_Reset))
                    g.hold.Reset();
                if (g.hold.CanReuse(epoch))
                {
                    pending = { cmd,
                                source,
                                output,
                                epoch,
                                std::max(Float(source, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1), 1e-4f),
                                true,
                                false };
                    return; // Apply the held residual to CURRENT clean SR, not a delayed raster.
                }
                g.hold.Reset();
                motion = g.zeroMotion; // private NR/SR only, with temporal history reset below.
            }

            bool half = false;
            if (!g.sampleAndHold)
            {
                ScopedNrStateEnvelope envelope(cmd);
                half = PrepareHalfRate(g, cmd, source, motion, epoch, submittedEpoch);
            }
            if (!half && g.half)
                g.half->Reset();
            if (half && g.half->havePrevious && g.half->previousWasAnchor)
            {
                pending = { cmd,
                            source,
                            output,
                            epoch,
                            std::max(Float(source, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1), 1e-4f),
                            true,
                            true };
                return; // NR and private residual SR are both skipped; game SR still runs normally.
            }
            auto* nrMotion = half ? (g.half->havePrevious ? g.half->anchorMotion : g.half->motion) : motion;

            const auto arrival = cfg.ColorResourceBarrier.has_value()
                                     ? (D3D12_RESOURCE_STATES) cfg.ColorResourceBarrier.value()
                                     : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            owner.Barrier(cmd, color, arrival, D3D12_RESOURCE_STATE_COPY_SOURCE);
            owner.Barrier(cmd, g.edited, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            CopyActiveColor(cmd, g.edited, color, *active);
            owner.Barrier(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, arrival);
            owner.Barrier(cmd, g.edited, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            DlssNrFrameInfo frame {};
            frame.BeforeUpscale = frame.PrivateColorCopy = true;
            frame.IndependentCommands = privateJob;
            frame.SubmissionEpoch = submittedEpoch;
            frame.RenderSubrectWidth = g.w;
            frame.RenderSubrectHeight = g.h;
            frame.DepthInverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
            frame.ColourIsLinearHdr =
                ((owner.featureFlags ? owner.featureFlags
                                     : UInt(source, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags)) &
                 NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0 &&
                owner.FormatCanHoldLinearHdr(outDesc.Format);
            frame.Reset = UInt(source, NVSDK_NGX_Parameter_Reset) != 0 || g.reset || g.sampleAndHold || privateJob;
            frame.MvScaleX = Float(source, NVSDK_NGX_Parameter_MV_Scale_X, 1);
            frame.MvScaleY = Float(source, NVSDK_NGX_Parameter_MV_Scale_Y, 1);
            if (half)
            {
                frame.MvScaleX = (float) g.w;
                frame.MvScaleY = (float) g.h;
            }
            if (g.sampleAndHold)
                frame.MvScaleX = frame.MvScaleY = 1.0f;
            frame.PreExposure = std::max(Float(source, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1), 1e-4f);
            frame.ExposureTexture = owner.GetResource(source, NVSDK_NGX_Parameter_ExposureTexture, "ExposureTexture");
            owner.nr.exposureOfferedNow = frame.ExposureTexture != nullptr;
            owner.nr.exposureEverOffered = owner.nr.exposureEverOffered || owner.nr.exposureOfferedNow;
            ++owner.nr.exposureFrames;
            const auto before = owner.nr.successfulDispatches;
            owner.Run(cmd, g.edited, depth, nrMotion, g.edited, frame, queue);
            const bool evaluated = owner.nr.successfulDispatches != before;
            if (evaluated)
            {
                ScopedNrStateEnvelope envelope(cmd);
                if (g.smallReadable)
                    owner.Barrier(cmd, g.residualInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                owner.Barrier(cmd, color, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                DlssNrConstants encode {};
                encode.Mode = DlssNrMode_EncodeResidual;
                encode.Width = g.w;
                encode.Height = g.h;
                encode.ExposurePreMul = frame.PreExposure;
                bool ok;
                if (cfg.DlssNrFinishedPicture.value_or_default())
                {
                    encode.Mode = 5; // finished-colour shader: encode relative changes before FP16 storage
                    encode.WhitePoint = frame.PreExposure;
                    encode.TransferStrength = frame.ColourIsLinearHdr ? 1.0f : 0.0f;
                    encode.MaxRatio = std::clamp(cfg.DlssNrMaxRatio.value_or_default(), 1.0f, 8.0f);
                    ok = g.codec->DispatchResidualPass(cmd, encode, color, g.edited, nullptr, nullptr, g.residualInput,
                                                       true);
                }
                else
                    ok = g.codec->DispatchPass(cmd, encode, color, g.edited, nullptr, nullptr, nullptr, g.residualInput,
                                               nullptr);
                owner.Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
                owner.Barrier(cmd, g.residualInput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                g.smallReadable = true;
                if (ok)
                {
                    auto* p = g.parameters;
                    p->Set(NVSDK_NGX_Parameter_Color, g.residualInput);
                    p->Set(NVSDK_NGX_Parameter_Output, g.residualOutput);
                    p->Set(NVSDK_NGX_Parameter_Depth, depth);
                    p->Set(NVSDK_NGX_Parameter_MotionVectors, nrMotion);
                    p->Set(NVSDK_NGX_Parameter_ExposureTexture, g.exposure);
                    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, g.w);
                    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, g.h);
                    p->Set(NVSDK_NGX_Parameter_Reset, (unsigned) (frame.Reset || g.reset));
                    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, Float(source, NVSDK_NGX_Parameter_Jitter_Offset_X, 0));
                    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, Float(source, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0));
                    p->Set(NVSDK_NGX_Parameter_MV_Scale_X, frame.MvScaleX);
                    p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, frame.MvScaleY);
                    p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,
                           Float(source, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 16.67f) *
                               (half && g.half->havePrevious ? 2.0f : 1.0f));
                    p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
                    p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
                    p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
                    pending = { cmd, source, output, epoch, frame.PreExposure, false, half };
                    LOG_DEBUG("DLSS-NR deferred: Before armed epoch {} half {} preExp {:.4f} reset-carried {}", epoch,
                              half, frame.PreExposure, frame.Reset);
                }
            }
            else
            {
                g.reset = true;
                Say("waiting for NR evaluation; clean SR frame retained");
            }
            owner.Barrier(cmd, g.edited, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        // Background GPU job: produce only the DLSS-upscaled residual. No raster composition,
        // regular FG call, or presentation operation is recorded on this queue.
        bool ResolvePrivate(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* source,
                            [[maybe_unused]] unsigned long long epoch, ID3D12Resource* destination)
        {
            const auto pair = pending;
            pending = {};
            // No epoch match, as in After() -- see the comment there. (Dead path on this branch: async NR
            // was removed in v0.7.1 and nothing calls ResolvePrivate; kept consistent for a future revival.)
            if (!current || current->failed || pair.cmd != cmd || pair.caller != source)
                return false;
            auto& g = *current;
            Use use(g, cmd);
            if (!use.valid)
            {
                g.reset = true;
                return false;
            }
            const auto result = NVNGXProxy::D3D12_EvaluateFeature()(cmd, g.feature, g.parameters, nullptr);
            if (result != NVSDK_NGX_Result_Success)
            {
                g.failed = true;
                Say("asynchronous residual DLSS evaluation failed");
                return false;
            }
            owner.Barrier(cmd, g.residualOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_SOURCE);
            owner.Barrier(cmd, destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(destination, g.residualOutput);
            owner.Barrier(cmd, destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            owner.Barrier(cmd, g.residualOutput, D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.reset = false;
            return true;
        }

        void After(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* source, unsigned long long epoch)
        {
            const auto pair = pending;
            pending = {}; // Consume once, only for the immediately matching successful upscale.
            // Match and consume the immediately preceding Before by resource identity, not Present timing.
            // The pending epoch also owns history continuity if Present changed while DLSS was recording.
            if (!current || current->failed || pair.cmd != cmd || pair.caller != source ||
                pair.output != owner.GetResource(source, NVSDK_NGX_Parameter_Output, "DLSSD.Output"))
            {
                LOG_DEBUG("DLSS-NR deferred: After no-op -> reset. current={} failed={} cmdMatch={} callerMatch={} "
                          "epoch(pending/now)={}/{} outputMatch={}",
                          current != nullptr, current && current->failed, pair.cmd == cmd, pair.caller == source,
                          pair.epoch, epoch,
                          pair.output == owner.GetResource(source, NVSDK_NGX_Parameter_Output, "DLSSD.Output"));
                if (current)
                    current->reset = true;
                return;
            }
            auto& g = *current;
            const auto& cfg = *Config::Instance();
            if ((cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default()) &&
                !D3D12Hooks::CanRestoreRootSignature(cmd))
            {
                g.reset = true;
                Say("inactive: game state cannot be restored after SR");
                return;
            }
            Use use(g, cmd);
            if (!use.valid)
            {
                g.reset = true;
                Say("waiting for GPU completion slots; clean SR frame retained");
                return;
            }
            ScopedNrStateEnvelope envelope(cmd);
            if (!pair.skipNr)
            {
                const auto result = NVNGXProxy::D3D12_EvaluateFeature()(cmd, g.feature, g.parameters, nullptr);
                if (result != NVSDK_NGX_Result_Success)
                {
                    g.failed = true;
                    if (g.half)
                        g.half->Reset();
                    Say("private DLSS evaluation failed: " + std::to_string((unsigned) result));
                    return;
                }
            }
            if (g.reset)
                LOG_DEBUG("DLSS-NR deferred: After fed Reset=1 to the private DLSS SR this frame (history restart). "
                          "epoch {} skipNr {} half {}",
                          epoch, pair.skipNr, pair.half);
            else
                LOG_TRACE("DLSS-NR deferred: After applied epoch {} skipNr {} half {}", epoch, pair.skipNr, pair.half);
            g.reset = false;
            if (cfg.DlssNrFinishedPicture.value_or_default())
            {
                const bool sceneLinear =
                    ((owner.featureFlags ? owner.featureFlags
                                         : UInt(source, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags)) &
                     NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0 &&
                    owner.FormatCanHoldLinearHdr(g.outputFormat);
                if (owner.late.CaptureResidual(cmd, pair.output, g.residualOutput, pair.scale, sceneLinear))
                    Say("running: model before SR; changes saved for the finished picture");
                else
                {
                    g.reset = true;
                    Say("waiting to save the upscaled changes for the finished picture");
                }
                return; // Keep the game's SR output clean: no early composition and no second NR evaluation.
            }

            owner.Barrier(cmd, g.residualOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            bool half = pair.half && g.half && !g.half->failed;
            if (half && !pair.skipNr)
            {
                auto& h = *g.half;
                owner.Barrier(cmd, h.suppression, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyBufferRegion(h.suppression, 0, h.zeroUpload, 0, 256);
                owner.Barrier(cmd, h.suppression, D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (h.interpolationReadable)
                    owner.Barrier(cmd, h.interpolated, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                const auto result = h.fg->Evaluate(
                    cmd, g.residualOutput, owner.GetResource(source, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth"),
                    h.havePrevious ? h.anchorMotion : h.motion, h.interpolated, h.suppression, h.camera,
                    (g.flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0, !h.havePrevious, h.anchorId++);
                owner.Barrier(cmd, h.interpolated, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                h.interpolationReadable = true;
                if (result != NVSDK_NGX_Result_Success)
                {
                    h.failed = true;
                    h.Reset();
                    half = false;
                    g.halfStatus = "FG evaluate failed: " + std::to_string((unsigned) result);
                }
                else
                {
                    // NVIDIA writes a boolean to the first buffer byte. Copy it to R8_UNORM
                    // for the shader: avoids CPU waiting and changing the game's predication.
                    owner.Barrier(cmd, h.suppression, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
                    owner.Barrier(cmd, h.suppressionTexture,
                                  h.suppressionReadable ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                        : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
                    D3D12_TEXTURE_COPY_LOCATION from {}, to {};
                    from.pResource = h.suppression;
                    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    from.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UNORM, 1, 1, 1, 256 };
                    to.pResource = h.suppressionTexture;
                    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                    owner.Barrier(cmd, h.suppressionTexture, D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    owner.Barrier(cmd, h.suppression, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    h.suppressionReadable = true;
                }
            }
            const auto arrival = cfg.OutputResourceBarrier.has_value()
                                     ? (D3D12_RESOURCE_STATES) cfg.OutputResourceBarrier.value()
                                     : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            owner.Barrier(cmd, pair.output, arrival, D3D12_RESOURCE_STATE_COPY_SOURCE);
            owner.Barrier(cmd, g.clean, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(g.clean, pair.output);
            owner.Barrier(cmd, g.clean, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ID3D12Resource* base = g.clean;
            ID3D12Resource* residual = g.residualOutput;
            ID3D12Resource* suppression = nullptr;
            DlssNrConstants apply {};
            apply.Mode = DlssNrMode_ApplyResidual;
            apply.Width = g.outW;
            apply.Height = g.outH;
            apply.ExposurePreMul = pair.scale;
            if (half)
            {
                auto& h = *g.half;
                if (h.havePrevious)
                {
                    const unsigned previous = 1 - h.writeIndex;
                    base = h.history[previous];
                    apply.ExposurePreMul = h.historyScale[previous];
                    if (!pair.skipNr)
                    {
                        residual = h.interpolated;
                        suppression = h.suppressionTexture;
                        apply.Mode = DlssNrMode_ApplyInterpolatedResidual;
                    }
                }
                owner.Barrier(cmd, g.clean, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_COPY_SOURCE);
                owner.Barrier(cmd, h.history[h.writeIndex],
                              h.historyReadable[h.writeIndex] ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                              : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyResource(h.history[h.writeIndex], g.clean);
                owner.Barrier(cmd, h.history[h.writeIndex], D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                owner.Barrier(cmd, g.clean, D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                h.historyReadable[h.writeIndex] = true;
                h.historyScale[h.writeIndex] = pair.scale;
            }
            const bool ok =
                g.codec->DispatchPass(cmd, apply, base, residual, nullptr, nullptr, suppression, g.composed, nullptr);
            if (ok)
            {
                owner.Barrier(cmd, g.composed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                owner.Barrier(cmd, pair.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyResource(pair.output, g.composed);
                owner.Barrier(cmd, pair.output, D3D12_RESOURCE_STATE_COPY_DEST, arrival);
                owner.Barrier(cmd, g.composed, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Say(g.sampleAndHold ? "running: sample-and-hold (motion unavailable); each NR residual applied to 2 "
                                      "current frames; no residual FG or SR delay"
                    : half ? "running: NR every second frame + NVIDIA residual FG; SR delayed 1 frame; APPROXIMATE "
                             "camera guides"
                           : "running: " + std::to_string(g.w) + "x" + std::to_string(g.h) +
                                 " contribution -> private DLSS -> " + std::to_string(g.outW) + "x" +
                                 std::to_string(g.outH) + "; applied after SR" +
                                 (g.halfRequested ? "; residual FG inactive: " + g.halfStatus : ""));
            }
            else
            {
                owner.Barrier(cmd, pair.output, D3D12_RESOURCE_STATE_COPY_SOURCE, arrival);
                Say("composition failed; clean frame retained");
            }
            owner.Barrier(cmd, g.clean, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            owner.Barrier(cmd, g.residualOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (g.sampleAndHold)
            {
                if (ok && !pair.skipNr)
                    g.hold.SampleSucceeded(pair.epoch);
                else
                    g.hold.Reset();
            }
            if (half)
            {
                auto& h = *g.half;
                owner.Barrier(cmd, h.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_COPY_SOURCE);
                owner.Barrier(cmd, h.previousMotion,
                              h.previousReadable ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                 : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyResource(h.previousMotion, h.motion);
                owner.Barrier(cmd, h.previousMotion, D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                owner.Barrier(cmd, h.motion, D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                h.previousReadable = true;
                h.havePrevious = ok;
                h.previousWasAnchor = !pair.skipNr;
                if (pair.skipNr)
                    ++h.skippedNr;
                else
                    ++h.nrAnchors;
                if (h.skippedNr == 8 && pair.skipNr)
                    LOG_INFO(
                        "Residual FG cadence: {} NR anchor frames, {} skipped NR frames; matching clean history active",
                        h.nrAnchors, h.skippedNr);
                h.writeIndex = 1 - h.writeIndex;
                if (!ok)
                {
                    h.Reset();
                    g.reset = true;
                }
            }
        }

        void ReleaseResources()
        {
            Cancel();
            if (current)
                retired.push_back(std::move(current));
            Collect();
            // Never free feature histories, descriptors or surfaces referenced by an unsubmitted/in-flight
            // list. At shutdown only, retain uncompleted generations for process teardown rather than UAF.
            for (auto& g : retired)
                (void) g.release();
            retired.clear();
        }
    };
    DeferredSrContext deferredSr { *this };

    std::string SynchronousDeferredDlssStatus()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        auto result = deferredSr.status;
        if (deferredSr.current && deferredSr.current->half && deferredSr.current->half->havePrevious)
        {
            const auto& h = *deferredSr.current->half;
            result += " (NR frames " + std::to_string(h.nrAnchors) + ", skipped " + std::to_string(h.skippedNr) + ")";
        }
        return result;
    }

    struct LateContext
    {
        State& owner;
        explicit LateContext(State& state) : owner(state) {}

        template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;
        struct Slot
        {
            ComPtr<ID3D12Resource> depth, motion, linear, encoded, residual;
            ComPtr<ID3D12Fence> fence;
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> commands;
            ID3D12CommandList* producer = nullptr; // identity only; never dereferenced
            DlssNrFrameInfo frame {};
            uint64_t ready = 0, done = 0, serial = 0;
            bool pending = false, submitted = false, residualOnly = false, sceneLinear = true;
        };
        std::array<Slot, 4> slots;
        ComPtr<ID3D12Device> device;
        uint64_t serial = 0, successes = 0;
        std::string status = "Waiting for a finished picture.";
        bool reset = true;
        std::atomic<bool> tracking { false };
        static constexpr GUID colorSpaceKey = {
            0x34a31e7b, 0x84c5, 0x44ef, { 0xa7, 0x4d, 0x6b, 0xd3, 0x60, 0x8c, 0xe5, 0x22 }
        };

        void Say(const char* message)
        {
            if (status != message)
            {
                status = message;
                LOG_INFO("DLSS-NR finished picture: {}", message);
            }
        }

        bool Finished(const Slot& slot)
        {
            return !slot.fence ||
                   (slot.fence->GetCompletedValue() != UINT64_MAX && slot.fence->GetCompletedValue() >= slot.done);
        }

        void Cancel()
        {
            // Submitted copies may still be in flight; their fences still protect reuse.
            for (auto& slot : slots)
                if (slot.submitted)
                    slot.pending = false;
            reset = true;
        }

        bool Clone(ComPtr<ID3D12Resource>& copy, ID3D12Resource* source)
        {
            const auto want = source->GetDesc();
            if (want.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || want.SampleDesc.Count != 1 ||
                want.DepthOrArraySize != 1 || want.MipLevels != 1)
                return false;
            if (copy)
            {
                const auto have = copy->GetDesc();
                if (have.Width != want.Width || have.Height != want.Height ||
                    have.Format != owner.TypedGuideFormat(want.Format))
                    copy.Reset(); // caller checked the previous GPU fence
            }
            if (!copy)
                copy.Attach(owner.CreateGuideClone(device.Get(), source));
            return copy != nullptr;
        }

        Slot* Acquire(ID3D12GraphicsCommandList* cmd)
        {
            if (!cmd || ::State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
            {
                Say("This option needs a native DirectX 12 game.");
                return nullptr;
            }
            ComPtr<ID3D12Device> currentDevice;
            if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&currentDevice))))
                return nullptr;
            if (device && device != currentDevice)
            {
                Say("The graphics device changed. Restart the game to use this option.");
                return nullptr;
            }
            device = currentDevice;
            tracking.store(true);
            // Only the lightweight submission hook is needed, including when FG is disabled.
            ResTrack_Dx12::HookLateNrQueue(device.Get());
            Slot* next = nullptr;
            for (auto& slot : slots)
                if (!slot.pending && Finished(slot))
                {
                    next = &slot;
                    break;
                }
            if (!next)
            {
                Say("Waiting for the previous picture to finish.");
                return nullptr;
            }
            auto& slot = *next;
            if (!slot.commands)
            {
                if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence))) ||
                    FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&slot.allocator))) ||
                    FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                                     IID_PPV_ARGS(&slot.commands))) ||
                    FAILED(slot.commands->Close()))
                {
                    slot.commands.Reset();
                    slot.allocator.Reset();
                    slot.fence.Reset();
                    Say("Could not prepare the finished-picture option.");
                    return nullptr;
                }
            }
            return next;
        }

        void Arm(Slot& slot, ID3D12GraphicsCommandList* cmd)
        {
            slot.producer = cmd;
            ID3D12GraphicsCommandList* real = nullptr;
            if (Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**) &real))
                slot.producer = real;
            slot.serial = ++serial;
            slot.ready = slot.done + 1;
            slot.done = slot.ready;
            slot.submitted = false;
            slot.pending = true;
        }

        void Capture(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params, bool rr)
        {
            if (!params)
                return;
            auto* depth = owner.GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
            auto* motion = owner.GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");
            auto* output = owner.GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
            if (!depth || !motion || !output)
            {
                Cancel();
                Say("Waiting for the game's depth and movement data.");
                return;
            }
            auto* next = Acquire(cmd);
            if (!next)
                return;
            auto& slot = *next;
            if (!Clone(slot.depth, depth) || !Clone(slot.motion, motion))
            {
                Say("The game's depth or movement data is not supported.");
                return;
            }
            slot.residualOnly = false;
            // Copy at the NGX seam, where guide states and lifetimes are defined. Keep typed,
            // shader-readable copies until both the producing queue and NR have finished.
            for (auto pair : { std::pair { depth, slot.depth.Get() }, std::pair { motion, slot.motion.Get() } })
            {
                owner.Barrier(cmd, pair.first, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmd->CopyResource(pair.second, pair.first);
                owner.Barrier(cmd, pair.first, D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                // The copy returns to COPY_DEST after the late dispatch.
            }
            slot.frame = {};
            auto& frame = slot.frame;
            unsigned flags = 0, gameReset = 0;
            params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);
            params->Get(NVSDK_NGX_Parameter_Reset, &gameReset);
            frame.Reset = gameReset != 0;
            frame.DepthInverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
            frame.MotionVectorsLowResolution = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
            frame.RayReconstruction = rr;
            frame.OutputWidth = (unsigned) output->GetDesc().Width;
            frame.OutputHeight = output->GetDesc().Height;
            params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth);
            params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight);
            params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &frame.DepthSubrectBaseX);
            params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &frame.DepthSubrectBaseY);
            params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &frame.MotionSubrectBaseX);
            params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &frame.MotionSubrectBaseY);
            params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX);
            params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY);
            frame.ColourIsLinearHdr = false;
            frame.IndependentCommands = true;
            frame.FinishedPicture = true;
            frame.OutputArrivalState = D3D12_RESOURCE_STATE_PRESENT;
            frame.SubmissionEpoch = ::State::Instance().frameCount;
            Arm(slot, cmd);
        }

        bool CaptureResidual(ID3D12GraphicsCommandList* cmd, ID3D12Resource* clean, ID3D12Resource* residual,
                             float scale, bool sceneLinear)
        {
            auto* next = Acquire(cmd);
            if (!next)
                return false;
            auto& slot = *next;
            if (!Clone(slot.residual, residual))
            {
                Say("The upscaled changes could not be saved.");
                return false;
            }
            owner.Barrier(cmd, residual, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyResource(slot.residual.Get(), residual);
            owner.Barrier(cmd, residual, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            slot.frame = {};
            slot.frame.OutputWidth = (unsigned) clean->GetDesc().Width;
            slot.frame.OutputHeight = clean->GetDesc().Height;
            slot.frame.PreExposure = scale;
            slot.frame.SubmissionEpoch = ::State::Instance().frameCount;
            slot.residualOnly = true;
            slot.sceneLinear = sceneLinear;
            Arm(slot, cmd);
            return true;
        }
    };
    LateContext late { *this };

    void FinishedPictureResetCommandList(ID3D12CommandList* cmd)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (gpuTime)
            gpuTime->ResetRecording(cmd);
        if (ngxTime)
            ngxTime->ResetRecording(cmd);
        if (!late.tracking.load())
            return;
        for (auto& slot : late.slots)
            if (slot.pending && !slot.submitted && slot.producer == cmd)
            {
                slot.pending = false;
                slot.done = slot.ready - 1; // discarded recording: no GPU signal was promised
                late.reset = true;
            }
    }

    bool WaitForFinishedPicture()
    {
        if (!late.tracking.load())
            return true;
        std::lock_guard<std::recursive_mutex> lock(mutex);
        late.Cancel();
        for (auto& slot : late.slots)
        {
            if (!slot.submitted || late.Finished(slot))
                continue;
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event)
                return false;
            const auto hr = slot.fence->SetEventOnCompletion(slot.done, event);
            const bool finished = SUCCEEDED(hr) && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
            CloseHandle(event);
            if (!finished)
                return false;
        }
        return true;
    }

    std::string FinishedPictureStatus()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        return late.status;
    }

    void FinishedPictureSubmitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (gpuTime)
            gpuTime->Submitted(queue, count, lists);
        if (ngxTime)
            ngxTime->Submitted(queue, count, lists);
        if (!late.tracking.load())
            return;
        for (auto& slot : late.slots)
            if (slot.pending && !slot.submitted)
                for (UINT i = 0; i < count; ++i)
                    if (lists[i] == slot.producer)
                    {
                        // Signal after ExecuteCommandLists, never when merely recording the copy.
                        slot.submitted = true;
                        if (FAILED(queue->Signal(slot.fence.Get(), slot.ready)))
                        {
                            // The copy already executed. Keep its unsignalled fence protecting
                            // the slot instead of treating this as a discarded recording.
                            slot.pending = false;
                            late.Say("The graphics queue stopped. Restart the game to retry.");
                        }
                        break;
                    }
    }

    void FinishedPictureColorSpace(IDXGISwapChain* swapchain, DXGI_COLOR_SPACE_TYPE colorSpace)
    {
        swapchain->SetPrivateData(late.colorSpaceKey, sizeof(colorSpace), &colorSpace);
    }

    void ApplyToFinishedPicture(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (!Config::Instance()->DlssNrFinishedPicture.value_or_default() ||
            !Config::Instance()->DlssNrEnabled.value_or_default())
        {
            late.Cancel();
            return;
        }
        if (!swapchain || !queue || ::State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
            return;
        LateContext::ComPtr<IDXGISwapChain3> sc;
        LateContext::ComPtr<ID3D12Resource> color;
        LateContext::ComPtr<ID3D12Device> currentDevice;
        if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc))) ||
            FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&color))) ||
            FAILED(queue->GetDevice(IID_PPV_ARGS(&currentDevice))) || currentDevice != late.device)
            return;
        const auto desc = color->GetDesc();
        auto colorSpace = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                                        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        UINT colorSpaceSize = sizeof(colorSpace);
        swapchain->GetPrivateData(late.colorSpaceKey, &colorSpaceSize, &colorSpace);
        const bool pq = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        const bool scrgb = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        const bool sdr = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        if ((!sdr && !pq && !scrgb) || (scrgb && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) ||
            (!scrgb && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM))
        {
            late.Cancel();
            late.Say("This screen colour format is not supported.");
            return;
        }
        LateContext::Slot* latest = nullptr;
        const auto epoch = ::State::Instance().frameCount;
        const bool residualOnly = Config::Instance()->DlssNrRunBeforeSr.value_or_default() ||
                                  Config::Instance()->DlssNrDeferredDlss.value_or_default();
        for (auto& slot : late.slots)
        {
            if (!slot.pending || !slot.submitted)
                continue;
            if (epoch < slot.frame.SubmissionEpoch || epoch - slot.frame.SubmissionEpoch > 1)
            {
                slot.pending = false;
                late.reset = true;
                continue;
            }
            if (slot.residualOnly == residualOnly && slot.frame.OutputWidth == desc.Width &&
                slot.frame.OutputHeight == desc.Height && (!latest || slot.serial > latest->serial))
                latest = &slot;
        }
        if (!latest)
            return; // loading screen, another swapchain, or this real frame was already consumed
        auto& slot = *latest;
        // Drop other submitted evaluates from this picture, not their in-flight resources.
        for (auto& other : late.slots)
            if (other.submitted && other.serial <= slot.serial)
                other.pending = false;
        if (FAILED(queue->Wait(slot.fence.Get(), slot.ready)) || FAILED(slot.allocator->Reset()) ||
            FAILED(slot.commands->Reset(slot.allocator.Get(), nullptr)))
        {
            late.reset = true;
            late.Say("Could not prepare the finished picture.");
            return;
        }
        auto* cmd = slot.commands.Get();
        const auto before = nr.successfulDispatches;
        bool appliedResidual = false;
        if (slot.residualOnly)
        {
            if (slot.encoded &&
                (slot.encoded->GetDesc().Width != desc.Width || slot.encoded->GetDesc().Height != desc.Height ||
                 slot.encoded->GetDesc().Format != desc.Format))
                slot.encoded.Reset();
            if (!slot.encoded)
                slot.encoded.Attach(CreateScratch(late.device.Get(), desc.Format, (unsigned) desc.Width, desc.Height));
            if (slot.encoded && Config::Instance()->DlssNrApplyModel.value_or_default())
            {
                Barrier(cmd, slot.residual.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                DlssNrConstants apply {};
                apply.Mode = pq ? 4 : scrgb ? 3 : 2;
                apply.Width = (unsigned) desc.Width;
                apply.Height = desc.Height;
                apply.WhitePoint = slot.frame.PreExposure;
                apply.TransferStrength = slot.sceneLinear ? 1.0f : 0.0f;
                apply.MaxRatio = std::clamp(Config::Instance()->DlssNrMaxRatio.value_or_default(), 1.0f, 8.0f);
                appliedResidual = shader.DispatchResidualPass(cmd, apply, color.Get(), nullptr, slot.residual.Get(),
                                                              nullptr, slot.encoded.Get(), true);
                if (appliedResidual)
                {
                    Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_DEST);
                    cmd->CopyResource(color.Get(), slot.encoded.Get());
                    Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
                Barrier(cmd, slot.residual.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
            }
        }
        else
        {
            auto frame = slot.frame;
            frame.ColourIsLinearHdr = pq || scrgb;
            // Absolute display encodings: 203-nit reference white, in 80-nit scRGB units.
            frame.WhitePointOverride = (pq || scrgb) ? 203.0f / 80.0f : 0.0f;
            frame.Reset |= late.reset;
            frame.SubmissionEpoch = epoch;
            Barrier(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, slot.motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ID3D12Resource* nrColor = color.Get();
            bool colorReady = true;
            DlssNrConstants conversion {};
            conversion.Width = (unsigned) desc.Width;
            conversion.Height = desc.Height;
            if (pq)
            {
                auto ensure = [&](LateContext::ComPtr<ID3D12Resource>& resource, DXGI_FORMAT format)
                {
                    if (resource && (resource->GetDesc().Width != desc.Width ||
                                     resource->GetDesc().Height != desc.Height || resource->GetDesc().Format != format))
                        resource.Reset();
                    if (!resource)
                        resource.Attach(CreateScratch(late.device.Get(), format, (unsigned) desc.Width, desc.Height));
                    return resource != nullptr;
                };
                colorReady = ensure(slot.linear, DXGI_FORMAT_R16G16B16A16_FLOAT) && ensure(slot.encoded, desc.Format);
                if (colorReady)
                {
                    Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_PRESENT,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    colorReady = shader.DispatchResidualPass(cmd, conversion, color.Get(), nullptr, nullptr, nullptr,
                                                             slot.linear.Get(), true);
                    Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_PRESENT);
                    // Dispatch reads the converted colour; its transition out of UAV orders the conversion.
                    nrColor = slot.linear.Get();
                    frame.OutputArrivalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                }
            }
            if (colorReady)
                Run(cmd, nrColor, slot.depth.Get(), slot.motion.Get(), nrColor, frame, queue);
            if (pq && colorReady && nr.successfulDispatches > before &&
                Config::Instance()->DlssNrApplyModel.value_or_default())
            {
                conversion.Mode = 1;
                Barrier(cmd, slot.linear.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (shader.DispatchResidualPass(cmd, conversion, slot.linear.Get(), nullptr, color.Get(), nullptr,
                                                slot.encoded.Get(), true))
                {
                    Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_DEST);
                    cmd->CopyResource(color.Get(), slot.encoded.Get());
                    Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
                Barrier(cmd, slot.linear.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Barrier(cmd, slot.motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
            Barrier(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
        }
        if (FAILED(cmd->Close()))
        {
            late.Say("Could not finish the picture. Restart the game to retry.");
            slot.pending = true; // quarantine the slot; do not reuse possibly recorded NR resources
            slot.submitted = false;
            return;
        }
        ID3D12CommandList* lists[] = { cmd };
        queue->ExecuteCommandLists(1, lists);
        slot.done = slot.ready + 1;
        if (FAILED(queue->Signal(slot.fence.Get(), slot.done)))
        {
            late.Say("The graphics queue stopped. Restart the game to retry.");
            return;
        }
        const bool ran = slot.residualOnly ? appliedResidual : nr.successfulDispatches > before;
        late.reset = !ran;
        late.Say(!Config::Instance()->DlssNrApplyModel.value_or_default() ? "NR changes are hidden."
                 : ran ? (slot.residualOnly ? "Applying the pre-SR changes to the finished picture."
                                            : "Applying NR to the finished picture.")
                       : "Preparing NR for the finished picture.");
        if (ran && (++late.successes == 1 || late.successes % 300 == 0))
            LOG_INFO("DLSS-NR finished picture: {} frames, {}x{}, FG {}", late.successes, desc.Width, desc.Height,
                     ::State::Instance().currentFG && ::State::Instance().currentFG->IsActive() &&
                         !::State::Instance().currentFG->IsPaused());
    }

    void Run(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour, ID3D12Resource* depth, ID3D12Resource* motion,
             ID3D12Resource* output, const DlssNrFrameInfo& frame, ID3D12CommandQueue* timingQueue)
    {
        std::lock_guard<std::recursive_mutex> nrLock(mutex);
        const Config& cfg = *Config::Instance();

        if (nr.failed || cmdList == nullptr || colour == nullptr || depth == nullptr || motion == nullptr ||
            output == nullptr)
        {
            ReportSkipOnce(nr.failed ? "it already failed this session" : "a resource was missing");
            return;
        }

        ID3D12Resource* target = output;

        // Feature creation records GPU work too, and may return before the first evaluate.
        // Guard the entire dispatch, not just the colour passes at the bottom. Otherwise
        // creation/resize during an RE Engine loading screen captures NR's bindings as
        // the game's state, or returns with those bindings still active.
        const bool restoreRequired =
            cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default();
        if (restoreRequired && !frame.IndependentCommands && !D3D12Hooks::CanRestoreRootSignature(cmdList))
        {
            ReportSkipOnce("the upscaler could not restore state this frame");
            return;
        }
        ScopedNrStateEnvelope stateEnvelope(cmdList);

        // A completed upscaler output normally arrives as a UAV. The pre-SR colour input instead arrives
        // readable. Track every transition so both paths return the resource exactly as their caller gave
        // it to us; a pre-SR resource without UAV support is written through a scratch-and-copy fallback.
        const D3D12_RESOURCE_STATES outputArrival =
            frame.PipelineManagedStates ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            : frame.FinishedPicture     ? (D3D12_RESOURCE_STATES) frame.OutputArrivalState
            : frame.BeforeUpscale ? (!frame.PrivateColorCopy && Config::Instance()->ColorResourceBarrier.has_value()
                                         ? (D3D12_RESOURCE_STATES) Config::Instance()->ColorResourceBarrier.value()
                                         : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            : Config::Instance()->OutputResourceBarrier.has_value()
                ? (D3D12_RESOURCE_STATES) Config::Instance()->OutputResourceBarrier.value()
                : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES targetState = outputArrival;
        const auto TransitionTarget = [&](D3D12_RESOURCE_STATES to)
        {
            Barrier(cmdList, target, targetState, to);
            targetState = to;
        };

        ID3D12Device* device = nullptr;

        if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        {
            ReportSkipOnce("the output texture belongs to no D3D12 device");
            return;
        }

        const D3D12_RESOURCE_DESC desc = target->GetDesc();
        const auto active =
            frame.BeforeUpscale
                ? DlssNr::PreSrColorExtent(desc, frame.RenderSubrectWidth, frame.RenderSubrectHeight)
                : std::optional<DlssNr::ColorExtent> { DlssNr::ColorExtent { (unsigned int) desc.Width, desc.Height } };
        if (!active)
        {
            ReportSkipOnce("the pre-SR active colour size is invalid");
            device->Release();
            return;
        }
        const auto width = active->width;
        const auto height = active->height;
        const bool cropColor = frame.BeforeUpscale && (width != desc.Width || height != desc.Height);
        const bool targetSupportsUav = cropColor || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;

        // Resource/dispatch failures in this mode must preserve Color for RR.
        const bool residualAcrossRr = frame.ResidualAcrossRr && frame.BeforeUpscale;

        const auto guideDesc = depth->GetDesc();
        const auto motionDesc = motion->GetDesc();
        const auto guides = DlssNr::ResolveGuideRegions(
            { (unsigned int) guideDesc.Width, guideDesc.Height },
            { (unsigned int) motionDesc.Width, motionDesc.Height },
            { frame.RenderSubrectWidth, frame.RenderSubrectHeight }, { frame.OutputWidth, frame.OutputHeight },
            frame.MotionVectorsLowResolution, frame.DepthSubrectBaseX, frame.DepthSubrectBaseY,
            frame.MotionSubrectBaseX, frame.MotionSubrectBaseY);
        if (!guides.depth.valid() || !guides.motion.valid())
        {
            ReportSkipOnce("depth or motion-vector subrect is empty");
            device->Release();
            return;
        }
        const auto guideWidth = guides.depth.width, guideHeight = guides.depth.height;
        const auto motionWidth = guides.motion.width, motionHeight = guides.motion.height;
        const auto depthBaseX = guides.depth.x, depthBaseY = guides.depth.y;
        const auto motionBaseX = guides.motion.x, motionBaseY = guides.motion.y;

        nr.guideWidth = guideWidth;
        nr.guideHeight = guideHeight;
        nr.guideDepthInverted = frame.DepthInverted;

        // The game's own encoding, passed through. Every resource already carries a subrect saying how
        // big it is, so scaling by the resolution ratio on top of that counts it twice -- vectors come
        // out too long and the model warps its history past where the surface went.
        nr.guideMvScaleX = frame.MvScaleX;
        nr.guideMvScaleY = frame.MvScaleY;

        if (frame.Reset)
        {
            nr.reset = true;
            // Drop the accumulated enhancement layer: a cut invalidates the reprojection history.
            nr.residualHistoryPrimed = false;
            nr.residualStoreValid = false;

            ++resets;

            if (resets <= 3 || resets % 100 == 0)
                LOG_INFO("DLSS-NR: the game asked for a history reset ({} so far)", resets);
        }

        // Logged whenever it changes, not once per session.
        //
        // A guide size change does not rebuild the feature -- the guides are handed over as subrects and
        // the output size is what the model is built for -- so a once-only line goes stale the moment the
        // player moves the quality slider, and every later line in the log is then read against numbers
        // that stopped being true. In Nioh 3 the session opened at DLAA, moved to 66% and ended at 33%,
        // and the log claimed 1920x1080 guides throughout.

        const GuideReport guidesNow {
            true,  nr.guideDepthInverted, nr.guideMvScaleX, nr.guideMvScaleY, guideWidth, guideHeight,
            width, (unsigned int) height
        };

        if (!loggedGuides.valid || loggedGuides.depthInverted != guidesNow.depthInverted ||
            loggedGuides.mvScaleX != guidesNow.mvScaleX || loggedGuides.mvScaleY != guidesNow.mvScaleY ||
            loggedGuides.guideW != guidesNow.guideW || loggedGuides.guideH != guidesNow.guideH ||
            loggedGuides.frameW != guidesNow.frameW || loggedGuides.frameH != guidesNow.frameH)
        {
            loggedGuides = guidesNow;
            LOG_INFO("DLSS-NR guides: depth {}, motion vector scale {} x {}, guides {}x{} for a {}x{} frame",
                     nr.guideDepthInverted ? "inverted" : "not inverted", nr.guideMvScaleX, nr.guideMvScaleY,
                     guideWidth, guideHeight, width, height);
        }

        if (cfg.DlssNrProxyProbe.value_or_default())
            ProbeProxyDispatch(cmdList);

        const unsigned int configuredPasses =
            std::clamp(cfg.DlssNrPasses.value_or_default(), 1u,
                       cfg.DlssNrUnlockPasses.value_or_default() ? DlssNr::MaxPassCount : DlssNr::DefaultMaxPassCount);
        const bool proxyBackend = cfg.DlssNrUseProxy.value_or_default() && configuredPasses == 1;
        const unsigned int requestedPasses = configuredPasses;
        if (cfg.DlssNrUseProxy.value_or_default() && configuredPasses > 1 && !warnedProxyPasses)
        {
            warnedProxyPasses = true;
            LOG_WARN("DLSS-NR: {} model passes require the direct backend; using it instead of the driver proxy",
                     configuredPasses);
        }
        if (usingProxy != proxyBackend)
        {
            ParkNrFeature(nr.feature);
            for (unsigned pass = 1; pass < DlssNr::MaxPassCount; ++pass)
                ParkNrFeature(nr.passFeature[pass]);
            proxy.RetryAfterFailure();
            proxyRunning = false;
            nr.reset = true;
            usingProxy = proxyBackend;
        }
        if (proxyBackend ? (!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device)) || !proxy.Available()
                         : (!EnsureForwarder() || !EnsureCapabilityParams(device)))
        {
            nr.failed = true;
            LOG_ERROR("DLSS-NR unavailable: {}", nr.reason);
            device->Release();
            return;
        }

        // What the model works at. The frame and its edit stay full resolution; only the model's input and
        // answer change size, and the resolve enlarges (or minifies) the answer while compositing. Below 1
        // the model runs reduced and cheaper; above 1 it SUPERSAMPLES -- the proxy is upscaled to a larger
        // working size so the model denoises a super-native input, which the resolve then samples back down.
        // Capped at 2x: cost grows with the area and NGX acceptance above native is what this probe tests.
        float workScale = cfg.DlssNrWorkingScale.value_or_default();
        if (!std::isfinite(workScale))
            workScale = 1.0f;
        workScale = workScale < 0.25f ? 0.25f : (workScale > 2.0f ? 2.0f : workScale);
        const auto workWidth = (unsigned int) (width * workScale + 0.5f);
        const auto workHeight = (unsigned int) (height * workScale + 0.5f);
        const bool reduced = workWidth != width || workHeight != height;
        ReleaseSurfacesIfFormatChanged(desc.Format);

        const bool resolutionChanged =
            nr.width != width || nr.height != height || nr.workWidth != workWidth || nr.workHeight != workHeight;
        const bool placementChanged = nr.width != 0 && (nr.beforeUpscale != frame.BeforeUpscale ||
                                                        nr.rayReconstruction != frame.RayReconstruction);

        // The model reads its tuning once, while the feature is built, so a changed setting only takes
        // effect when the feature is rebuilt. TuningMatchesFeature was written to notice that and then
        // never called, which is why every one of these controls appeared to do nothing until something
        // else -- a resolution change -- happened to force a rebuild by accident.
        const bool tuningChanged = !TuningMatchesFeature(cfg, requestedPasses);
        if (tuningChanged)
            nr.residualHistoryPrimed = false;

        if (resolutionChanged || placementChanged || (nr.feature != nullptr && tuningChanged))
        {
            // Parked rather than released: with frame generation the GPU can still be several frames
            // deep in work that references all of it.
            ParkNrFeature(nr.feature);
            nr.featurePendingSubmission = false;

            for (unsigned int i = 1; i < DlssNr::MaxPassCount; ++i)
            {
                ParkNrFeature(nr.passFeature[i]);
                nr.passNeedsReset[i] = false;
                nr.passCreateFailed[i] = false;
                nr.passPendingSubmission[i] = false;
            }

            // Resolution and seam changes invalidate the scratch state. Tuning does not, and throwing
            // resources away for it would mean a reallocation every time a slider moves.
            if (resolutionChanged || placementChanged)
            {
                if (placementChanged)
                    ForgetCalibration();

                ParkNrResource(nr.output);
                ParkNrResource(nr.passScratch);
                ParkNrResource(nr.colorCopy);
                ParkNrResource(nr.hdrCopy);
                ParkNrResource(nr.colorSmall);
                ParkNrResource(nr.outputNative);
                ParkNrResource(nr.activeColor);
                ParkNrResource(nr.residualEdited);
                ParkNrResource(nr.residualHistory[0]);
                ParkNrResource(nr.residualHistory[1]);
                ParkNrResource(nr.residualComposed);
                nr.residualStoreValid = false;
                nr.residualHistoryIndex = 0;
                nr.residualHistoryPrimed = false;
                nr.passScratchFailed = false;
            }
        }

        if (nr.output == nullptr)
        {
            nr.output = CreateScratch(device, desc.Format, workWidth, workHeight);
            nr.colorCopy = CreateScratch(device, desc.Format, width, height);
            nr.hdrCopy = CreateScratch(device, desc.Format, width, height);
            nr.workWidth = workWidth;
            nr.workHeight = workHeight;
            nr.width = width;
            nr.height = height;
            nr.beforeUpscale = frame.BeforeUpscale;
            nr.rayReconstruction = frame.RayReconstruction;
            nr.reset = true;
        }

        if (cropColor && nr.activeColor == nullptr)
            nr.activeColor = CreateScratch(device, desc.Format, width, height);
        if (cropColor && nr.activeColor == nullptr)
        {
            nr.failed = true;
            nr.reason = "the pre-SR active colour staging texture could not be allocated";
            LOG_ERROR("DLSS-NR unavailable: {}", nr.reason);
            device->Release();
            return;
        }

        // All carriers rest in NPSR, including their first use. CreateScratch starts
        // in UAV, so transition only newly allocated resources here.
        if (residualAcrossRr)
        {
            auto ensureReadable = [&](ID3D12Resource*& resource, DXGI_FORMAT format, unsigned w, unsigned h)
            {
                if (resource == nullptr)
                {
                    resource = CreateScratch(device, format, w, h);
                    if (resource)
                        Barrier(cmdList, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
                return resource != nullptr;
            };
            if (shader._residualPipelineState == nullptr ||
                !ensureReadable(nr.residualEdited, desc.Format, width, height) ||
                !ensureReadable(nr.residualHistory[0], DXGI_FORMAT_R16G16B16A16_FLOAT, width, height) ||
                !ensureReadable(nr.residualHistory[1], DXGI_FORMAT_R16G16B16A16_FLOAT, width, height) ||
                !ensureReadable(nr.residualComposed, nr.residualOutputFormat, nr.residualOutputWidth,
                                nr.residualOutputHeight))
            {
                ReportSkipOnce("RR residual resources unavailable; preserving the game's colour");
                nr.residualStoreValid = false;
                nr.residualHistoryPrimed = false;
                device->Release();
                return;
            }
        }

        if (requestedPasses == 1)
        {
            // Reclaim the extra raster and clear its failure latch. Raising the count later gets one fresh
            // allocation attempt; holding a failing allocation at two must not retry it every frame.
            ParkNrResource(nr.passScratch);
            nr.passScratchFailed = false;
        }
        else if (nr.passScratch == nullptr && !nr.passScratchFailed)
        {
            nr.passScratch = CreateScratch(device, desc.Format, workWidth, workHeight);
            nr.passScratchFailed = nr.passScratch == nullptr;

            if (nr.passScratchFailed)
                LOG_ERROR("DLSS-NR: could not allocate the model-output ping-pong; extra passes are disabled");
        }

        if (reduced && nr.colorSmall == nullptr)
            nr.colorSmall = CreateScratch(device, desc.Format, workWidth, workHeight);

        // The down-leg target is native (the answer is brought back to frame size before the resolve).
        if (workScale > 1.0f && nr.outputNative == nullptr)
            nr.outputNative = CreateScratch(device, desc.Format, width, height);

        if (nr.meter == nullptr)
        {
            nr.meter = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, kDlssNrMeterGrid, kDlssNrMeterGrid);

            D3D12_HEAP_PROPERTIES readback {};
            readback.Type = D3D12_HEAP_TYPE_READBACK;

            D3D12_RESOURCE_DESC bufferDesc {};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = kMeterBytes;
            bufferDesc.Height = 1;
            bufferDesc.DepthOrArraySize = 1;
            bufferDesc.MipLevels = 1;
            bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufferDesc.SampleDesc.Count = 1;
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            for (auto& rb : nr.meterReadback)
            {
                if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))))
                {
                    rb = nullptr;
                    LOG_WARN("DLSS-NR: the white point meter could not allocate its readback; falling back "
                             "to the paper white slider");
                }
            }

            if (nr.meter != nullptr)
                LOG_INFO("DLSS-NR: white point meter up, {}x{} tiles", kDlssNrMeterGrid, kDlssNrMeterGrid);
        }

        if (!proxyBackend && nr.feature == nullptr && nr.output != nullptr && nr.colorCopy != nullptr &&
            nr.hdrCopy != nullptr)
        {
            auto snippet = FindNvidiaModel();

            if (!snippet.has_value())
            {
                nr.failed = true;
                nr.reason = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
                LOG_ERROR("DLSS-NR unavailable: {}", nr.reason);
                device->Release();
                return;
            }

            SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
            const auto tuning = PassTuning(cfg, 0);
            nr.feature = nr.create(snippet->wstring().c_str(), ::State::Instance().NVNGX_ApplicationDataPath.c_str(),
                                   device, cmdList, nr.capabilityParams, workWidth, workHeight,
                                   (int) PassPreset(cfg, 0), tuning.intensity, (int) PassStyle(cfg, 0),
                                   tuning.structure, tuning.tone, tuning.skin, tuning.autoMask ? 1 : 0,
                                   // UI correction at the model's own default: with no UI layer fed to it there
                                   // is nothing for it to correct.
                                   1);

            if (nr.feature == nullptr)
            {
                nr.featurePendingSubmission = false;
                nr.failed = true;
                nr.reason = "the model would not initialise";
                if (nr.lastModelError && *nr.lastModelError())
                {
                    nr.modelError = nr.lastModelError();
                    nr.reason = nr.modelError.c_str();
                }
                const auto initResult = (unsigned int) (nr.lastInit != nullptr ? *nr.lastInit : 0);
                const auto createResult = (unsigned int) (nr.lastCreate != nullptr ? *nr.lastCreate : 0);

                // Cast before formatting. These are ints, and "0x{:X}" on a negative int prints
                // 0x-452FFFFF, which no one can decode back to 0xBAD00001.
                LOG_ERROR("DLSS-NR create failed: init 0x{:X} ({}), create 0x{:X} ({})", initResult,
                          NgxResultName(initResult), createResult, NgxResultName(createResult));
                device->Release();
                return;
            }

            nr.width = width;
            nr.height = height;
            nr.beforeUpscale = frame.BeforeUpscale;
            nr.rayReconstruction = frame.RayReconstruction;
            nr.reset = true;
            nr.featurePendingSubmission = true;
            nr.featureCreateEpoch = frame.SubmissionEpoch;
            RecordBuiltPrimaryTuning(cfg);
            LOG_INFO("DLSS-NR model feature created from {}", snippet->string());
            LOG_INFO("DLSS-NR running {}: target {}x{}, model {}x{}, guides {}x{} "
                     "(preset {}, intensity {}, style {}, build epoch {})",
                     frame.RayReconstruction ? (frame.BeforeUpscale ? "before RR+SR" : "after RR+SR")
                                             : (frame.BeforeUpscale ? "before SR" : "after SR"),
                     width, height, workWidth, workHeight, guideWidth, guideHeight, nr.builtPreset[0],
                     nr.builtIntensity, nr.builtStyle[0], frame.SubmissionEpoch);

            // Creating and evaluating a feature in the same command list is the dice-roll that hung the
            // GPU (every crash died on a creation frame). The creation goes through the game's own submit
            // first; the first evaluate happens next frame. One frame without the model is invisible.
            device->Release();
            return;
        }

        if (!proxyBackend && nr.feature == nullptr)
        {
            device->Release();
            return;
        }

        // A later function call is not proof that the command list containing CreateFeature was
        // submitted: some engines record more than one upscale on the same list. Native DX12 supplies
        // the wrapped Present count and the bridges supply their post-Execute frame counter, so an epoch
        // change is the first point at which evaluating the feature is safe.
        if (!proxyBackend && nr.featurePendingSubmission)
        {
            if (frame.SubmissionEpoch == nr.featureCreateEpoch)
            {
                device->Release();
                return;
            }

            nr.featurePendingSubmission = false;
            LOG_INFO("DLSS-NR: primary feature ready after submitted epoch {}", nr.featureCreateEpoch);
        }

        // Park no-longer-requested feature histories immediately (their actual release remains deferred),
        // and clear their failure latch so a later 1 -> N change is a deliberate retry.
        for (unsigned int pass = 1; pass < DlssNr::MaxPassCount; ++pass)
        {
            if (pass >= requestedPasses)
            {
                ParkNrFeature(nr.passFeature[pass]);
                nr.passNeedsReset[pass] = false;
                nr.passCreateFailed[pass] = false;
                nr.passPendingSubmission[pass] = false;
            }
        }

        // Do not create another feature, and do not evaluate any feature, while a requested layer still
        // belongs to the current submission epoch. This keeps multiple upscaler evaluations recorded on
        // one command list from recreating the historical create/evaluate GPU hang.
        for (unsigned int pass = 1; pass < requestedPasses; ++pass)
        {
            if (!nr.passPendingSubmission[pass])
                continue;

            if (frame.SubmissionEpoch == nr.passCreateEpoch[pass])
            {
                device->Release();
                return;
            }

            nr.passPendingSubmission[pass] = false;
            LOG_INFO("DLSS-NR: feature for pass {} ready after submitted epoch {}", pass + 1, nr.passCreateEpoch[pass]);
        }

        // Build at most one missing extra feature on this invocation and evaluate nothing afterwards.
        // NGX feature creation records work on the supplied command list; evaluating that feature before
        // the list has been submitted is the creation-frame GPU hang that caused the old multi-pass path
        // to be removed. A new feature therefore gets an entire build-only frame and starts next time.
        if (nr.passScratch != nullptr)
        {
            for (unsigned int pass = 1; pass < requestedPasses; ++pass)
            {
                if (nr.passFeature[pass] != nullptr)
                    continue;

                if (nr.passCreateFailed[pass])
                    break;

                auto snippet = FindNvidiaModel();

                if (!snippet.has_value())
                {
                    nr.passCreateFailed[pass] = true;
                    LOG_ERROR("DLSS-NR: pass {} feature not built because {} disappeared", pass + 1,
                              "nvngx_dlssnr.dll");
                }
                else
                {
                    SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
                    const auto tuning = PassTuning(cfg, pass);
                    nr.passFeature[pass] =
                        nr.create(snippet->wstring().c_str(), ::State::Instance().NVNGX_ApplicationDataPath.c_str(),
                                  device, cmdList, nr.capabilityParams, workWidth, workHeight,
                                  (int) PassPreset(cfg, pass), tuning.intensity, (int) PassStyle(cfg, pass),
                                  tuning.structure, tuning.tone, tuning.skin, tuning.autoMask ? 1 : 0, 1);

                    if (nr.passFeature[pass] != nullptr)
                    {
                        nr.builtPreset[pass] = PassPreset(cfg, pass);
                        nr.builtPassTuning[pass] = tuning;
                        nr.builtStyle[pass] = PassStyle(cfg, pass);
                        nr.passNeedsReset[pass] = true;
                        nr.passPendingSubmission[pass] = true;
                        nr.passCreateEpoch[pass] = frame.SubmissionEpoch;
                        LOG_INFO("DLSS-NR: feature for pass {} built with preset {}, style {} at epoch {}; "
                                 "waiting for submission",
                                 pass + 1, nr.builtPreset[pass], nr.builtStyle[pass], frame.SubmissionEpoch);
                    }
                    else
                    {
                        nr.passPendingSubmission[pass] = false;
                        nr.passCreateFailed[pass] = true;
                        LOG_ERROR("DLSS-NR: feature for pass {} failed to build; using {} ready pass(es)", pass + 1,
                                  pass);
                    }
                }

                device->Release();
                return;
            }
        }

        // The upscaler has just written this, so it is a UAV. The model needs it readable.
        // Whether the buffer the upscaler just wrote is linear HDR or an already tone-mapped picture is not
        // something to assume: the game says so, in the flags it created its own DLSS feature with. Running
        // the colour transform over a frame that has already been through a tonemapper is pure damage, and
        // skipping it on one that has not leaves the model reading ordinary values as enormously bright.
        // EvaluateInternal has already combined the game's HDR flag with the authoritative output format.
        // That authority matters before SR: Color and Output may use different surface formats while still
        // representing the same frame colour space.
        const bool isHdrBuffer = frame.ColourIsLinearHdr;

        if (!reportedHdr || reportedHdrValue != isHdrBuffer || reportedBefore != frame.BeforeUpscale)
        {
            reportedHdr = true;
            reportedHdrValue = isHdrBuffer;
            reportedBefore = frame.BeforeUpscale;
            LOG_INFO("DLSS-NR {} SR: the game's DLSS colour space is {} so the colour transform is {}",
                     frame.BeforeUpscale ? "before" : "after", isHdrBuffer ? "linear HDR" : "already tone-mapped",
                     isHdrBuffer ? "on" : "off");
        }

        const bool haveCodec = shader.IsInit();

        if (!haveCodec)
        {
            nr.failed = true;
            nr.reason = "the colour codec would not compile";
            LOG_ERROR("DLSS-NR unavailable: {}", nr.reason);
            device->Release();
            return;
        }

        // What the upscaler produces is linear HDR with an open-ended range; the model was trained on
        // finished, sRGB-encoded frames. The white point is what maps one to the other, and it is a property
        // of the game's exposure rather than a number worth asking anyone to guess: measured means of 0.065,
        // 1.8 and 185 have all been seen in this one game.
        ++frames;
        TickNrRetired();
        CheckCaptureTrigger();

        if (captureWriteAtFrame != 0 && frames >= captureWriteAtFrame)
        {
            captureWriteAtFrame = 0;
            const auto captureDir = Util::DllPath().remove_filename() / "dlssnr-capture";
            const auto written = captureFrames.write(captureDir);

            if (!written.empty())
                LOG_INFO("DLSS-NR wrote matched before/after frames to {}", written);
        }

        // Paper white, and nothing else. The frame is divided by this and encoded, and the soft knee
        // above 0.75 takes whatever is left over.
        //
        // It used to be divided by a white point measured from the frame -- around 3 in Cyberpunk -- which
        // was right for the old composition, where the encode had to be inverted and highlights therefore
        // had to survive it. Under the composition this now uses it is actively wrong twice over: the
        // model is handed a picture three times darker than it should see, and the highlight branch is
        // defeated. That branch hands back `originalLuma - proxyLuma`, the headroom the proxy could not
        // represent -- it exists precisely because the proxy is meant to clip. Normalising the highlights
        // away first leaves it nothing to give back.

        ResTrack_Dx12::HookLateNrQueue(device);
        if (gpuTime == nullptr)
            gpuTime = std::make_unique<DlssNrGpuTime>(device, "total");

        if (ngxTime == nullptr)
            ngxTime = std::make_unique<DlssNrGpuTime>(device, "model");

        if (gpuTime != nullptr)
            gpuTime->Start(cmdList);

        // Copy just the live image, not the stale right/bottom margins. Do this only after model
        // creation/pending-submission early returns, and inside the measured GPU interval. The compact
        // texture lets every existing codec/compare/hold/capture path use unmodified pixel coordinates.
        ID3D12Resource* const gameColor = target;
        if (cropColor)
        {
            TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cmdList, nr.activeColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            DlssNr::CopyActiveColor(cmdList, nr.activeColor, gameColor, *active);
            TransitionTarget(outputArrival);
            Barrier(cmdList, nr.activeColor, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            target = nr.activeColor;
            targetState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }

        const auto FinishColor = [&](bool copyBack)
        {
            if (cropColor)
            {
                if (copyBack)
                {
                    TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(cmdList, gameColor, outputArrival, D3D12_RESOURCE_STATE_COPY_DEST);
                    DlssNr::CopyActiveColor(cmdList, gameColor, target, *active);
                    Barrier(cmdList, gameColor, D3D12_RESOURCE_STATE_COPY_DEST, outputArrival);
                }
                TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            else
            {
                TransitionTarget(outputArrival);
            }
        };

        // Fetch the game's exposure, where the game supplies one and the user asked for it.
        //
        // This used to measure the white point off the frame as well, over a 64x64 grid of tile
        // luminances. That is gone: the pass writes the frame it was measuring, so the divisor chased its
        // own output -- one Enshrouded session walked it from 0.010 to 97.910, and toggling NR at a fixed
        // spot read 41.31 off against 0.46 on. What remains dispatches a single thread to copy the game's
        // 1x1 exposure texture into tile 0. That is a courier, not a measurement, and cannot feed back.
        // Gated on the source the menu actually writes. This read the retired WhitePointFromExposure
        // flag while consumption keyed on WhitePointSource == 1, so choosing "the game's own exposure"
        // never dispatched the meter and the white point silently fell back to the slider.
        const bool exposureSettingOn = cfg.DlssNrWhitePointSource.value_or_default() == 1;

        // Nothing held from before the option was switched off may survive switching it back on. See
        // InvalidateExposureMeter for what froze and why it read as a colour cast.
        if (exposureSettingOn && !nr.exposureSettingWasOn)
        {
            InvalidateExposureMeter();
            LOG_INFO("DLSS-NR exposure: option switched on, held reading discarded");
        }

        nr.exposureSettingWasOn = exposureSettingOn;

        const bool wantExposure = exposureSettingOn && frame.ExposureTexture != nullptr;

        if (nr.meter != nullptr && wantExposure)
        {
            DlssNrConstants meterParams {};
            meterParams.Mode = DlssNrMode_Meter;

            // One pixel. Only tile (0,0) is read back, and the tile-mean branch below it in the shader is
            // dead code the dispatch simply never reaches.
            meterParams.Width = 1;
            meterParams.Height = 1;

            const D3D12_RESOURCE_STATES priorTargetState = targetState;
            TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            shader.DispatchPass(cmdList, meterParams, target, nullptr, nullptr, (ID3D12Resource*) frame.ExposureTexture,
                                nullptr, nr.meter, nullptr);
            TransitionTarget(priorTargetState);

            CopyMeterToReadback(cmdList, device, true);
            ConsumeMeterReadback();
        }

        nr.gamePreExposure = frame.PreExposure;

        float whitePoint =
            frame.WhitePointOverride > 0.0f ? frame.WhitePointOverride : ResolveWhitePoint(cfg, isHdrBuffer);

        // Zero-latency exposure (D3D12, source 1): when the game hands us a live exposure texture, the
        // white point is recomputed in-shader every frame from it (ExposurePreMul / exposure) instead of
        // the 3-4 frame CPU meter readback. whitePoint above still rides along in gWhitePoint as the
        // fallback the shader uses if the live sample is missing or absurd. Bound at t4 (InPrevEdit) below.
        ID3D12Resource* exposureTex = nullptr;
        uint32_t useGameExposure = 0;
        float exposurePreMul = 0.0f;

        if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && frame.ExposureTexture != nullptr)
        {
            exposureTex = (ID3D12Resource*) frame.ExposureTexture;
            useGameExposure = 1;
            const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
            exposurePreMul = nr.gamePreExposure * trim;
        }

        // Frame hold. Freeze the encode's input so a live setting change re-renders the same frame. This
        // is self-contained on purpose: it copies the output aside on hold-on and copies it BACK over the
        // live output before the encode reads it while held, so the encode's own path and barriers below
        // are untouched and the default (hold off) is byte-identical. See design/frame-hold.md.
        //
        // `target` is UAV here (normalised at entry, restored by the meter block above). The held copy is
        // left in COPY_SOURCE after capture and stays there for every restore.
        {
            const bool hold = cfg.DlssNrHoldFrame.value_or_default();

            if (hold)
            {
                const D3D12_RESOURCE_DESC td = target->GetDesc();
                const bool needCapture = !nr.heldActive || nr.heldColor == nullptr ||
                                         (unsigned int) td.Width != nr.heldWidth || td.Height != nr.heldHeight ||
                                         td.Format != nr.heldFormat;

                if (needCapture)
                {
                    // Hold-on (or the output changed shape under a hold): capture THIS frame, do not
                    // restore -- target already holds the frame to freeze, and the pass runs on it.
                    if (nr.heldColor != nullptr)
                        ParkNrResource(nr.heldColor);

                    nr.heldColor = CreateScratch(device, td.Format, (unsigned int) td.Width, td.Height);

                    if (nr.heldColor != nullptr)
                    {
                        const D3D12_RESOURCE_STATES priorTargetState = targetState;
                        TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
                        Barrier(cmdList, nr.heldColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_COPY_DEST);
                        cmdList->CopyResource(nr.heldColor, target);
                        Barrier(cmdList, nr.heldColor, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_COPY_SOURCE);
                        TransitionTarget(priorTargetState);

                        nr.heldActive = true;
                        nr.heldWidth = (unsigned int) td.Width;
                        nr.heldHeight = td.Height;
                        nr.heldFormat = td.Format;
                        nr.heldWhitePoint = whitePoint;
                    }
                }
                else
                {
                    // Held: restore the frozen frame onto the live output before the encode reads it.
                    const D3D12_RESOURCE_STATES priorTargetState = targetState;
                    TransitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
                    cmdList->CopyResource(target, nr.heldColor);
                    TransitionTarget(priorTargetState);
                }

                // Suspend white-point measurement while held: use the snapshot so it cannot drift and
                // confound the comparison. (No-op on the capture frame, where the snapshot IS whitePoint.)
                if (nr.heldActive)
                    whitePoint = nr.heldWhitePoint;
            }
            else if (nr.heldActive)
            {
                // Released: let go of the frozen frame and resume live input next frame.
                if (nr.heldColor != nullptr)
                    ParkNrResource(nr.heldColor);
                nr.heldActive = false;
            }
        }

        DlssNrConstants encodeParams {};
        encodeParams.Mode = DlssNrMode_Encode;
        // A frame that is already display-referred is handed over untouched: the encode becomes a copy and
        // the resolve adds the model's edit back at full scale.
        encodeParams.Passthrough = isHdrBuffer ? 0u : 1u;
        encodeParams.WhitePoint = whitePoint;
        encodeParams.UseGameExposure = useGameExposure;
        encodeParams.ExposurePreMul = exposurePreMul;
        encodeParams.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
        // Match only takes effect once a fit exists; until then the table is empty and the shader would
        // read a curve of zeros, so it falls back to the plain proxy.
        encodeParams.Width = width;
        encodeParams.Height = height;

        TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        shader.DispatchPass(cmdList, encodeParams, target, nullptr, nullptr, nullptr, exposureTex, nr.colorCopy,
                            nr.hdrCopy);

        if (targetSupportsUav)
            TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // The transitions double as the wait for the encode's writes.
        Barrier(cmdList, nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // Measure the buffer's scale from the copy the encode just kept -- untouched, so there is no path
        // (Calibration pass removed: it produced only a menu suggestion nothing consumed, at the cost
        // of a 4096-thread dispatch, a readback and an nth_element every frame.)

        Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // Below full resolution the model is shown a filtered shrink of the proxy; the edit it returns is
        // enlarged during the resolve while the frame underneath stays full size and untouched.
        ID3D12Resource* modelInput = nr.colorCopy;

        if (reduced && nr.colorSmall != nullptr)
        {
            bool built = false;

            if (workScale > 1.0f)
            {
                // Supersample: enlarge the proxy to the larger working size with a real upscaling filter
                // (the Output Scaling upsampler) so the model sees a clean super-native input, rather than
                // the box minifier which only makes sense going down. colorCopy is NON_PIXEL_SHADER_RESOURCE
                // from the encode (SRV-ready); colorSmall is UNORDERED_ACCESS from last frame's resolve.
                // (Re)build the supersample scalers when missing or when the NR downscaler changed (the
                // filter is baked at construction). Both use NR's own DlssNrScalingDownscaler, independent
                // of Output Scaling, so the two can run different filters at once. superDown is built here
                // and used after the model (the down-leg below).
                const Scaler nrScaler = cfg.DlssNrScalingDownscaler.value_or_default();
                if (nr.nrScaler != nrScaler)
                {
                    if (nr.superUp != nullptr)
                    {
                        delete nr.superUp;
                        nr.superUp = nullptr;
                    }
                    if (nr.superDown != nullptr)
                    {
                        delete nr.superDown;
                        nr.superDown = nullptr;
                    }
                    nr.nrScaler = nrScaler;
                }
                if (nr.superUp == nullptr)
                    nr.superUp = new OS_Dx12("DLSS-NR supersample up", device, true, nrScaler);
                if (nr.superDown == nullptr)
                    nr.superDown = new OS_Dx12("DLSS-NR supersample down", device, false, nrScaler);

                if (nr.superUp != nullptr && nr.superUp->Dispatch(cmdList, nr.colorCopy, nr.colorSmall))
                {
                    Barrier(cmdList, nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    built = true;
                }
            }

            if (!built)
            {
                if (workScale > 1.0f)
                {
                    // Wanted to supersample but the upscaler was not available -- warn once; the box path
                    // below can only enlarge blockily, so the user should know the clean path is off.

                    if (!warnedSuper)
                    {
                        warnedSuper = true;
                        LOG_WARN("DLSS-NR supersample: upscaler unavailable, falling back to a blocky enlarge.");
                    }
                }

                // Sub-native (or the upsampler could not be built): box-resample the proxy to the work size.
                DlssNrConstants down {};
                down.Mode = DlssNrMode_Downsample;
                down.Width = workWidth;
                down.Height = workHeight;
                shader.DispatchPass(cmdList, down, modelInput, nullptr, nullptr, nullptr, nullptr, nr.colorSmall,
                                    nullptr);
                Barrier(cmdList, nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }

            modelInput = nr.colorSmall;
        }

        // Read the exposure scan's candidates on the pass's own command list, once a frame.
        DlssNr::ExposureScan::Tick(device, cmdList, frame.SubmissionEpoch);

        ID3D12Resource* depthIn = ReadableGuide(device, cmdList, depth, &nr.depthClone);
        ID3D12Resource* motionIn = ReadableGuide(device, cmdList, motion, &nr.motionClone);

        if (depthIn == nullptr || motionIn == nullptr)
        {
            nr.failed = true;
            nr.reason = "the game's depth or motion vectors could not be made readable";
            LOG_ERROR("DLSS-NR unavailable: {}", nr.reason);
            FinishColor(false);
            device->Release();
            return;
        }

        // The vectors were scaled to full-frame pixels; the image the model reprojects is the working size.
        // The vectors were scaled to full-frame pixels; the image the model reprojects is the
        // working size.
        const float mvToWorkX = width != 0 ? (float) workWidth / (float) width : 1.0f;
        const float mvToWorkY = height != 0 ? (float) workHeight / (float) height : 1.0f;

        SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);

        // The proxy path, when asked for. Same inputs, same model -- the difference is who calls it.
        //
        // Nothing falls back automatically. A silent fallback would mean never finding out the proxy
        // path was broken: the picture would look right either way, because the forwarder would be
        // quietly doing the work.
        if (ngxTime != nullptr)
            ngxTime->Start(cmdList);

        // Count only a contiguous set of ready, separate feature histories. A failed extra creation never
        // falls back to reusing the main feature: that tells one temporal model several frames elapsed in
        // one game frame and makes its history fight the later layers.
        unsigned int effectivePasses = 1;
        if (nr.passScratch != nullptr)
        {
            for (unsigned int pass = 1; pass < requestedPasses; ++pass)
            {
                if (nr.passFeature[pass] == nullptr || nr.passPendingSubmission[pass])
                    break;
                ++effectivePasses;
            }
        }

        {

            if (loggedConfigured != configuredPasses || loggedEffective != effectivePasses)
            {
                loggedConfigured = configuredPasses;
                loggedEffective = effectivePasses;
                LOG_INFO("DLSS-NR model passes: configured {}, effective {}", configuredPasses, effectivePasses);
            }
        }

        // Encode happened once above. Keep that base proxy immutable and ping-pong only model answers:
        //   pass 0: base -> A, pass 1: A -> B, pass 2: B -> A.
        // The final answer is resolved once against the original base, so matched-residual transfer is the
        // cumulative final-minus-base edit and colour/transfer controls are not compounded.
        ID3D12Resource* passInput = modelInput;
        ID3D12Resource* passOutput = nr.output;
        ID3D12Resource* finalAnswer = nullptr;
        bool outputReadable = false;
        bool scratchReadable = false;

        const auto MakeModelReadable = [&](ID3D12Resource* resource)
        {
            bool& readable = resource == nr.output ? outputReadable : scratchReadable;
            if (!readable)
            {
                Barrier(cmdList, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                readable = true;
            }
        };

        const auto MakeModelWritable = [&](ID3D12Resource* resource)
        {
            bool& readable = resource == nr.output ? outputReadable : scratchReadable;
            if (readable)
            {
                Barrier(cmdList, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                readable = false;
            }
        };

        int result = NVSDK_NGX_Result_Success;

        for (unsigned int pass = 0; pass < effectivePasses && result == NVSDK_NGX_Result_Success; ++pass)
        {
            void* const passFeature = pass == 0 ? nr.feature : nr.passFeature[pass];
            const bool passReset = nr.reset || (pass > 0 && nr.passNeedsReset[pass]);
            const auto tuning = PassTuning(cfg, pass);

            MakeModelWritable(passOutput);
            bool evaluated = true;
            if (proxyBackend)
            {
                result = static_cast<int>(proxy.Run(cmdList, device, passInput, depthIn, motionIn, passOutput,
                                                    workWidth, workHeight, guideWidth, guideHeight, motionWidth,
                                                    motionHeight, depthBaseX, depthBaseY, motionBaseX, motionBaseY,
                                                    nr.guideDepthInverted, passReset, nr.guideMvScaleX * mvToWorkX,
                                                    nr.guideMvScaleY * mvToWorkY, &evaluated));
                proxyRunning = evaluated && result == NVSDK_NGX_Result_Success;
            }
            else
            {
                result = nr.evaluate(
                    cmdList, passFeature, nr.capabilityParams, passInput, depthIn, motionIn, passOutput, workWidth,
                    workHeight, guideWidth, guideHeight, motionWidth, motionHeight, depthBaseX, depthBaseY, motionBaseX,
                    motionBaseY, nr.guideDepthInverted ? 1 : 0, passReset ? 1 : 0, tuning.intensity,
                    (int) PassStyle(cfg, pass), tuning.structure, tuning.tone, tuning.skin, tuning.autoMask ? 1 : 0,
                    nr.guideMvScaleX * mvToWorkX, nr.guideMvScaleY * mvToWorkY);
            }
            if (!evaluated)
                break;

            if (result != NVSDK_NGX_Result_Success)
                break;

            if (pass > 0)
                nr.passNeedsReset[pass] = false;

            finalAnswer = passOutput;
            MakeModelReadable(finalAnswer);

            if (pass + 1 < effectivePasses)
            {
                passInput = finalAnswer;
                passOutput = passOutput == nr.output ? nr.passScratch : nr.output;
            }
        }

        if (ngxTime != nullptr)
            ngxTime->End(cmdList);

        nr.reset = finalAnswer == nullptr;

        // Supersampling probe: report the model working ABOVE native so a test log tells us whether NGX even
        // accepts a super-native evaluate and what it returns. Once per working-size change, or on any error.
        if (workWidth > width || workHeight > height)
        {

            if (lastSuper != workWidth || result != 1)
            {
                lastSuper = workWidth;
                LOG_INFO("DLSS-NR SUPERSAMPLE: model at {}x{} = {:.2f}x native {}x{}, evaluate result {} ({})",
                         workWidth, workHeight, (float) workWidth / (float) width, width, height, result,
                         NgxResultName((unsigned int) result));
            }
        }

        // Once, a few seconds in, so it lands after the values have been written at least once.

        if (!proxyBackend && !tuningReported && frames > 240)
        {
            tuningReported = true;

            // This checks the parameter table, not whether the neural network uses a
            // setting. Multipass leaves the final pass's values in this shared table.
            auto report = [this](const char* name, float wrote)
            {
                float value = 0.0f;
                const NVSDK_NGX_Result r = nr.capabilityParams->Get(name, &value);
                LOG_INFO("DLSS-NR readback {} -> {} (we wrote {}, result 0x{:X})", name, value, wrote, (uint32_t) r);
            };

            const auto lastTuning = PassTuning(cfg, effectivePasses - 1);
            LOG_INFO("DLSS-NR parameter-table readback for pass {} (not proof of visual effect)", effectivePasses);
            report("DLSSNR.Intensity", lastTuning.intensity);
            report("DLSSNR.LocalStructureStrength", lastTuning.structure);
            report("DLSSNR.LocalToneStrength", lastTuning.tone);
            report("DLSSNR.SkinStructureStrength", lastTuning.skin);
            unsigned int autoMask = 0;
            const auto maskResult = nr.capabilityParams->Get("DLSSNR.UseAutoMask", &autoMask);
            LOG_INFO("DLSS-NR AutoMask readback: {} (wrote {}, result 0x{:X})", autoMask, lastTuning.autoMask,
                     (uint32_t) maskResult);

            unsigned int style = 0;
            const NVSDK_NGX_Result styleResult = nr.capabilityParams->Get("DLSSNR.Style", &style);
            LOG_DEBUG("DLSS-NR readback DLSSNR.Style -> {} (result 0x{:X})", style, (uint32_t) styleResult);

            // The preset is the last control whose arrival has never been checked, and three of them look
            // identical in play. Either it is not landing or the presets really are alike.
            unsigned int preset = 0;
            const NVSDK_NGX_Result presetResult = nr.capabilityParams->Get("DLSSNR.Hint.Render.Preset", &preset);
            LOG_DEBUG("DLSS-NR readback DLSSNR.Hint.Render.Preset -> {} (result 0x{:X}, we wrote {})", preset,
                      (uint32_t) presetResult, PassPreset(cfg, 0));

            LOG_DEBUG("DLSS-NR wrote intensity {}, local structure {}, local tone {}, skin {}, style {}",
                      cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                      cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                      PassStyle(cfg, 0));
        }

        if (result == NVSDK_NGX_Result_Success && finalAnswer != nullptr)
        {
            // Resolve takes the difference between what the model returned and what it was shown, and adds
            // that back to the frame. At strength zero the result is what the upscaler produced, exactly, and
            // anything the model left alone is untouched rather than round-tripped through the curve.
            DlssNrConstants resolveParams {};
            resolveParams.Mode = DlssNrMode_Resolve;
            resolveParams.WhitePoint = whitePoint;
            resolveParams.UseGameExposure = useGameExposure;
            resolveParams.ExposurePreMul = exposurePreMul;
            resolveParams.Width = width;
            resolveParams.Height = height;
            resolveParams.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
            const auto strength = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f; };
            resolveParams.SkinProtection = cfg.DlssNrSkinProtection.value_or_default();
            resolveParams.ShowSkinMask = cfg.DlssNrShowSkinMask.value_or_default();
            resolveParams.SkinDetail = strength(cfg.DlssNrSkinDetail.value_or_default());
            resolveParams.SkinColour =
                cfg.DlssNrSkinToneEnabled.value_or_default() ? strength(cfg.DlssNrSkinColour.value_or_default()) : 0.0f;
            resolveParams.EnvironmentDetail = strength(cfg.DlssNrEnvironmentDetail.value_or_default());
            resolveParams.EnvironmentColour = strength(cfg.DlssNrEnvironmentColour.value_or_default());
            resolveParams.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
            resolveParams.DebugView = cfg.DlssNrDebugView.value_or_default();
            resolveParams.MaxRatio = cfg.DlssNrMaxRatio.value_or_default();
            resolveParams.Transfer = cfg.DlssNrTransfer.value_or_default();
            resolveParams.DebugScale = cfg.DlssNrWhitePointScale.value_or_default();
            resolveParams.Passthrough = isHdrBuffer ? 0u : 1u;
            resolveParams.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
            resolveParams.ApplyModel = cfg.DlssNrApplyModel.value_or_default() ? 1u : 0u;
            resolveParams.CompareMode = cfg.DlssNrCompare.value_or_default();
            resolveParams.CompareSplit = cfg.DlssNrCompareSplit.value_or_default();
            resolveParams.CompareZoom = std::max(1.0f, cfg.DlssNrCompareZoom.value_or_default());
            resolveParams.CompareSwap = cfg.DlssNrCompareSwap.value_or_default() ? 1u : 0u;

            // The numbers the composition actually ran with, logged when any of them changes.
            //
            // A colour report without these cannot be read. Paper white alone decides whether the model
            // was shown a sensible picture or a blown one, and it was absent from every log in the first
            // round of reports -- one tester's "much better at 16" had to be taken on trust because
            // nothing in the file said what the value was. Debug view and compare mode are here for the
            // same reason from the other direction: both change what is on screen, and a screenshot with
            // one left on is indistinguishable from a bug.

            // Quantised to the precision it is printed at. Comparing raw floats logged 2376 lines in one
            // Enshrouded session, because a measured white point drifts continuously and every drift was a
            // change. A line per meaningful change is the point; a line per frame is a different problem.
            const ComposeReport composeNow { true,
                                             std::round(resolveParams.WhitePoint * 100.0f) / 100.0f,
                                             resolveParams.TransferStrength,
                                             resolveParams.ColourStrength,
                                             resolveParams.MaxRatio,
                                             resolveParams.Passthrough,
                                             resolveParams.DebugView,
                                             resolveParams.CompareMode,
                                             resolveParams.Transfer,
                                             nr.workWidth,
                                             nr.workHeight,
                                             effectivePasses };

            if (!loggedCompose.valid || loggedCompose.whitePoint != composeNow.whitePoint ||
                loggedCompose.transfer != composeNow.transfer || loggedCompose.colour != composeNow.colour ||
                loggedCompose.maxRatio != composeNow.maxRatio || loggedCompose.passthrough != composeNow.passthrough ||
                loggedCompose.debugView != composeNow.debugView ||
                loggedCompose.compareMode != composeNow.compareMode || loggedCompose.residual != composeNow.residual ||
                loggedCompose.workW != composeNow.workW || loggedCompose.workH != composeNow.workH ||
                loggedCompose.passes != composeNow.passes)
            {
                loggedCompose = composeNow;
                LOG_INFO("DLSS-NR composition: paper white {:.2f}x, detail {:.2f}, colour {:.2f}, guard "
                         "{:.1f}x, colour transform {}, transfer {}, model {}x{}, passes {}, debug view {}, compare {}",
                         composeNow.whitePoint, composeNow.transfer, composeNow.colour, composeNow.maxRatio,
                         composeNow.passthrough != 0 ? "off (frame already tone mapped)" : "on (linear HDR)",
                         composeNow.residual == 1 ? "matched residual" : "classic", composeNow.workW, composeNow.workH,
                         composeNow.passes, composeNow.debugView, composeNow.compareMode);
            }

            // Supersampling down-leg. Average the Nx model answer back to native with the chosen filter, so
            // the resolve composites a native answer against the native proxy 1:1 -- a real area resample,
            // not the single bilinear tap the Nx answer would otherwise get in the resolve (which aliases
            // the model's detail into noise, the "noisier above 100%" the probe showed). On success the
            // resolve reads the native proxy (colorCopy) and native answer (outputNative); on failure it
            // falls back to the Nx pair. finalAnswer is NPSR here; outputNative is UAV from last frame.
            bool superDownOk = false;
            if (workScale > 1.0f && nr.superDown != nullptr && nr.outputNative != nullptr &&
                nr.superDown->Dispatch(cmdList, finalAnswer, nr.outputNative))
            {
                Barrier(cmdList, nr.outputNative, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                superDownOk = true;
            }

            ID3D12Resource* resolveProxy = superDownOk ? nr.colorCopy : modelInput;
            ID3D12Resource* resolveAnswer = superDownOk ? nr.outputNative : finalAnswer;

            // Pre-SR Color is not guaranteed to have UAV support. Write directly when legal; otherwise
            // resolve into hdrCopy while the original Color remains readable, then copy the result back.
            // ResidualAcrossRR overrides both: the resolve goes to an owned scratch and nothing is
            // written back to Color -- the edit is carried to the after-SR seam as a residual instead.
            ID3D12Resource* resolveOriginal = residualAcrossRr ? nr.hdrCopy : (targetSupportsUav ? nr.hdrCopy : target);
            ID3D12Resource* resolveTarget =
                residualAcrossRr ? nr.residualEdited : (targetSupportsUav ? target : nr.hdrCopy);

            if (residualAcrossRr)
            {
                Barrier(cmdList, nr.residualEdited, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            else if (targetSupportsUav)
            {
                TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            else
            {
                Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }

            // Apply the shared strength once, at the post-RR seam.
            if (residualAcrossRr)
                resolveParams.TransferStrength = 1.0f;
            const bool resolved = shader.DispatchPass(cmdList, resolveParams, resolveProxy, resolveAnswer,
                                                      resolveOriginal, motionIn, exposureTex, resolveTarget, nullptr);

            if (residualAcrossRr)
            {
                Barrier(cmdList, nr.residualEdited, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                const unsigned prev = nr.residualHistoryIndex & 1u;
                const unsigned cur = prev ^ 1u;
                Barrier(cmdList, nr.residualHistory[cur], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                DlssNrConstants accum {};
                accum.Mode = DlssNrResidualMode_Accumulate;
                accum.Width = width;
                accum.Height = height;
                accum.ResidualBlend = std::clamp(cfg.DlssNrResidualAcrossRrBlend.value_or_default(), 0.01f, 1.0f);
                accum.ResidualHistoryValid = nr.residualHistoryPrimed ? 1u : 0u;
                accum.GuideWidth = motionWidth;
                accum.GuideHeight = motionHeight;
                accum.ResidualMotionBaseX = motionBaseX;
                accum.ResidualMotionBaseY = motionBaseY;
                accum.MvScaleX = frame.MvScaleX / (float) width;
                accum.MvScaleY = frame.MvScaleY / (float) height;
                const bool accumulated = resolved && shader.DispatchResidualPass(
                                                         cmdList, accum, nr.hdrCopy, nr.residualEdited,
                                                         nr.residualHistory[prev], motionIn, nr.residualHistory[cur]);
                Barrier(cmdList, nr.residualHistory[cur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                nr.residualHistoryPrimed = accumulated;
                nr.residualStoreValid = accumulated;
                if (accumulated)
                    nr.residualHistoryIndex = cur;
            }
            else if (!targetSupportsUav)
            {
                Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                const D3D12_RESOURCE_STATES priorTargetState = targetState;
                TransitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
                cmdList->CopyResource(target, nr.hdrCopy);
                TransitionTarget(priorTargetState);
                Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }

            MakeModelWritable(nr.output);
            if (nr.passScratch != nullptr)
                MakeModelWritable(nr.passScratch);

            if (superDownOk)
                Barrier(cmdList, nr.outputNative, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            // On-demand capture works in this path too: the staging copy still holds the frame as the
            // upscaler produced it, and the edited frame is the output itself. The write happens a few
            // frames later, once the GPU is certainly past these copies -- this path has no fence of its
            // own.
            if (captureFrames.isActive())
            {
                captureFrames.record(cmdList, device, nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                     target, targetState);

                if (captureFrames.readyToWrite() && captureWriteAtFrame == 0)
                    captureWriteAtFrame = frames + 8;
            }
        }
        else if (result != NVSDK_NGX_Result_Success)
        {
            nr.failed = true;
            nr.reason = "the model refused to run";
            if (nr.lastModelError && *nr.lastModelError())
            {
                nr.modelError = nr.lastModelError();
                nr.reason = nr.modelError.c_str();
            }
            LOG_ERROR("DLSS-NR evaluate returned 0x{:X} ({}), disabling for this session", (uint32_t) result,
                      NgxResultName((unsigned int) result));
        }

        // On an evaluation failure, intermediate A/B inputs may still be readable. Restore both persistent
        // ping-pong surfaces to the UAV state the next frame starts from.
        MakeModelWritable(nr.output);
        if (nr.passScratch != nullptr)
            MakeModelWritable(nr.passScratch);

        Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // Failed evaluations leave the game's original image intact. A successful copy-back writes
        // only the active rectangle and restores both resources before DLSS consumes the image.
        // ResidualAcrossRR never copies back -- Color must reach RR untouched.
        FinishColor(result == NVSDK_NGX_Result_Success && finalAnswer != nullptr && !residualAcrossRr);
        if (result == NVSDK_NGX_Result_Success && finalAnswer != nullptr)
            ++nr.successfulDispatches;

        if (gpuTime != nullptr)
        {
            gpuTime->End(cmdList);

            // This path records into the game's own list, so there is no queue of ours to read from.
            // A caller that knows which queue the list goes to says so; otherwise the one the upscaler was
            // invoked on serves. The bridges have to say, because they run on a queue of their own that
            // State never learns about -- a Vulkan game creates no D3D12 swapchain, so nothing ever sets
            // currentCommandQueue and the cost went unreported.
            auto* queue =
                timingQueue != nullptr ? timingQueue : (ID3D12CommandQueue*) ::State::Instance().currentCommandQueue;

            if (queue != nullptr)
            {
                if (auto ms = gpuTime->ReadGpuTime(queue); ms.has_value())
                    lastGpuTime = ms;

                if (ngxTime != nullptr)
                {
                    if (auto ngx = ngxTime->ReadGpuTime(queue); ngx.has_value())
                        lastNgxTime = ngx;
                }

                // The split, once every few hundred frames. What is worth reading is not the total but the
                // remainder: the model's cost is NVIDIA's to set, and everything else is ours.

                if (lastGpuTime.has_value() && lastNgxTime.has_value() && frames - lastSplitLog > 600)
                {
                    lastSplitLog = frames;
                    const double total = lastGpuTime.value();
                    const double ngx = lastNgxTime.value();
                    LOG_INFO("DLSS-NR elapsed: {:.2f} ms total, {:.2f} ms model, {:.2f} ms surrounding work ({:.0f}%; "
                             "intervals may include other GPU work)",
                             total, ngx, total - ngx, total > 0.0 ? 100.0 * (total - ngx) / total : 0.0);
                }
            }
        }

        // Put any guide clones back where the next frame's copy expects to find them.
        // A clone left in NON_PIXEL_SHADER_RESOURCE by a frozen frame was never transitioned back to
        // COPY_DEST, because a frozen frame does not copy. Putting it back unconditionally would be a
        // barrier from a state it is not in, so the frozen case is skipped here and picked up by the
        // first live frame after the toggle goes off -- which is a copy, and copies transition it.
        if (depthIn == nr.depthClone)
            Barrier(cmdList, nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);

        if (motionIn == nr.motionClone)
            Barrier(cmdList, nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);

        if (reduced && nr.colorSmall != nullptr)
            Barrier(cmdList, nr.colorSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // Leave the staging copy as the next frame expects to find it.
        Barrier(cmdList, nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        device->Release();
    }

    std::string DeferredDlssStatus() { return SynchronousDeferredDlssStatus(); }

    void RetryAfterFailure()
    {
        nr.failed = false;
        nr.reason = "";
        nr.reset = true;
    }

    // Consume only the residual produced by this exact CPU evaluate. Dispatch failures
    // leave the RR output untouched, and every exit restores the caller's bindings.
    void ApplyResidualAcrossRr(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params)
    {
        auto* output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
        const bool paired = nr.residualPair.Take(cmdList, params, output);
        const bool ready = nr.residualStoreValid;
        nr.residualStoreValid = false;
        if (!paired || !ready || !output || !nr.residualComposed)
        {
            nr.residualHistoryPrimed = false;
            return;
        }
        const auto& cfg = *Config::Instance();
        const float strength = cfg.DlssNrTransferStrength.value_or_default();
        if (!cfg.DlssNrApplyModel.value_or_default() || !std::isfinite(strength) || strength <= 0.0f)
        {
            nr.residualHistoryPrimed = false;
            return;
        }
        if ((cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default()) &&
            !D3D12Hooks::CanRestoreRootSignature(cmdList))
            return;
        ScopedNrStateEnvelope stateEnvelope(cmdList);
        const auto desc = output->GetDesc();
        const auto carrier = nr.residualComposed->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
            desc.Width != carrier.Width || desc.Height != carrier.Height || desc.Format != carrier.Format ||
            desc.DepthOrArraySize != 1 || desc.MipLevels != 1)
            return;
        auto arrival = cfg.OutputResourceBarrier.has_value() ? (D3D12_RESOURCE_STATES) cfg.OutputResourceBarrier.value()
                                                             : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Barrier(cmdList, output, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmdList, nr.residualComposed, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        DlssNrConstants apply {};
        apply.Mode = DlssNrResidualMode_Apply;
        apply.Width = (unsigned) desc.Width;
        apply.Height = desc.Height;
        apply.TransferStrength = std::clamp(strength, 0.0f, 1.0f);
        const bool composed =
            shader.DispatchResidualPass(cmdList, apply, output, nr.residualHistory[nr.residualHistoryIndex & 1u],
                                        nullptr, nullptr, nr.residualComposed);
        if (composed)
        {
            Barrier(cmdList, nr.residualComposed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cmdList, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyResource(output, nr.residualComposed);
            Barrier(cmdList, output, D3D12_RESOURCE_STATE_COPY_DEST, arrival);
            Barrier(cmdList, nr.residualComposed, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        else
        {
            Barrier(cmdList, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
            Barrier(cmdList, nr.residualComposed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            nr.residualHistoryPrimed = false;
        }
    }

    // Reads the game's parameter block and runs the pass on what it finds.
    //
    // This is the call site's job, not the pass's. A caller that has the resources in hand -- a
    // reprojection stage, a frame generation path, anything that is not the upscaler seam -- calls
    // RunPass directly and never touches an NGX parameter block.
    void EvaluateInternal(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, bool beforeUpscale,
                          ID3D12CommandQueue* timingQueue, bool rayReconstruction, unsigned long long submissionEpoch,
                          bool interop)
    {
        std::lock_guard<std::recursive_mutex> nrLock(mutex);
        const Config& cfg = *Config::Instance();

        const unsigned finishedMode =
            !cfg.DlssNrFinishedPicture.value_or_default()                                             ? 0u
            : (cfg.DlssNrRunBeforeSr.value_or_default() || cfg.DlssNrDeferredDlss.value_or_default()) ? 2u
                                                                                                      : 1u;

        if (lastFinishedMode != finishedMode)
        {
            nr.reset = true;
            if (gpuTime)
                gpuTime->ClearLast();
            if (ngxTime)
                ngxTime->ClearLast();
            lastGpuTime.reset();
            lastNgxTime.reset();
            late.Cancel();
            deferredSr.Cancel();
            lastFinishedMode = finishedMode;
        }
        if (finishedMode)
        {
            nr.residualPair.Cancel();
            nr.residualStoreValid = false;
            nr.residualHistoryPrimed = false;
            if (!cfg.DlssNrEnabled.value_or_default())
            {
                deferredSr.Cancel();
                late.Cancel();
                return;
            }
            if (interop || ::State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
            {
                deferredSr.Cancel();
                late.Cancel();
                late.Say("This option needs a native DirectX 12 game.");
                return;
            }
            if (finishedMode == 2)
            {
                if (rayReconstruction)
                {
                    deferredSr.Cancel();
                    late.Cancel();
                    late.Say("Running the model before SR with this option does not support Ray Reconstruction.");
                    return;
                }
                if (cmdList && params)
                {
                    const auto submitted = ::State::Instance().frameCount;
                    const auto epoch = seamClock.AtSeam(beforeUpscale, false, submitted);
                    if (beforeUpscale)
                        deferredSr.Before(cmdList, params, epoch, submitted, nullptr);
                    else
                        deferredSr.After(cmdList, params, epoch);
                }
            }
            else
            {
                deferredSr.Cancel();
                if (beforeUpscale)
                    late.Capture(cmdList, params, rayReconstruction);
            }
            return;
        }
        late.Cancel();
        if (beforeUpscale)
        {
            // Even a disabled/skipped pre pass must invalidate the previous result.
            if (nr.residualStoreValid)
                nr.residualHistoryPrimed = false;
            nr.residualPair.Cancel();
            nr.residualStoreValid = false;
        }
        const bool residualMode = cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrRunBeforeSr.value_or_default() &&
                                  cfg.DlssNrResidualAcrossRr.value_or_default() && rayReconstruction;
        if (residualMode != nr.residualModeActive)
        {
            nr.residualPair.Cancel();
            nr.residualHistoryPrimed = false;
            nr.residualStoreValid = false;
            nr.residualModeActive = residualMode;
        }
        if (!cfg.DlssNrEnabled.value_or_default() || !cfg.DlssNrDeferredDlss.value_or_default() || rayReconstruction)
        {
            deferredSr.Cancel();
            if (rayReconstruction && cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrDeferredDlss.value_or_default())
                deferredSr.Say("inactive: Ray Reconstruction; using ordinary before/after NR placement");
        }
        else
        {
            if (cmdList != nullptr && params != nullptr)
            {
                const auto submitted = interop ? submissionEpoch : ::State::Instance().frameCount;
                const auto epoch = seamClock.AtSeam(beforeUpscale, interop, submitted);
                if (beforeUpscale)
                    deferredSr.Before(cmdList, params, epoch, submitted, timingQueue);
                else
                    deferredSr.After(cmdList, params, epoch);
            }
            return;
        }

        if (!cfg.DlssNrEnabled.value_or_default())
        {
            ReportSkipOnce("it is switched off");
            return;
        }

        if (cmdList == nullptr || params == nullptr)
        {
            ReportSkipOnce("no command list or no parameter block");
            return;
        }

        // Both SR and RR+SR use the same placement control. Unsupported colour subrects
        // retain the common post-upscale fallback; RR identity only separates history
        // and prevents using the SR-only deferred-residual experiment on an RR feature.
        bool preSrCompatible = true;
        if (cfg.DlssNrRunBeforeSr.value_or_default())
        {
            ID3D12Resource* preColor = GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color");
            unsigned int renderWidth = 0, renderHeight = 0, colorBaseX = 0, colorBaseY = 0;
            params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &renderWidth);
            params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &renderHeight);
            params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &colorBaseX);
            params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &colorBaseY);

            if (preColor == nullptr)
            {
                preSrCompatible = false;
            }
            else
            {
                const D3D12_RESOURCE_DESC colorDesc = preColor->GetDesc();
                const unsigned int allocationWidth = (unsigned int) colorDesc.Width;
                const unsigned int allocationHeight = colorDesc.Height;
                const auto active =
                    DlssNr::PreSrColorExtent(colorDesc, renderWidth, renderHeight, colorBaseX, colorBaseY);
                preSrCompatible = active.has_value();

                if (active && (active->width != allocationWidth || active->height != allocationHeight))
                {

                    if (!reportedPadding)
                    {
                        reportedPadding = true;
                        LOG_INFO("DLSS-NR before SR: staging active {}x{} from padded Color allocation {}x{}; "
                                 "only the active rectangle is copied back. Model size follows active size and "
                                 "WorkingScale.",
                                 active->width, active->height, allocationWidth, allocationHeight);
                    }
                }

                if (!preSrCompatible)
                {

                    if (!warnedSubrect)
                    {
                        warnedSubrect = true;
                        LOG_WARN("DLSS-NR before SR requires a valid origin-zero active rectangle inside a "
                                 "single-sample 2D Color texture; got allocation {}x{}, active {}x{} at {},{}. "
                                 "Falling back after SR.",
                                 allocationWidth, allocationHeight, renderWidth, renderHeight, colorBaseX, colorBaseY);
                    }
                }
            }
        }

        const bool configuredBefore = cfg.DlssNrRunBeforeSr.value_or_default() && preSrCompatible;

        // ResidualAcrossRR: with RunBeforeSR + the game's Ray Reconstruction both on, run
        // the model before SR but leave Color untouched, then add its captured residual back after RR+SR.
        // Unlike every other placement this needs BOTH seams of one evaluate -- the before-upscale seam
        // falls through to the normal pre-SR body (which captures the residual and skips the copy-back),
        // and the after-upscale seam is handled here. If it is requested but the pre-SR path is not
        // compatible, configuredBefore is false and the normal gate runs plain post-SR NR instead.
        const bool residualAcrossRr =
            cfg.DlssNrResidualAcrossRr.value_or_default() && configuredBefore && rayReconstruction;

        if (residualAcrossRr && beforeUpscale &&
            (cfg.DlssNrHoldFrame.value_or_default() || cfg.DlssNrDebugView.value_or_default() != 0 ||
             cfg.DlssNrCompare.value_or_default() != 0 || cfg.DlssNrShowSkinMask.value_or_default()))
        {
            nr.residualHistoryPrimed = false;
            ReportSkipOnce("disable Hold frame, Compare and Debug view for residual-across-RR");
            return;
        }

        if (residualAcrossRr && !beforeUpscale)
        {
            ApplyResidualAcrossRr(cmdList, params);

            if (!announcedResidualAcrossRr)
            {
                announcedResidualAcrossRr = true;
                LOG_INFO("DLSS-NR: residual-across-RR active -- MV-reprojected accumulator, blend {:.2f}",
                         std::clamp(cfg.DlssNrResidualAcrossRrBlend.value_or_default(), 0.01f, 1.0f));
            }
            return;
        }

        if (configuredBefore != beforeUpscale)
            return;

        // Which of the game's APIs this evaluate arrived through.
        //
        // Says out loud what was previously only reasoned about: an FSR or XeSS title reaches this pass
        // transitively, because those shims call OptiScaler's own NVSDK_NGX_D3D12_EvaluateFeature and
        // this pass hangs off that. Nothing needed adding to the shims -- a call there would run the
        // model twice -- but "nothing needed adding" is a claim, and this is the line that checks it.
        {

            const ApiUpscalerInput api = ::State::Instance().currentInputApiName;

            if (saidApi != api)
            {
                saidApi = api;
                LOG_INFO("DLSS-NR reached through the game's {} input", ApiUpscalerInputName(api));
            }
        }

        ID3D12Resource* output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
        ID3D12Resource* target = beforeUpscale ? GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color") : output;
        ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
        ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

        // Without all three there is nothing to run on. This is not a failure -- some evaluates legitimately
        // carry none of it -- so it stays quiet and tries again next frame.
        if (target == nullptr || depth == nullptr || motion == nullptr)
        {
            ReportSkipOnce(target == nullptr  ? (beforeUpscale ? "the parameters carried no color texture"
                                                               : "the parameters carried no output texture")
                           : depth == nullptr ? "the parameters carried no depth"
                                              : "the parameters carried no motion vectors");
            return;
        }

        unsigned int createFlags = featureFlags;
        if (!createFlags)
            params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);

        DlssNrFrameInfo frame {};
        frame.DepthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
        frame.MotionVectorsLowResolution = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
        if (output != nullptr)
        {
            frame.OutputWidth = (unsigned int) output->GetDesc().Width;
            frame.OutputHeight = output->GetDesc().Height;
        }
        unsigned int outputWidth = 0, outputHeight = 0;
        params->Get(NVSDK_NGX_Parameter_OutWidth, &outputWidth);
        params->Get(NVSDK_NGX_Parameter_OutHeight, &outputHeight);
        if (outputWidth && outputHeight)
        {
            frame.OutputWidth = outputWidth;
            frame.OutputHeight = outputHeight;
        }
        frame.BeforeUpscale = beforeUpscale;
        frame.RayReconstruction = rayReconstruction;
        frame.ResidualAcrossRr = residualAcrossRr; // only reaches here on the before-upscale seam
        if (residualAcrossRr)
        {
            if (!output)
                return;
            const auto outDesc = output->GetDesc();
            if (outDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || outDesc.SampleDesc.Count != 1 ||
                outDesc.DepthOrArraySize != 1 || outDesc.MipLevels != 1)
                return;
            if (nr.residualOutputWidth != outDesc.Width || nr.residualOutputHeight != outDesc.Height ||
                nr.residualOutputFormat != outDesc.Format)
            {
                ParkNrResource(nr.residualComposed);
                nr.residualHistoryPrimed = false;
                nr.residualOutputWidth = (unsigned) outDesc.Width;
                nr.residualOutputHeight = outDesc.Height;
                nr.residualOutputFormat = outDesc.Format;
            }
        }
        frame.SubmissionEpoch = interop ? submissionEpoch : ::State::Instance().frameCount;

        // Color and Output may use different formats even though DLSS treats them as the same frame colour
        // space. Output is the stable authority across injection points; target is only a fallback for a
        // malformed parameter block.
        ID3D12Resource* colourAuthority = output != nullptr ? output : target;
        frame.ColourIsLinearHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0 &&
                                  colourAuthority != nullptr &&
                                  FormatCanHoldLinearHdr(colourAuthority->GetDesc().Format);

        // The game telling the upscaler to forget everything it has accumulated: a cut, a teleport, a
        // load. Every upscaler in this tree reads it and this pass did not, so the model's history was
        // only ever reset by things that happened to us -- a resize, a rebuild, a recovery from failure
        // -- and never by anything that happened in the game. Across a cut the model was reprojecting
        // the previous scene onto the new one and being asked to reconcile them.
        //
        // Read the same way FFXFeature_Dx12 reads it, including leaving it alone when the parameter is
        // absent: a game that never sets it is not asking for a reset every frame.
        {
            unsigned int gameReset = 0;

            if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success)
                frame.Reset = gameReset != 0;
        }

        // How much of the guides is real. See DlssNrFrameInfo -- zero means the game did not say.
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth);
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight);
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &frame.DepthSubrectBaseX);
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &frame.DepthSubrectBaseY);
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &frame.MotionSubrectBaseX);
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &frame.MotionSubrectBaseY);

        if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX) != NVSDK_NGX_Result_Success)
            frame.MvScaleX = 1.0f;

        if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY) != NVSDK_NGX_Result_Success)
            frame.MvScaleY = 1.0f;

        // What the game says about its own exposure. Logged, used for nothing yet.
        //
        // The white point measured from the frame turned out to be a control loop rather than a
        // measurement: the pass writes into the buffer it reads, most games adapt their exposure to the
        // finished frame, and the two chase each other -- 0.01 to 97.9 in one Enshrouded session. Any
        // statistic taken from a frame we modify has that problem.
        //
        // These do not. DLSS.Pre.Exposure is the scale the game applied before handing the buffer over,
        // and ExposureTexture is a 1x1 the game fills with the exposure it is using; both are the game's
        // own numbers, decided upstream of anything here. Whether either is close to the divisor the model
        // actually wants is unknown, which is why this only prints them.
        //
        // The auto-exposure flag decides whether the texture means anything: with it set the game is
        // telling DLSS to work exposure out for itself and may supply nothing. OptiScaler forces that flag
        // on for eighteen games, so it is logged too -- reading a value whose flag has been overridden is
        // how the debug views lied earlier tonight.
        {
            float preExposure = 0.0f;
            const bool havePre =
                params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;

            void* exposureTex = nullptr;
            params->Get(NVSDK_NGX_Parameter_ExposureTexture, &exposureTex);

            frame.ExposureTexture = exposureTex;
            frame.PreExposure = havePre && preExposure > 1e-6f ? preExposure : 1.0f;

            nr.exposureOfferedNow = exposureTex != nullptr;
            nr.exposureEverOffered = nr.exposureEverOffered || nr.exposureOfferedNow;
            nr.exposureFrames++;

            const bool autoExposureFlag = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0;

            const ExposureReport now { true, havePre ? preExposure : 0.0f, havePre, exposureTex != nullptr,
                                       autoExposureFlag };

            if (!logged.valid || logged.havePre != now.havePre || logged.haveTexture != now.haveTexture ||
                logged.autoFlag != now.autoFlag ||
                std::abs(logged.pre - now.pre) > std::max(0.01f * std::abs(now.pre), 1e-4f))
            {
                logged = now;
                LOG_INFO("DLSS-NR exposure from the game: DLSS.Pre.Exposure {}, ExposureTexture {}, "
                         "auto-exposure flag {}",
                         now.havePre ? std::to_string(now.pre) : std::string("not supplied"),
                         now.haveTexture ? "supplied" : "not supplied", now.autoFlag ? "set" : "clear");
            }

            // The value itself, once it has come back off the GPU. Separate from the line above because
            // that one says what the game offers and this one says what it actually reads -- and because
            // the reading arrives three frames after the offer.

            if (nr.gameExposure > 1e-6f &&
                std::abs(loggedExposure - nr.gameExposure) > std::max(0.02f * nr.gameExposure, 1e-5f))
            {
                loggedExposure = nr.gameExposure;
                LOG_INFO("DLSS-NR game exposure {:.5f} (pre-exposure {:.3f}) -> white point would be {:.2f}",
                         nr.gameExposure, nr.gamePreExposure, nr.gamePreExposure / nr.gameExposure);
            }

            // The scan's number, on the same cadence, so one log carries both.
            //
            // This is the whole validation. In a game that hands over an exposure texture there is a
            // known-correct value; if the scan's candidate tracks it, the scan found the right buffer
            // rather than merely a moving one, and can be trusted where a game hands over nothing.
            // Comparing two numbers after the fact needs both written down, and until now the scan's
            // value existed only in a menu nobody can read while playing.
            {
                int which = 0;
                float low = 0.0f, high = 0.0f;
                const float scanned = DlssNr::ExposureScan::BestValue(&which, &low, &high);

                if (scanned > 0.0f && std::abs(loggedScan - scanned) > std::max(0.02f * scanned, 1e-6f))
                {
                    loggedScan = scanned;

                    if (nr.gameExposure > 1e-6f)
                        LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  the "
                                 "game's own exposure is {:.5f}  |  ratio {:.4f}",
                                 which, scanned, low, high, nr.gameExposure, scanned / nr.gameExposure);
                    else
                        LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  this "
                                 "game supplies no exposure to compare against",
                                 which, scanned, low, high);
                }
            }
        }

        // The upscaler's inputs are at render resolution while colour and output are at display
        // resolution; the model takes that as a subrect per resource, which the pass reads from the
        // resources themselves.
        ID3D12Device* device = nullptr;

        if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        {
            ReportSkipOnce("the output texture belongs to no D3D12 device");
            return;
        }

        // The pass is the object, so the caller holds it. Built once, on the device the frame is on.

        device->Release();

        Run(cmdList, target, depth, motion, target, frame, timingQueue);
        if (residualAcrossRr && nr.residualStoreValid)
            nr.residualPair.Arm(cmdList, params, output);
    }

    // The pass. Resources in, nothing read from anywhere the caller cannot see.

    void ProbeD3D11(void* d3d11Device)
    {

        bool dx11Probed = false;

        if (done || d3d11Device == nullptr)
            return;

        // Every other entry point in this file takes the lock before touching nr; this one was reaching
        // EnsureForwarder without it.
        std::lock_guard<std::recursive_mutex> nrLock(mutex);

        // Opt in only. See the note on DlssNrProbeD3D11: this is the one call in the pass that reaches
        // into a subsystem on the game's own device rather than reading something we already hold.
        if (!Config::Instance()->DlssNrProbeD3D11.value_or_default())
            return;

        dx11Probed = true;

        if (!EnsureForwarder())
            return;

        auto probe = (int (*)(const wchar_t*)) GetProcAddress(nr.forwarder, "dlssnr_d3d11_probe");
        auto init = (int (*)(const wchar_t*, const wchar_t*, void*, int, int*, int*)) GetProcAddress(
            nr.forwarder, "dlssnr_d3d11_init");

        if (probe == nullptr || init == nullptr)
        {
            LOG_INFO("DLSS-NR D3D11: this forwarder has no D3D11 probe");
            return;
        }

        auto snippet = Util::FindFilePath(dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            return;

        // Four bits, one per entry point: init 1, create 2, evaluate 4, release 8.
        const int bits = probe(snippet->wstring().c_str());

        // And the question NGX has an API for. Asked first because it creates nothing: if the feature
        // declines D3D11 here, that is the feature's own answer rather than our reading of a failed init.
        auto requirements = (int (*)(const wchar_t*, void*, unsigned int*, unsigned int*,
                                     unsigned int*)) GetProcAddress(nr.forwarder, "dlssnr_d3d11_requirements");

        if (requirements != nullptr)
        {
            // The adapter the game is actually running on. Without it the query answers
            // AdapterUnsupported, which looks like a verdict on the hardware and is really a verdict on
            // the question -- that is what the first attempt got, on a 5080.
            IDXGIAdapter* adapter = nullptr;
            IDXGIFactory1* factory = nullptr;

            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory != nullptr)
                factory->EnumAdapters(0, &adapter);

            unsigned int supported = 0xFFFFFFFFu;
            unsigned int minArch = 0;
            unsigned int minOs = 0;
            const int rc = requirements(snippet->wstring().c_str(), adapter, &supported, &minArch, &minOs);

            const char* meaning = supported == 0     ? "SUPPORTED"
                                  : (supported & 16) ? "NotImplemented -- the feature has no D3D11 path"
                                  : (supported & 4)  ? "AdapterUnsupported"
                                  : (supported & 2)  ? "DriverVersionUnsupported"
                                  : (supported & 8)  ? "OSVersionBelowMinimum"
                                  : (supported & 1)  ? "CheckNotPresent"
                                                     : "unknown";

            LOG_WARN("DLSS-NR D3D11: GetFeatureRequirements {} ({}), FeatureSupported 0x{:X} -- {}. "
                     "minimum architecture 0x{:X}, minimum OS 0x{:X}",
                     rc, NgxResultName((unsigned int) rc), supported, meaning, minArch, minOs);

            if (adapter != nullptr)
                adapter->Release();

            if (factory != nullptr)
                factory->Release();
        }

        LOG_INFO("DLSS-NR D3D11: entry points resolved {}/15 (init {}, create {}, evaluate {}, release {})", bits,
                 (bits & 1) ? "yes" : "no", (bits & 2) ? "yes" : "no", (bits & 4) ? "yes" : "no",
                 (bits & 8) ? "yes" : "no");

        if (bits != 15)
        {
            LOG_INFO("DLSS-NR D3D11: incomplete surface, the bridge stays the only route");
            return;
        }

        // Four ways of asking, since the feature has already said it supports this platform.
        int attempt = 0;
        int results[4] = { -9, -9, -9, -9 };

        const int result = init(snippet->wstring().c_str(), ::State::Instance().NVNGX_ApplicationDataPath.c_str(),
                                d3d11Device, 0x0000015, &attempt, results);

        static const char* kNames[4] = { "Init_Ext on our own copy", "Init on our own copy",
                                         "Init_Ext on the shared module", "Init on the shared module" };

        for (int i = 0; i < 4; ++i)
        {
            LOG_INFO("DLSS-NR D3D11:   {} -> {} ({})", kNames[i], results[i],
                     results[i] == -2   ? "module not loaded"
                     : results[i] == -3 ? "export missing"
                     : results[i] == -9 ? "not reached"
                                        : NgxResultName((unsigned int) results[i]));
        }

        if (result == 1)
            LOG_WARN("DLSS-NR D3D11: initialised, via {}. The feature already said this platform is "
                     "supported; now the call works too. Next is a feature create on a device context.",
                     attempt > 0 ? kNames[attempt - 1] : "?");
        else
            // Deliberately not "so the bridge is required". GetFeatureRequirements answers 0x0 SUPPORTED
            // with a minimum architecture this card meets, so the platform is not the obstacle and saying
            // otherwise here would be printing a conclusion the evidence does not carry.
            LOG_WARN("DLSS-NR D3D11: every init variant refused, last {} ({}) -- though the feature itself "
                     "reports this platform as supported, so the obstacle is in how it is being called",
                     result, NgxResultName((unsigned int) result));
    }

    DlssNr::CalibrationReading Calibration()
    {
        CalibrationReading r {};
        r.suggestion = nr.calibSuggestion;
        r.steadiness = nr.calibSteadiness;
        r.samples = nr.calibCount;
        r.usable = nr.calibUsable;
        r.why = nr.calibWhy;
        return r;
    }

    // What the game offers by way of exposure, and what has been read from it. For the menu, so a user
    // can see whether this game supplies one at all without having to read a log.

    void ReleaseResources()
    {
        std::lock_guard<std::recursive_mutex> nrLock(mutex);
        deferredSr.ReleaseResources();

        for (auto& r : retired)
        {
            if (r.feature != nullptr && nr.release != nullptr)
                nr.release(r.feature);

            if (r.resource != nullptr)
                r.resource->Release();
        }

        retired.clear();

        if (nr.feature != nullptr && nr.release != nullptr)
            nr.release(nr.feature);

        nr.feature = nullptr;
        nr.featurePendingSubmission = false;

        for (unsigned int pass = 1; pass < DlssNr::MaxPassCount; ++pass)
        {
            void*& f = nr.passFeature[pass];
            if (f != nullptr && nr.release != nullptr)
                nr.release(f);

            f = nullptr;
            nr.passNeedsReset[pass] = false;
            nr.passCreateFailed[pass] = false;
            nr.passPendingSubmission[pass] = false;
        }

        if (nr.output != nullptr)
        {
            nr.output->Release();
            nr.output = nullptr;
        }

        if (nr.passScratch != nullptr)
        {
            nr.passScratch->Release();
            nr.passScratch = nullptr;
        }
        nr.passScratchFailed = false;

        if (nr.colorCopy != nullptr)
        {
            nr.colorCopy->Release();
            nr.colorCopy = nullptr;
        }

        if (nr.hdrCopy != nullptr)
        {
            nr.hdrCopy->Release();
            nr.hdrCopy = nullptr;
        }

        if (nr.activeColor != nullptr)
        {
            nr.activeColor->Release();
            nr.activeColor = nullptr;
        }

        if (nr.colorSmall != nullptr)
        {
            nr.colorSmall->Release();
            nr.colorSmall = nullptr;
        }

        if (nr.superUp != nullptr)
        {
            delete nr.superUp;
            nr.superUp = nullptr;
        }

        if (nr.superDown != nullptr)
        {
            delete nr.superDown;
            nr.superDown = nullptr;
        }

        for (ID3D12Resource** r :
             { &nr.residualEdited, &nr.residualHistory[0], &nr.residualHistory[1], &nr.residualComposed })
            if (*r != nullptr)
            {
                (*r)->Release();
                *r = nullptr;
            }
        nr.residualPair.Cancel();
        nr.residualStoreValid = false;
        nr.residualHistoryPrimed = false;
        nr.residualHistoryIndex = 0;

        if (nr.outputNative != nullptr)
        {
            nr.outputNative->Release();
            nr.outputNative = nullptr;
        }

        if (nr.heldColor != nullptr)
        {
            nr.heldColor->Release();
            nr.heldColor = nullptr;
        }
        nr.heldActive = false;

        if (nr.meter != nullptr)
        {
            nr.meter->Release();
            nr.meter = nullptr;
        }

        if (nr.calib != nullptr)
        {
            nr.calib->Release();
            nr.calib = nullptr;
        }

        for (auto& r : nr.calibReadback)
        {
            if (r != nullptr)
            {
                r->Release();
                r = nullptr;
            }
        }

        nr.calibFrames = 0;
        nr.calibCount = 0;
        nr.calibSuggestion = 0.0f;
        nr.calibSteadiness = 0.0f;
        nr.calibUsable = false;
        nr.calibWhy = "measuring...";

        for (auto& rb : nr.meterReadback)
        {
            if (rb != nullptr)
            {
                rb->Release();
                rb = nullptr;
            }
        }

        // The slots these flags describe have just been released, so nothing may vouch for what the next
        // buffers happen to contain. gameExposure is deliberately NOT cleared here: a recreate is a
        // transition within the same scene, and dropping to the slider for a few frames would be the
        // flicker the held value exists to prevent. The user switching the option off is the case where
        // the held value has to go, and that is handled at the edge in Dispatch.
        for (bool& valid : nr.meterExposureValid)
            valid = false;

        nr.meterFrames = 0;

        if (nr.depthClone != nullptr)
        {
            nr.depthClone->Release();
            nr.depthClone = nullptr;
        }

        if (nr.motionClone != nullptr)
        {
            nr.motionClone->Release();
            nr.motionClone = nullptr;
        }

        captureFrames.release();
        gpuTime.reset();
        ngxTime.reset();
        lastNgxTime.reset();
        lastGpuTime.reset();
    }

    struct GuideReport
    {
        bool valid;
        bool depthInverted;
        float mvScaleX;
        float mvScaleY;
        unsigned int guideW;
        unsigned int guideH;
        unsigned int frameW;
        unsigned int frameH;
    };
    GuideReport loggedGuides {};
    struct ComposeReport
    {
        bool valid;
        float whitePoint;
        float transfer;
        float colour;
        float maxRatio;
        unsigned int passthrough;
        unsigned int debugView;
        unsigned int compareMode;
        unsigned int residual;
        unsigned int workW;
        unsigned int workH;
        unsigned int passes;
    };
    ComposeReport loggedCompose {};

    struct ExposureReport
    {
        bool valid;
        float pre;
        bool havePre;
        bool haveTexture;
        bool autoFlag;
    };
    ExposureReport logged {};
    bool done = false;
    bool dx11Probed = false;
    std::set<std::string> seen;
    unsigned long long resets = 0;
    bool warnedProxyPasses = false;
    bool reportedHdr = false;
    bool reportedHdrValue = false;
    bool reportedBefore = false;
    bool warnedSuper = false;
    unsigned int loggedConfigured = 0;
    unsigned int loggedEffective = 0;
    unsigned int lastSuper = 0;
    bool tuningReported = false;
    unsigned long long lastSplitLog = 0;
    unsigned lastFinishedMode = 0;
    bool reportedPadding = false;
    bool warnedSubrect = false;
    bool announcedResidualAcrossRr = false;
    ApiUpscalerInput saidApi = (ApiUpscalerInput) -1;
    float loggedExposure = -1.0f;
    float loggedScan = -1.0f;

    bool usingProxy = false, proxyRunning = false;
    DlssNr::Proxy::Context proxy;
    ID3D12Resource* buffer = nullptr;
    D3D12_RESOURCE_STATES bufferState = D3D12_RESOURCE_STATE_COMMON;
    uint32_t featureFlags = 0;
    DlssNr::ControlRequests controls = DlssNr::ReadControlRequests();
    explicit State(DlssNr_Dx12& owner) : shader(owner) {}
    void ConsumeControls()
    {
        const auto requested = DlssNr::ReadControlRequests();
        if (requested.retryGeneration != controls.retryGeneration)
        {
            proxy.RetryAfterFailure();
            RetryAfterFailure();
        }
        if (requested.captureGeneration != controls.captureGeneration)
            captureFrames.request(requested.captureFrames);
        controls = requested;
    }
    void Publish()
    {
        DlssNr::PublishStatus(
            &shader, DlssNr::Backend::Dx12,
            { !nr.failed && (usingProxy ? proxyRunning : nr.feature != nullptr),
              nr.failed ? nr.reason : "",
              lastGpuTime,
              frames,
              { nr.exposureFrames, nr.exposureOfferedNow, nr.exposureEverOffered, nr.gameExposure, nr.gamePreExposure },
              captureFrames.isActive() });
    }
    ~State()
    {
        WaitForFinishedPicture();
        ReleaseResources();
        proxy.Release();
        SAFE_RELEASE(buffer);
        if (nr.capabilityParams && NVNGXProxy::D3D12_DestroyParameters())
            NVNGXProxy::D3D12_DestroyParameters()(nr.capabilityParams);
        nr.capabilityParams = nullptr;
        if (nr.forwarder)
            FreeLibrary(nr.forwarder);
    }
};

// ---------------------------------------------------------------------------------------------
// The pass itself. Everything above is what it is made of; everything below is the shape the rest
// of OptiScaler sees.
// ---------------------------------------------------------------------------------------------

DlssNr_Dx12::DlssNr_Dx12(std::string InName, ID3D12Device* InDevice)
    : Shader_Dx12(InName, InDevice), _state(std::make_unique<State>(*this))
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    // Five inputs, two outputs, one constant buffer, and a clamped linear sampler.
    //
    // The sampler exists because the model may be run below full resolution, in which case its answer
    // has to be read back at a different size from the frame it is being transferred onto.
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    if (!SetupRootSignature(InDevice, kSrvCount, kUavCount, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DlssNrConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (uint32_t i = 0; i < DLSSNR_NUM_OF_HEAPS; ++i)
    {
        auto result = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&_constantBuffers[i]));

        if (result != S_OK)
        {
            LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
            return;
        }
    }

    // Precompiled, with no source fallback. The shader used to be compiled at runtime from a string,
    // which would have meant no shader at all for anyone leaving UsePrecompiledShaders at its
    // default.
    if (!CreateComputePipeline(InDevice, &_pipelineState, DlssNr_cso, sizeof(DlssNr_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }

    // Second PSO for the ResidualAcrossRR v2 accumulator (its own blob, same root signature).
    // A failure here is not fatal to the class -- only that experimental mode goes unavailable.
    if (!CreateComputePipeline(InDevice, &_residualPipelineState, dlssnr_residual_cso, sizeof(dlssnr_residual_cso),
                               nullptr))
    {
        _residualPipelineState = nullptr;
        LOG_WARN("[{0}] ResidualAcrossRR compute pipeline unavailable", _name);
    }

    _init = InitHeaps(InDevice, _frameHeaps, DLSSNR_NUM_OF_HEAPS);
}

bool DlssNr_Dx12::DispatchPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                               ID3D12Resource* InSource, ID3D12Resource* InModel, ID3D12Resource* InOriginal,
                               ID3D12Resource* InMotion, ID3D12Resource* InPrevEdit, ID3D12Resource* OutTarget,
                               ID3D12Resource* OutKeep)
{
    if (!_init || InCmdList == nullptr || _device == nullptr || InSource == nullptr || OutTarget == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    // Every slot in the table gets a view, whether the mode reads it or not. An unbound descriptor is
    // not an empty read; it is a read from nothing, and the source stands in wherever a mode has
    // nothing of its own to put there.
    ID3D12Resource* const srvs[kSrvCount] = {
        InSource,
        InModel != nullptr ? InModel : InSource,
        InOriginal != nullptr ? InOriginal : InSource,
        InMotion != nullptr ? InMotion : InSource,
        InPrevEdit != nullptr ? InPrevEdit : InSource,
    };

    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, srvs[i], currentHeap.GetSrvCPU(i));

    ID3D12Resource* const uavs[kUavCount] = {
        OutTarget,
        OutKeep != nullptr ? OutKeep : OutTarget,
    };

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    // Sized from the constants rather than from a resource, because the pass that shrinks the proxy
    // writes fewer pixels than its source has.
    const UINT dispatchWidth = (InConstants.Width + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (InConstants.Height + _numThreadsY - 1) / _numThreadsY;
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

DlssNr_Dx12::~DlssNr_Dx12()
{
    std::lock_guard lock(nrOwnersMutex);
    std::erase(nrOwners, this);
    if (activeNrOwner == this)
        activeNrOwner = nullptr;
    DlssNr::ClearStatus(this);
    _state.reset();
    for (auto& heap : _frameHeaps)
        heap.ReleaseHeaps();
    if (_finishedColorPipelineState)
        _finishedColorPipelineState->Release();
    for (auto& buffer : _constantBuffers)
    {
        if (buffer != nullptr)
        {
            buffer->Release();
            buffer = nullptr;
        }
    }

    if (_residualPipelineState != nullptr)
    {
        _residualPipelineState->Release();
        _residualPipelineState = nullptr;
    }
}

bool DlssNr_Dx12::DispatchResidualPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                                       ID3D12Resource* InSource, ID3D12Resource* InModel, ID3D12Resource* InOriginal,
                                       ID3D12Resource* InMotion, ID3D12Resource* OutTarget, bool finishedColor)
{
    if (finishedColor && !_finishedColorPipelineState && _init)
        CreateComputePipeline(_device, &_finishedColorPipelineState, dlssnr_finished_color_cso,
                              sizeof(dlssnr_finished_color_cso), nullptr);
    auto* pipeline = finishedColor ? _finishedColorPipelineState : _residualPipelineState;
    if (!_init || pipeline == nullptr || InCmdList == nullptr || _device == nullptr || InSource == nullptr ||
        OutTarget == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    // Same table shape as DispatchPass: the residual shader reads t0..t3 + u0, and t4/u1 get the
    // source as a stand-in so no descriptor in the table is left unbound.
    ID3D12Resource* const srvs[kSrvCount] = {
        InSource,
        InModel != nullptr ? InModel : InSource,
        InOriginal != nullptr ? InOriginal : InSource,
        InMotion != nullptr ? InMotion : InSource,
        InSource,
    };

    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, srvs[i], currentHeap.GetSrvCPU(i));

    ID3D12Resource* const uavs[kUavCount] = { OutTarget, OutTarget };

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(pipeline);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    const UINT dispatchWidth = (InConstants.Width + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (InConstants.Height + _numThreadsY - 1) / _numThreadsY;
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

bool DlssNr_Dx12::CreateBufferResource(ID3D12Device* device, ID3D12Resource* source, D3D12_RESOURCE_STATES state)
{
    if (device == nullptr || source == nullptr)
        return false;
    auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1)
        return false;
    desc.Flags = (desc.Flags | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) & ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    if (_state->buffer != nullptr)
    {
        const auto previous = _state->buffer->GetDesc();
        if (previous.Width == desc.Width && previous.Height == desc.Height && previous.Format == desc.Format &&
            previous.Flags == desc.Flags)
            return true;
        _state->ParkNrResource(_state->buffer);
    }
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&_state->buffer))))
        return false;
    _state->bufferState = state;
    return true;
}

void DlssNr_Dx12::SetBufferState(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES state)
{
    Shader_Dx12::SetBufferState(cmdList, state, _state->buffer, &_state->bufferState);
}

ID3D12Resource* DlssNr_Dx12::Buffer() { return _state->buffer; }
bool DlssNr_Dx12::CanRender() const { return _init && _state->buffer != nullptr; }

bool DlssNr_Dx12::Dispatch(ID3D12GraphicsCommandList* cmd, ID3D12Resource* colour, ID3D12Resource* depth,
                           ID3D12Resource* motion, ID3D12Resource* output, const DlssNrFrameInfo& frame,
                           ID3D12CommandQueue* queue)
{
    std::lock_guard ownersLock(nrOwnersMutex);
    ActivateNrOwner(this);
    std::lock_guard stateLock(_state->mutex);
    _state->ConsumeControls();
    struct Publish
    {
        State& s;
        ~Publish() { s.Publish(); }
    } publish { *_state };
    if (!_init || !cmd || !colour || !depth || !motion || !output)
        return false;
    auto info = frame;
    info.PipelineManagedStates = true;
    info.PrivateColorCopy = true;
    if (!info.RenderSubrectWidth)
        info.RenderSubrectWidth = info.Width;
    if (!info.RenderSubrectHeight)
        info.RenderSubrectHeight = info.Height;
    if (colour != output)
    {
        const auto source = colour->GetDesc(), target = output->GetDesc();
        if (source.Width != target.Width || source.Height != target.Height || source.Format != target.Format)
            return false;
        _state->Barrier(cmd, colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        _state->Barrier(cmd, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(output, colour);
        _state->Barrier(cmd, colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        _state->Barrier(cmd, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    _state->nr.exposureOfferedNow = info.ExposureTexture != nullptr;
    _state->nr.exposureEverOffered |= _state->nr.exposureOfferedNow;
    ++_state->nr.exposureFrames;
    const auto before = _state->nr.successfulDispatches;
    _state->Run(cmd, output, depth, motion, output, info, queue);
    return _state->nr.successfulDispatches != before;
}

bool DlssNr_Dx12::ProcessSeam(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params, bool beforeUpscale,
                              ID3D12CommandQueue* queue, bool rayReconstruction, unsigned long long submissionEpoch,
                              bool interop, uint32_t featureFlags)
{
    std::lock_guard ownersLock(nrOwnersMutex);
    ActivateNrOwner(this);
    std::lock_guard stateLock(_state->mutex);
    _state->ConsumeControls();
    _state->featureFlags = featureFlags;
    const auto& cfg = *Config::Instance();
    // Both seams reach this scheduler; ordinary passes remain in the shared shader pipeline.
    const bool special = cfg.DlssNrFinishedPicture.value_or_default() ||
                         (!rayReconstruction && cfg.DlssNrDeferredDlss.value_or_default()) ||
                         (rayReconstruction && cfg.DlssNrRunBeforeSr.value_or_default() &&
                          cfg.DlssNrResidualAcrossRr.value_or_default());
    if (special)
        _state->EvaluateInternal(cmd, params, beforeUpscale, queue, rayReconstruction, submissionEpoch, interop);
    else
    {
        if (_state->lastFinishedMode != 0)
        {
            _state->lastFinishedMode = 0;
            _state->nr.reset = true;
            if (_state->gpuTime)
                _state->gpuTime->ClearLast();
            if (_state->ngxTime)
                _state->ngxTime->ClearLast();
            _state->lastGpuTime.reset();
            _state->lastNgxTime.reset();
        }
        _state->late.Cancel();
        _state->deferredSr.Cancel();
        _state->nr.residualPair.Cancel();
        _state->nr.residualStoreValid = false;
        _state->nr.residualHistoryPrimed = false;
    }
    _state->Publish();
    return special;
}
void DlssNr_Dx12::ResetFinishedCommands(ID3D12CommandList* cmd) { _state->FinishedPictureResetCommandList(cmd); }
void DlssNr_Dx12::SubmitFinishedCommands(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    _state->FinishedPictureSubmitted(queue, count, lists);
}
bool DlssNr_Dx12::WaitFinished() { return _state->WaitForFinishedPicture(); }
void DlssNr_Dx12::ApplyFinished(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue)
{
    _state->ApplyToFinishedPicture(swapchain, queue);
    _state->Publish();
}
std::string DlssNr_Dx12::FinishedStatus() { return _state->FinishedPictureStatus(); }
std::string DlssNr_Dx12::DeferredStatus() { return _state->DeferredDlssStatus(); }
DlssNr::CalibrationReading DlssNr_Dx12::CalibrationStatus()
{
    std::lock_guard lock(_state->mutex);
    return _state->Calibration();
}
void DlssNr_Dx12::ProbeDx11(void* device)
{
    std::lock_guard lock(_state->mutex);
    _state->ProbeD3D11(device);
}

namespace DlssNr
{
void FinishedPictureResetCommandList(ID3D12CommandList* cmd)
{
    std::lock_guard lock(nrOwnersMutex);
    for (auto* owner : nrOwners)
        owner->ResetFinishedCommands(cmd);
}
void FinishedPictureSubmitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    std::lock_guard lock(nrOwnersMutex);
    for (auto* owner : nrOwners)
        owner->SubmitFinishedCommands(queue, count, lists);
}
bool WaitForFinishedPicture()
{
    std::lock_guard lock(nrOwnersMutex);
    bool ready = true;
    for (auto* owner : nrOwners)
        ready = owner->WaitFinished() && ready;
    return ready;
}
void ApplyToFinishedPicture(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue)
{
    std::lock_guard lock(nrOwnersMutex);
    if (activeNrOwner)
        activeNrOwner->ApplyFinished(swapchain, queue);
}
void FinishedPictureColorSpace(IDXGISwapChain* swapchain, DXGI_COLOR_SPACE_TYPE colorSpace)
{
    static constexpr GUID key = { 0x34a31e7b, 0x84c5, 0x44ef, { 0xa7, 0x4d, 0x6b, 0xd3, 0x60, 0x8c, 0xe5, 0x22 } };
    if (swapchain)
        swapchain->SetPrivateData(key, sizeof(colorSpace), &colorSpace);
}
std::string FinishedPictureStatus()
{
    std::lock_guard lock(nrOwnersMutex);
    return activeNrOwner ? activeNrOwner->FinishedStatus() : "Waiting for a finished picture.";
}
std::string DeferredDlssStatus()
{
    std::lock_guard lock(nrOwnersMutex);
    return activeNrOwner ? activeNrOwner->DeferredStatus() : "not started";
}
CalibrationReading Calibration()
{
    std::lock_guard lock(nrOwnersMutex);
    return activeNrOwner ? activeNrOwner->CalibrationStatus() : CalibrationReading {};
}
void ProbeD3D11(void* device)
{
    std::lock_guard lock(nrOwnersMutex);
    if (activeNrOwner)
        activeNrOwner->ProbeDx11(device);
}
void Shutdown() { WaitForFinishedPicture(); }
} // namespace DlssNr
