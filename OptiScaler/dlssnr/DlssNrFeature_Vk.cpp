#include "pch.h"

#include "DlssNrFeature_Vk.h"
#include "DlssNrFeature_Dx12.h"
#include "DlssNr_Status.h"
#include <nvsdk_ngx_vk.h>
#include "PassProfiles.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <NVNGX_Parameter.h>

#include <shaders/dlssnr/DlssNr_Vk.h>
#include <shaders/dlssnr/DlssNr_Guides.h>
#include <shaders/output_scaling/OS_Vk.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace DlssNr
{

// The forwarder's Vulkan surface. The model checks its caller's module path and requires nvngx.dll in
// it, whichever API is being used, so these calls go through the same shim the D3D12 path does.
using PFN_VkProbe = int(__cdecl*)(const wchar_t*);
using PFN_VkInit = int(__cdecl*)(const wchar_t*, const wchar_t*, void*, void*, void*, int);
using PFN_VkCreate = void*(__cdecl*) (void*, void*, unsigned int, unsigned int, int, float, int, float, float, float,
                                      int, int);
using PFN_VkEvaluate = int(__cdecl*)(void*, void*, void*, void*, void*, void*, void*, unsigned int, unsigned int,
                                     unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,
                                     unsigned int, unsigned int, int, int, float, int, float, float, float, int, float,
                                     float);
using PFN_VkRelease = void(__cdecl*)(void*);

// One image this pass owns: the storage, the view, and the NGX wrapper that describes it. Kept
// together because they are created, resized and destroyed as one thing.
struct OwnedImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    NVSDK_NGX_Resource_VK ngx {};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;

    bool Valid() const { return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE; }
};

struct VkState
{
    bool failed = false;
    const char* reason = "";

    HMODULE forwarder = nullptr;
    PFN_VkProbe probe = nullptr;
    PFN_VkInit init = nullptr;
    PFN_VkCreate create = nullptr;
    PFN_VkEvaluate evaluate = nullptr;
    PFN_VkRelease release = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    bool ngxInitialised = false;
    void* feature = nullptr;
    void* laterFeatures[DlssNr::MaxPassCount] {};
    Profiles::NrPassTuning builtTuning[DlssNr::MaxPassCount] {};
    unsigned int builtPreset[DlssNr::MaxPassCount] {};
    unsigned int builtStyle[DlssNr::MaxPassCount] {};
    unsigned int activePasses = 0;
    VkEvent creationReady = VK_NULL_HANDLE;
    bool creationPending = false;
    NVSDK_NGX_Parameter* capabilityParams = nullptr;

    // What the model writes, the proxy it is shown, and the frame as the upscaler left it.
    OwnedImage output;
    OwnedImage scratch;
    OwnedImage proxy;
    OwnedImage keep;
    bool beforeSr = false;
    bool rayReconstruction = false;

    // The proxy at the model's working size, when that is below the frame. The model -- 98% of the
    // cost -- then runs on this instead of the full proxy, which is the whole point of the working
    // scale slider. Unused (and never created) at scale 1, so the default path is unchanged.
    OwnedImage proxySmall;

    // Supersampling (working scale > 1): the model runs above native, superUp enlarges the proxy to
    // that size and superDown averages the answer (output) back into outputNative at native for a 1:1
    // composite. nrScaler is the filter both were built with, so a changed DlssNrScalingDownscaler
    // rebuilds them. Unused and never created at scale <= 1.
    OwnedImage outputNative;
    std::unique_ptr<OS_Vk> superUp;
    std::unique_ptr<OS_Vk> superDown;
    Scaler nrScaler = Scaler::Count;

    DlssNr_Vk* pass = nullptr;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t workWidth = 0;
    uint32_t workHeight = 0;
    bool reset = true;
    unsigned long long frames = 0;

    // Timing. A pair of timestamps per frame across a ring, read back three frames later: a query
    // read the frame it was written stalls the CPU on the GPU, which would cost more than the pass
    // it is measuring. Vulkan reports ticks, and timestampPeriod is how many nanoseconds a tick is.
    VkQueryPool queryPool = VK_NULL_HANDLE;
    float timestampPeriod = 0.0f;
    unsigned long long timedFrames = 0;
    std::optional<double> lastGpuTime;

    // Whether the game hands over an exposure texture, and what it said when it did.
    bool exposureOffered = false;

    // The game's own exposure, read off its 1x1 texture, and the scale it multiplied its buffer by.
    //
    // gameExposure holds its last good value rather than resetting when a frame arrives without a
    // texture: GTA V dropped it three times in one session on the D3D12 path, and falling back to a
    // default on those frames is a flicker, not a fallback.
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;

    // The exposure's courier: an 8x8 R32_FLOAT image the meter writes, and a ring of host-visible
    // buffers it is copied into. Only texel (0,0) is ever read -- the rest of the grid belongs to the
    // frame-statistics meter that was removed from the shared shader, and 8x8 is here only so that a
    // single 8x8 thread group lands entirely inside the image.
    OwnedImage meter;
    VkBuffer meterReadback[4] = {};
    VkDeviceMemory meterReadbackMemory[4] = {};
    void* meterMapped[4] = {};
    unsigned long long meterFrames = 0;
};

// The grid the meter writes, and the size of one readback. 8 * 8 * sizeof(float).
constexpr uint32_t kMeterSide = 8;
constexpr VkDeviceSize kMeterBytes = kMeterSide * kMeterSide * sizeof(float);

// Four, so the slot being read is four frames behind the slot being written and the read never waits
// on the GPU. Same depth as the D3D12 meter's ring, for the same reason.
constexpr unsigned long long kMeterSlots = 4;

// Four frames of pairs. Three would do, four keeps the modulo cheap and the slot being written well
// clear of the slot being read.
constexpr uint32_t kTimingSlots = 4;

struct ModelVk::Impl
{
    VkState state;
    bool reported = false;
    bool warnedVkSuper = false;
    bool saidEncoding = false;
    float loggedExposure = -1.0f;
    bool saidExposure = false;
    bool warnedDeferred = false;
    std::mutex mutex;
    uint64_t retryGeneration = ReadControlRequests().retryGeneration;
    Impl(DlssNr_Vk& shader) { state.pass = &shader; }
    ~Impl()
    {
        Shutdown();
        ClearStatus(this);
    }

    void Fail(const char* why)
    {
        if (state.failed)
            return;

        state.failed = true;
        state.reason = why;
        LOG_ERROR("DLSS-NR Vulkan unavailable: {}", why);
    }

    // ---------------------------------------------------------------------------------------------
    // Images this pass owns
    // ---------------------------------------------------------------------------------------------

    void DestroyImage(OwnedImage& img)
    {
        if (state.device == VK_NULL_HANDLE)
            return;

        if (img.view != VK_NULL_HANDLE)
            vkDestroyImageView(state.device, img.view, nullptr);

        if (img.image != VK_NULL_HANDLE)
            vkDestroyImage(state.device, img.image, nullptr);

        if (img.memory != VK_NULL_HANDLE)
            vkFreeMemory(state.device, img.memory, nullptr);

        img = OwnedImage {};
    }

    uint32_t FindMemoryTypeIndex(uint32_t typeBits, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memProps {};
        vkGetPhysicalDeviceMemoryProperties(state.physicalDevice, &memProps);

        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        {
            if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        }

        return UINT32_MAX;
    }

    // STORAGE and SAMPLED both, because every one of these is written by one dispatch and read by the
    // next; TRANSFER_SRC so a capture can copy it out without a second surface.
    // Build the OS_Vk resample descriptor for one of our own images. OS_Vk reads Width/Height/Format from
    // this (the NR override makes it size from the images, not the current feature).
    static VkImageInfo ImageInfoOf(const OwnedImage& img)
    {
        VkImageInfo info {};
        info.ImageView = img.view;
        info.Image = img.image;
        info.SubresourceRange = VkImageSubresourceRange { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        info.Format = img.format;
        info.Width = img.width;
        info.Height = img.height;
        return info;
    }

    bool CreateImage(OwnedImage& img, uint32_t width, uint32_t height, VkFormat format, bool readWrite)
    {
        DestroyImage(img);

        VkImageCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(state.device, &info, nullptr, &img.image) != VK_SUCCESS)
        {
            LOG_ERROR("DLSS-NR Vulkan: could not create a {}x{} image", width, height);
            return false;
        }

        VkMemoryRequirements req {};
        vkGetImageMemoryRequirements(state.device, img.image, &req);

        VkMemoryAllocateInfo alloc {};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = FindMemoryTypeIndex(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (alloc.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(state.device, &alloc, nullptr, &img.memory) != VK_SUCCESS ||
            vkBindImageMemory(state.device, img.image, img.memory, 0) != VK_SUCCESS)
        {
            LOG_ERROR("DLSS-NR Vulkan: could not back a {}x{} image", width, height);
            DestroyImage(img);
            return false;
        }

        VkImageViewCreateInfo view {};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = img.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        if (vkCreateImageView(state.device, &view, nullptr, &img.view) != VK_SUCCESS)
        {
            LOG_ERROR("DLSS-NR Vulkan: could not view a {}x{} image", width, height);
            DestroyImage(img);
            return false;
        }

        img.width = width;
        img.height = height;
        img.format = format;
        img.layout = VK_IMAGE_LAYOUT_UNDEFINED;

        // The NGX wrapper. Filled once, because none of it changes until the image is recreated.
        img.ngx.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
        img.ngx.Resource.ImageViewInfo.ImageView = img.view;
        img.ngx.Resource.ImageViewInfo.Image = img.image;
        img.ngx.Resource.ImageViewInfo.SubresourceRange = view.subresourceRange;
        img.ngx.Resource.ImageViewInfo.Format = format;
        img.ngx.Resource.ImageViewInfo.Width = width;
        img.ngx.Resource.ImageViewInfo.Height = height;
        img.ngx.ReadWrite = readWrite;

        return true;
    }

    // The ring of host-visible buffers the meter's grid is copied into, created once and mapped for
    // good. HOST_COHERENT so the read needs no invalidate; it is universally available for a buffer this
    // small and the alternative is a vkInvalidateMappedMemoryRanges on a path that runs every frame.
    bool CreateMeterReadback()
    {
        for (unsigned long long i = 0; i < kMeterSlots; ++i)
        {
            if (state.meterReadback[i] != VK_NULL_HANDLE)
                continue;

            VkBufferCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = kMeterBytes;
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(state.device, &info, nullptr, &state.meterReadback[i]) != VK_SUCCESS)
            {
                LOG_WARN("DLSS-NR Vulkan: could not create the exposure readback buffer");
                return false;
            }

            VkMemoryRequirements req {};
            vkGetBufferMemoryRequirements(state.device, state.meterReadback[i], &req);

            VkMemoryAllocateInfo alloc {};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = FindMemoryTypeIndex(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            if (alloc.memoryTypeIndex == UINT32_MAX ||
                vkAllocateMemory(state.device, &alloc, nullptr, &state.meterReadbackMemory[i]) != VK_SUCCESS ||
                vkBindBufferMemory(state.device, state.meterReadback[i], state.meterReadbackMemory[i], 0) !=
                    VK_SUCCESS ||
                vkMapMemory(state.device, state.meterReadbackMemory[i], 0, kMeterBytes, 0, &state.meterMapped[i]) !=
                    VK_SUCCESS)
            {
                LOG_WARN("DLSS-NR Vulkan: could not back the exposure readback buffer");
                return false;
            }
        }

        return true;
    }

    void DestroyMeterReadback()
    {
        for (unsigned long long i = 0; i < kMeterSlots; ++i)
        {
            if (state.meterReadbackMemory[i] != VK_NULL_HANDLE)
            {
                if (state.meterMapped[i] != nullptr)
                    vkUnmapMemory(state.device, state.meterReadbackMemory[i]);

                vkFreeMemory(state.device, state.meterReadbackMemory[i], nullptr);
            }

            if (state.meterReadback[i] != VK_NULL_HANDLE)
                vkDestroyBuffer(state.device, state.meterReadback[i], nullptr);

            state.meterMapped[i] = nullptr;
            state.meterReadbackMemory[i] = VK_NULL_HANDLE;
            state.meterReadback[i] = VK_NULL_HANDLE;
        }

        state.meterFrames = 0;
    }

    // A layout transition with the access masks that go with it. Vulkan has no equivalent of D3D12's
    // state promotion, so every read and every write says which layout it needs and this is how it gets
    // there. Tracked per image so a no-op transition is not recorded.
    void Transition(VkCommandBuffer cmd, OwnedImage& img, VkImageLayout to)
    {
        if (img.image == VK_NULL_HANDLE || img.layout == to)
            return;

        VkImageMemoryBarrier barrier {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = img.layout;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = img.image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        const auto access = [](VkImageLayout layout) -> VkAccessFlags
        {
            switch (layout)
            {
            case VK_IMAGE_LAYOUT_UNDEFINED:
                return 0;
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                return VK_ACCESS_TRANSFER_READ_BIT;
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                return VK_ACCESS_TRANSFER_WRITE_BIT;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return VK_ACCESS_SHADER_READ_BIT;
            default:
                return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            }
        };
        barrier.srcAccessMask = access(img.layout);
        barrier.dstAccessMask = access(to);

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);

        img.layout = to;
    }

    // A resource the game owns. Its layout is the game's business, so this records the transition and
    // puts it back exactly as it was rather than tracking it.
    void TransitionForeign(VkCommandBuffer cmd, VkImage image, VkImageSubresourceRange range, VkImageLayout from,
                           VkImageLayout to)
    {
        if (image == VK_NULL_HANDLE || from == to)
            return;

        VkImageMemoryBarrier barrier {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = range;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);
    }

    // ---------------------------------------------------------------------------------------------
    // Bring-up
    // ---------------------------------------------------------------------------------------------

    bool LoadForwarder()
    {
        if (state.forwarder != nullptr)
            return state.init != nullptr && state.create != nullptr && state.evaluate != nullptr;

        auto path = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx.dll_dlssnr.dll");

        if (!path.has_value())
            path = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

        if (!path.has_value())
        {
            Fail("nvngx.dll_dlssnr.dll was not found beside OptiScaler or the game");
            return false;
        }

        state.forwarder = LoadLibraryW(path->wstring().c_str());

        if (state.forwarder == nullptr)
        {
            Fail("the forwarder would not load");
            return false;
        }

        state.probe = (PFN_VkProbe) GetProcAddress(state.forwarder, "dlssnr_vk_probe");
        state.init = (PFN_VkInit) GetProcAddress(state.forwarder, "dlssnr_vk_init");
        state.create = (PFN_VkCreate) GetProcAddress(state.forwarder, "dlssnr_vk_create");
        state.evaluate = (PFN_VkEvaluate) GetProcAddress(state.forwarder, "dlssnr_vk_evaluate_v2");
        state.release = (PFN_VkRelease) GetProcAddress(state.forwarder, "dlssnr_vk_release");

        if (state.init == nullptr || state.create == nullptr || state.evaluate == nullptr)
        {
            Fail("Update nvngx.dll_dlssnr.dll from the complete release (NR v2 exports required)");
            return false;
        }

        return true;
    }

    // Whether a format can hold linear, open-ended light. A frame the game already tone mapped has white
    // at 1 and must not be encoded a second time; an 8-bit or normalised format cannot be scene-referred
    // whatever the game says. The D3D12 path asks the same question of DXGI formats.
    bool FormatCanHoldLinearHdr(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R16G16B16_SFLOAT:
        case VK_FORMAT_R32G32B32_SFLOAT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
            return true;
        default:
            return false;
        }
    }

    std::optional<std::filesystem::path> FindSnippet()
    {
        auto snippet = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        return snippet;
    }

    bool Evaluate(VkCommandBuffer cmdBuffer, const VkImageInfo& colourInfo, const VkImageInfo& depthInfo,
                  const VkImageInfo& motionInfo, const VkImageInfo& target, const DlssNrFrameInfo_Vk& frame,
                  VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, VkImageLayout inputLayout)
    {
        const bool beforeSr = frame.BeforeUpscale;
        const bool rayReconstruction = frame.RayReconstruction;
        auto& cfg = *Config::Instance();

        if (cfg.DlssNrDeferredDlss.value_or_default() && !rayReconstruction)
        {

            if (!warnedDeferred)
            {
                LOG_WARN("DLSS-NR DeferredDLSS requires the D3D12 path or a D3D12 bridge; "
                         "native Vulkan leaves the clean SR frame unchanged");
                warnedDeferred = true;
            }
            return false;
        }

        if (cfg.DlssNrFinishedPicture.value_or_default())
            return false; // finished-picture composition requires a native D3D12 swapchain

        if (!cfg.DlssNrEnabled.value_or_default())
            return false;

        if (cmdBuffer == VK_NULL_HANDLE || device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE)
            return false;

        std::lock_guard<std::mutex> lock(mutex);

        struct ReportStatus
        {
            Impl* owner;
            ~ReportStatus()
            {
                const auto& state = owner->state;
                ExposureStatus exposure {};
                exposure.seenFrames = state.frames;
                exposure.offeredNow = exposure.everOffered = state.exposureOffered;
                exposure.exposure = state.gameExposure;
                exposure.preExposure = state.gamePreExposure;
                PublishStatus(owner, Backend::Vulkan,
                              { state.feature != nullptr && !state.failed, state.reason, state.lastGpuTime,
                                state.frames, exposure, false });
            }
        } report { this };
        const auto requests = ReadControlRequests();
        if (requests.retryGeneration != retryGeneration)
        {
            retryGeneration = requests.retryGeneration;
            if (state.device && vkDeviceWaitIdle(state.device) != VK_SUCCESS)
            {
                Fail("the Vulkan device could not retire work for retry");
                return false;
            }
            Shutdown();
            state.failed = false;
            state.reason = "";
        }
        if (state.failed)
            return false;

        auto wrap = [](const VkImageInfo& image, bool readWrite)
        {
            NVSDK_NGX_Resource_VK resource {};
            resource.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
            resource.Resource.ImageViewInfo = { image.ImageView, image.Image, image.SubresourceRange,
                                                image.Format,    image.Width, image.Height };
            resource.ReadWrite = readWrite;
            return resource;
        };
        auto colourResource = wrap(colourInfo, false);
        auto depthResource = wrap(depthInfo, frame.DepthReadWrite);
        auto motionResource = wrap(motionInfo, frame.MotionReadWrite);
        auto* colour = &colourResource;
        auto* depth = &depthResource;
        auto* motion = &motionResource;

        // The game's exposure, now read rather than only counted.
        //
        // What blocked this was the layout: a descriptor names the layout its image will be in when the
        // shader runs, NVIDIA's Vulkan header does not use the word "layout" once, and a barrier is no
        // safer because it needs the layout it is coming from. Three things in this tree answer it, and
        // they agree. FSR2Feature_Vk hands this same texture to FidelityFX as COMPUTE_READ, which its
        // Vulkan backend maps to SHADER_READ_ONLY_OPTIMAL, on a path that works in these games. The
        // D3D12-on-Vulkan bridge transitions the game's exposure image out of SHADER_READ_ONLY_OPTIMAL,
        // on a path that works. And the header stating nothing means there is no contract to break --
        // the convention is the contract.
        //
        // So it is bound in SHADER_READ_ONLY_OPTIMAL and no barrier is recorded: this never transitions a
        // resource it does not own. If a game turns out to leave it somewhere else the cost is a wrong
        // number, not a lost device, and the gate on the readback throws a wrong number away.
        auto* exposure = static_cast<NVSDK_NGX_Resource_VK*>(frame.ExposureTexture);
        const float preExposure = frame.PreExposure;
        const bool havePre = true;

        if (!saidExposure)
        {
            saidExposure = true;
            LOG_INFO("DLSS-NR Vulkan: exposure from the game: DLSS.Pre.Exposure {}, ExposureTexture {}",
                     havePre ? std::to_string(preExposure) : std::string("not supplied"),
                     exposure != nullptr ? "supplied" : "not supplied");
        }

        state.exposureOffered = exposure != nullptr;

        if (havePre && std::isfinite(preExposure) && preExposure > 0.0f)
            state.gamePreExposure = preExposure;

        // Take the grid written four frames ago. Retired by now, so this reads mapped memory rather than
        // waiting on the GPU -- which is the whole reason for the ring.
        if (state.meterFrames >= kMeterSlots)
        {
            const void* mapped = state.meterMapped[state.meterFrames % kMeterSlots];

            if (mapped != nullptr)
            {
                float measured = 0.0f;
                std::memcpy(&measured, mapped, sizeof(float));

                // Believed only if it could be an exposure. A texel read through a layout the game did
                // not leave it in, or a slot the game stopped filling, fails here and the last good
                // value stands.
                if (std::isfinite(measured) && measured > 0.0f)
                    state.gameExposure = measured;
            }
        }

        // Said when it moves by more than a fiftieth, not every frame. Enough to see in a log that the
        // number is the game's and that it tracks the scene, without a line per frame.

        if (state.gameExposure > 1e-6f &&
            std::abs(loggedExposure - state.gameExposure) > std::max(0.02f * state.gameExposure, 1e-5f))
        {
            loggedExposure = state.gameExposure;
            LOG_INFO("DLSS-NR Vulkan: the game's exposure is {}, pre-exposure {}, so white point {}",
                     state.gameExposure, state.gamePreExposure, state.gamePreExposure / state.gameExposure);
        }

        if (colour->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ||
            depth->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ||
            motion->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ||
            colour->Resource.ImageViewInfo.ImageView == VK_NULL_HANDLE ||
            depth->Resource.ImageViewInfo.ImageView == VK_NULL_HANDLE ||
            motion->Resource.ImageViewInfo.ImageView == VK_NULL_HANDLE)
            return false;

        uint32_t width = colour->Resource.ImageViewInfo.Width;
        uint32_t height = colour->Resource.ImageViewInfo.Height;
        const auto renderWidth = frame.RenderSubrectWidth, renderHeight = frame.RenderSubrectHeight;
        const auto baseX = frame.ColorSubrectBaseX, baseY = frame.ColorSubrectBaseY;
        if (beforeSr)
        {
            // Origin-zero padded inputs are common with dynamic resolution. Never use a preset table.
            if (baseX || baseY || ((renderWidth == 0) != (renderHeight == 0)) || renderWidth > width ||
                renderHeight > height)
                return false; // caller falls back to post-SR, without editing the input
            if (renderWidth && renderHeight)
            {
                width = renderWidth;
                height = renderHeight;
            }
        }
        const auto depthX = frame.DepthSubrectBaseX, depthY = frame.DepthSubrectBaseY;
        const auto motionX = frame.MotionSubrectBaseX, motionY = frame.MotionSubrectBaseY;
        const auto outputWidth = frame.OutputWidth ? frame.OutputWidth : target.Width;
        const auto outputHeight = frame.OutputHeight ? frame.OutputHeight : target.Height;
        const auto guides =
            ResolveGuideRegions({ depth->Resource.ImageViewInfo.Width, depth->Resource.ImageViewInfo.Height },
                                { motion->Resource.ImageViewInfo.Width, motion->Resource.ImageViewInfo.Height },
                                { renderWidth, renderHeight }, { outputWidth, outputHeight },
                                frame.MotionVectorsLowResolution, depthX, depthY, motionX, motionY);
        if (!guides.depth.valid() || !guides.motion.valid())
            return false;
        const auto guideWidth = guides.depth.width, guideHeight = guides.depth.height;

        if (!width || !height || width > target.Width || height > target.Height)
            return false;

        // The model's working size. The slider is a fraction of the frame; at 1 it is the frame, and the
        // reduced path below never runs, so the default is byte-for-byte what it was.
        // Above 1 the model supersamples (up to 2x): the proxy is enlarged, the model runs above native,
        // and superDown averages the answer back. Vulkan matches the D3D12 cap.
        float workScale = cfg.DlssNrWorkingScale.value_or_default();
        workScale = std::isfinite(workScale) ? std::clamp(workScale, 0.25f, 2.0f) : 1.0f;
        const uint32_t workWidth = std::max(1u, (uint32_t) (width * workScale + 0.5f));
        const uint32_t workHeight = std::max(1u, (uint32_t) (height * workScale + 0.5f));
        const bool reduced = workWidth != width || workHeight != height;
        const unsigned int passes =
            std::clamp(cfg.DlssNrPasses.value_or_default(), 1u,
                       cfg.DlssNrUnlockPasses.value_or_default() ? DlssNr::MaxPassCount : DlssNr::DefaultMaxPassCount);

        state.instance = instance;
        state.physicalDevice = physicalDevice;

        // The shader belongs to one device. Replacement features own replacement models.
        if (state.device != VK_NULL_HANDLE && state.device != device)
        {
            Fail("a Vulkan model was dispatched on a different device");
            return false;
        }
        state.device = device;

        if (!LoadForwarder())
            return false;

        // Initialise NGX on this device, once. The snippet path is the model itself; the forwarder loads
        // it so the caller gate sees a module named nvngx.dll.
        if (state.creationPending)
        {
            // A second NGX evaluate need not mean the previous command buffer was submitted.
            // Poll the GPU marker without waiting; repeated calls during warm-up stay clean.
            if (vkGetEventStatus(device, state.creationReady) != VK_EVENT_SET)
                return false;
            state.creationPending = false;
        }

        if (!state.ngxInitialised)
        {
            auto snippet = FindSnippet();

            if (!snippet.has_value())
            {
                Fail("nvngx_dlssnr.dll was not found beside OptiScaler or the game");
                return false;
            }

            const int probe = state.probe != nullptr ? state.probe(snippet->wstring().c_str()) : 0;

            // Four bits, one per entry point. Anything short of fifteen means the model's Vulkan surface
            // is not entirely reachable and there is no point going further.
            if (probe != 15)
            {
                LOG_ERROR("DLSS-NR Vulkan: the model's Vulkan surface is incomplete (probe {})", probe);
                Fail("the model does not expose a complete Vulkan surface");
                return false;
            }

            const int result =
                state.init(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                           (void*) instance, (void*) physicalDevice, (void*) device, 0x0000015);

            if (result != 1)
            {
                LOG_ERROR("DLSS-NR Vulkan: NVSDK_NGX_VULKAN_Init_Ext returned {}", result);
                Fail("the model would not initialise on this Vulkan device");
                return false;
            }

            state.ngxInitialised = true;
            LOG_INFO("DLSS-NR Vulkan: the model initialised on this device");
        }

        if (state.capabilityParams == nullptr)
        {
            if (NVSDK_NGX_VULKAN_AllocateParameters(&state.capabilityParams) != NVSDK_NGX_Result_Success ||
                state.capabilityParams == nullptr)
            {
                Fail("a parameter block could not be allocated");
                return false;
            }
        }

        if (state.queryPool == VK_NULL_HANDLE)
        {
            VkPhysicalDeviceProperties props {};
            vkGetPhysicalDeviceProperties(physicalDevice, &props);

            // A period of zero means the device does not support timestamps on this queue. The pass runs
            // regardless; it simply reports no cost, which is what the D3D12 path does when its heap is
            // unavailable.
            state.timestampPeriod = props.limits.timestampPeriod;

            if (state.timestampPeriod > 0.0f)
            {
                VkQueryPoolCreateInfo info {};
                info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
                info.queryType = VK_QUERY_TYPE_TIMESTAMP;
                info.queryCount = kTimingSlots * 2;

                if (vkCreateQueryPool(device, &info, nullptr, &state.queryPool) != VK_SUCCESS)
                {
                    state.queryPool = VK_NULL_HANDLE;
                    LOG_INFO("DLSS-NR Vulkan: no timestamp pool, the pass will not report its cost");
                }
            }
        }

        if (!state.pass->IsInit())
            return false;

        // Resize. The feature is built for a size and has to be rebuilt when the frame OR the working
        // size changes -- moving the slider is a rebuild, which is why it is compared here.
        bool profileChanged = state.activePasses != passes;
        for (unsigned int pass = 0; pass < passes; ++pass)
            profileChanged |= state.builtTuning[pass] != Profiles::PassTuning(cfg, pass) ||
                              state.builtPreset[pass] != Profiles::PassPreset(cfg, pass) ||
                              state.builtStyle[pass] != Profiles::PassStyle(cfg, pass);
        if (state.width != width || state.height != height || state.workWidth != workWidth ||
            state.workHeight != workHeight || state.beforeSr != beforeSr ||
            state.rayReconstruction != rayReconstruction || profileChanged)
        {
            // This block releases the feature and frees the surfaces below IMMEDIATELY. A frame-size
            // change is already fenced by the game -- it recreates the swapchain around it -- but moving
            // the working-scale slider is not: the game is mid-flight and previous frames' command
            // buffers still reference the feature and images about to be destroyed. Freeing a Vulkan
            // resource that in-flight GPU work still touches is device removal (ERR_GFX_STATE, reproduced
            // on RDR2 and Enshrouded by dragging the model-resolution slider). Drain the device first.
            // Only the rare resize path reaches here, so the CPU stall is a one-off hitch, not per-frame.
            if (state.device != VK_NULL_HANDLE && vkDeviceWaitIdle(state.device) != VK_SUCCESS)
            {
                Fail("the Vulkan device could not retire previous model resources");
                return false;
            }

            if (state.feature != nullptr && state.release != nullptr)
            {
                state.release(state.feature);
                state.feature = nullptr;
            }
            for (auto& feature : state.laterFeatures)
            {
                if (feature && state.release)
                    state.release(feature);
                feature = nullptr;
            }
            DestroyImage(state.scratch);

            const VkFormat working = VK_FORMAT_R16G16B16A16_SFLOAT;

            // The meter is a fixed 8x8 whatever the frame is, so it is only built the once -- but it is
            // built alongside the rest so that a failure here is caught by the same check.
            const bool meterReady =
                (state.meter.Valid() || CreateImage(state.meter, kMeterSide, kMeterSide, VK_FORMAT_R32_SFLOAT, true)) &&
                CreateMeterReadback();

            if (!meterReady)
                LOG_WARN("DLSS-NR Vulkan: no exposure meter; the white point stays on the slider");

            DestroyImage(state.proxySmall);
            DestroyImage(state.outputNative);

            // output is the model's target, so it is the working size. proxy and keep are full: proxy is
            // the source the downsample reads, keep is the untouched frame the resolve composites onto.
            // outputNative is the native buffer the supersample down-leg averages the answer into.
            const bool ok = CreateImage(state.output, workWidth, workHeight, working, true) &&
                            (passes == 1 || CreateImage(state.scratch, workWidth, workHeight, working, true)) &&
                            CreateImage(state.proxy, width, height, working, true) &&
                            CreateImage(state.keep, width, height, working, true) &&
                            (!reduced || CreateImage(state.proxySmall, workWidth, workHeight, working, true)) &&
                            (workScale <= 1.0f || CreateImage(state.outputNative, width, height, working, true));

            if (!ok)
            {
                Fail("the pass could not allocate its own surfaces");
                return false;
            }

            state.width = width;
            state.height = height;
            state.workWidth = workWidth;
            state.workHeight = workHeight;
            state.beforeSr = beforeSr;
            state.rayReconstruction = rayReconstruction;
            state.activePasses = passes;
            for (unsigned int pass = 0; pass < passes; ++pass)
            {
                state.builtTuning[pass] = Profiles::PassTuning(cfg, pass);
                state.builtPreset[pass] = Profiles::PassPreset(cfg, pass);
                state.builtStyle[pass] = Profiles::PassStyle(cfg, pass);
            }
            state.reset = true;
        }

        bool created = false;
        for (unsigned int pass = 0; pass < passes; ++pass)
        {
            void*& feature = pass == 0 ? state.feature : state.laterFeatures[pass];
            if (feature)
                continue;
            const auto tuning = Profiles::PassTuning(cfg, pass);
            feature = state.create((void*) cmdBuffer, state.capabilityParams, workWidth, workHeight,
                                   (int) Profiles::PassPreset(cfg, pass), tuning.intensity,
                                   (int) Profiles::PassStyle(cfg, pass), tuning.structure, tuning.tone, tuning.skin,
                                   tuning.autoMask ? 1 : 0, 1);
            if (!feature)
            {
                Fail("the model would not build a feature on this device");
                return false;
            }

            LOG_INFO("DLSS-NR Vulkan: pass {} built at {}x{} (frame {}x{}, {} SR)", pass + 1, workWidth, workHeight,
                     width, height, beforeSr ? "before" : "after");
            created = true;
            state.reset = true;
        }
        // NGX creation may record GPU uploads. Do not evaluate until a subsequent submission,
        // just as on D3D12. Leave this warm-up frame clean instead of evaluating unready weights.
        if (created)
        {
            if (state.creationReady == VK_NULL_HANDLE)
            {
                VkEventCreateInfo info { VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
                if (vkCreateEvent(device, &info, nullptr, &state.creationReady) != VK_SUCCESS)
                {
                    Fail("could not allocate the model creation marker");
                    return false;
                }
            }
            else
                vkResetEvent(device, state.creationReady); // rebuild above drained previous GPU users
            vkCmdSetEvent(cmdBuffer, state.creationReady, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            state.creationPending = true;
            return false;
        }

        // -----------------------------------------------------------------------------------------
        // Encode: the frame the upscaler wrote -> a display-referred proxy, plus an untouched copy
        // -----------------------------------------------------------------------------------------

        const bool gameSaysHdr = frame.ColourIsLinearHdr;
        const bool depthInverted = frame.DepthInverted;

        if (frame.Reset)
            state.reset = true;

        // Both have to agree. A game can set the HDR flag on a buffer that cannot hold open-ended light,
        // and encoding an already tone-mapped frame a second time looks washed out and banded.
        const bool linearHdr = gameSaysHdr && FormatCanHoldLinearHdr(colour->Resource.ImageViewInfo.Format);

        // The same rule as the D3D12 path, deliberately spelled the same way: the game divides its frame
        // by preExposure and multiplies by exposure, so undoing that is the divisor this pass wants, and
        // the slider becomes a trim on top rather than the answer.
        //
        // The trim is bounded here, at the point of use, rather than at the slider. Someone who found 64
        // by hand on the manual path and then switches the exposure source on keeps that 64 in their ini;
        // bounding it in the menu would leave the picture wrong for a reason the menu no longer showed.
        // Their value stays in the config untouched, so switching back to manual restores it.
        float whitePoint = cfg.DlssNrWhitePointScale.value_or_default();

        if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && state.gameExposure > 1e-6f)
        {
            const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
            whitePoint = std::clamp(state.gamePreExposure / state.gameExposure * trim, 0.01f, 4096.0f);
        }

        if (!saidEncoding)
        {
            saidEncoding = true;
            LOG_INFO("DLSS-NR Vulkan: the game's buffer is {} (flag {}, format {}), depth {}",
                     linearHdr ? "linear HDR" : "already tone-mapped", gameSaysHdr ? "set" : "clear",
                     (int) colour->Resource.ImageViewInfo.Format, depthInverted ? "inverted" : "normal");
        }

        auto encode = DlssNr_Common::MakeConstants(DlssNrMode_Encode, width, height, whitePoint, linearHdr, cfg);
        encode.GuideWidth = guideWidth;
        encode.GuideHeight = guideHeight;

        const VkImageSubresourceRange colourRange = colour->Resource.ImageViewInfo.SubresourceRange;

        // Open the measurement. Reset immediately before writing: a query pool slot must be reset before
        // it is written again, and doing it here rather than at the end keeps the two in one place.
        const uint32_t timingSlot = (uint32_t) (state.timedFrames % kTimingSlots);

        if (state.queryPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(cmdBuffer, state.queryPool, timingSlot * 2, 2);
            vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, state.queryPool, timingSlot * 2);
        }

        // The game's colour is read here and written at the end. Its layout on arrival is GENERAL, which
        // is what NGX requires of a resource it is handed, so it is left alone.
        Transition(cmdBuffer, state.proxy, VK_IMAGE_LAYOUT_GENERAL);
        Transition(cmdBuffer, state.keep, VK_IMAGE_LAYOUT_GENERAL);

        // Read in GENERAL, which is the layout it is actually in.
        //
        // This slot used to take the default and declare SHADER_READ_ONLY_OPTIMAL, which disagreed with
        // the comment four lines up and with the resolve below -- the resolve writes this same image as a
        // storage image, which is only legal in GENERAL, and nothing transitions it in between. It is the
        // upscaler's output, a storage image the upscaler has just written, so GENERAL is what it is.
        // Inert on the only hardware this model runs on, wrong everywhere it is read.
        if (!state.pass->Dispatch(cmdBuffer, encode, width, height, colour->Resource.ImageViewInfo.ImageView,
                                  VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, state.proxy.view, state.keep.view,
                                  inputLayout))
        {
            Fail("the encode dispatch failed");
            return false;
        }

        // The model's input: the full proxy, or a downsampled copy of it when the working scale is below
        // the frame. Mirrors the D3D12 path -- the encode always writes a full proxy, and a separate
        // downsample makes the small one the model actually reads.
        OwnedImage* modelInput = &state.proxy;

        if (reduced && state.proxySmall.Valid())
        {
            bool built = false;

            if (workScale > 1.0f)
            {
                // Supersample: upscale the proxy to the super-native working size with the chosen filter so
                // the model sees a clean input. Rebuild both scalers when the NR downscaler changed (baked
                // at construction). proxy -> SHADER_READ_ONLY (sampled), proxySmall -> GENERAL (storage).
                const Scaler wantScaler = cfg.DlssNrScalingDownscaler.value_or_default();
                if (state.nrScaler != wantScaler)
                {
                    // Rebuilding frees the old scalers' pipelines/descriptors. The filter dropdown changes
                    // no size, so this does NOT go through the resize block's drain -- and prior frames'
                    // submitted command buffers still bind these pipelines. Freeing them under in-flight GPU
                    // work is device removal (the same hazard the resize path drains for). Drain first. A
                    // filter change is rare, so the one-off stall is a hitch, not a per-frame cost.
                    if (state.device != VK_NULL_HANDLE && vkDeviceWaitIdle(state.device) != VK_SUCCESS)
                    {
                        Fail("the Vulkan device could not retire the supersampling filters");
                        return false;
                    }
                    state.superUp.reset();
                    state.superDown.reset();
                    state.nrScaler = wantScaler;
                }
                if (!state.superUp)
                    state.superUp =
                        std::make_unique<OS_Vk>("DLSS-NR VK supersample up", device, physicalDevice, true, wantScaler);
                if (!state.superDown)
                    state.superDown = std::make_unique<OS_Vk>("DLSS-NR VK supersample down", device, physicalDevice,
                                                              false, wantScaler);

                Transition(cmdBuffer, state.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                Transition(cmdBuffer, state.proxySmall, VK_IMAGE_LAYOUT_GENERAL);

                VkImageInfo upin = ImageInfoOf(state.proxy);
                VkImageInfo upout = ImageInfoOf(state.proxySmall);

                if (state.superUp && state.superUp->IsInit() && state.superUp->Dispatch(cmdBuffer, upin, upout))
                    built = true;
                else
                {

                    if (!warnedVkSuper)
                    {
                        warnedVkSuper = true;
                        LOG_WARN("DLSS-NR Vulkan supersample: upscaler unavailable, falling back to box enlarge.");
                    }
                }
            }

            if (!built)
            {
                DlssNrConstants down = encode;
                down.Mode = DlssNrMode_Downsample;
                down.Width = workWidth;
                down.Height = workHeight;

                Transition(cmdBuffer, state.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                Transition(cmdBuffer, state.proxySmall, VK_IMAGE_LAYOUT_GENERAL);

                if (!state.pass->Dispatch(cmdBuffer, down, workWidth, workHeight, state.proxy.view, VK_NULL_HANDLE,
                                          VK_NULL_HANDLE, VK_NULL_HANDLE, state.proxySmall.view, VK_NULL_HANDLE,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
                {
                    Fail("the downsample dispatch failed");
                    return false;
                }
            }

            modelInput = &state.proxySmall;
        }

        // -----------------------------------------------------------------------------------------
        // The meter: the game's 1x1 exposure -> texel (0,0) of the grid -> a buffer the CPU can read
        // -----------------------------------------------------------------------------------------

        // The motion slot carries it, because the meter has no use for motion vectors and the shader is
        // one shader with a fixed set of bindings. The source slot is left empty and gets the dummy.
        //
        // Gated on the setting that consumes the answer, which is not merely tidy. This is the only place
        // the pass binds a resource it does not own on a guess about its layout, and the guess is good
        // but it is still a guess. A user who has not asked for the exposure source never has the game's
        // image touched at all, so if some engine does leave it somewhere unexpected, the blast radius is
        // people who turned the thing on rather than everyone on Vulkan.
        if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && exposure != nullptr &&
            exposure->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW &&
            exposure->Resource.ImageViewInfo.ImageView != VK_NULL_HANDLE && state.meter.Valid())
        {
            const unsigned long long slot = state.meterFrames % kMeterSlots;

            if (state.meterReadback[slot] != VK_NULL_HANDLE)
            {
                DlssNrConstants meter = encode;
                meter.Mode = DlssNrMode_Meter;
                meter.Width = kMeterSide;
                meter.Height = kMeterSide;

                Transition(cmdBuffer, state.meter, VK_IMAGE_LAYOUT_GENERAL);

                if (state.pass->Dispatch(cmdBuffer, meter, kMeterSide, kMeterSide, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                         VK_NULL_HANDLE, exposure->Resource.ImageViewInfo.ImageView, state.meter.view,
                                         VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
                {
                    Transition(cmdBuffer, state.meter, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

                    VkBufferImageCopy region {};
                    region.bufferOffset = 0;
                    region.bufferRowLength = 0;
                    region.bufferImageHeight = 0;
                    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    region.imageOffset = { 0, 0, 0 };
                    region.imageExtent = { kMeterSide, kMeterSide, 1 };

                    vkCmdCopyImageToBuffer(cmdBuffer, state.meter.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           state.meterReadback[slot], 1, &region);

                    // The copy has to be visible to a host read, and only the host will read it.
                    VkBufferMemoryBarrier toHost {};
                    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost.buffer = state.meterReadback[slot];
                    toHost.offset = 0;
                    toHost.size = kMeterBytes;

                    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                         nullptr, 1, &toHost, 0, nullptr);

                    state.meterFrames++;
                }
            }
        }

        // -----------------------------------------------------------------------------------------
        // The model
        // -----------------------------------------------------------------------------------------

        float mvX = frame.MvScaleX, mvY = frame.MvScaleY;
        // Match D3D12: preserve the game's vector encoding, then adjust only for the NR working scale.
        mvX *= (float) workWidth / width;
        mvY *= (float) workHeight / height;
        OwnedImage* answer = &state.output;
        OwnedImage* input = modelInput;
        int evaluated = 1;
        for (unsigned int pass = 0; pass < passes; ++pass)
        {
            Transition(cmdBuffer, *input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, *answer, VK_IMAGE_LAYOUT_GENERAL);
            const auto tuning = Profiles::PassTuning(cfg, pass);
            evaluated =
                state.evaluate((void*) cmdBuffer, pass == 0 ? state.feature : state.laterFeatures[pass],
                               state.capabilityParams, &input->ngx, depth, motion, &answer->ngx, workWidth, workHeight,
                               guideWidth, guideHeight, guides.motion.width, guides.motion.height, guides.depth.x,
                               guides.depth.y, guides.motion.x, guides.motion.y, depthInverted ? 1 : 0,
                               state.reset ? 1 : 0, tuning.intensity, (int) Profiles::PassStyle(cfg, pass),
                               tuning.structure, tuning.tone, tuning.skin, tuning.autoMask ? 1 : 0, mvX, mvY);
            if (evaluated != 1)
                break;
            if (pass + 1 < passes)
            {
                input = answer;
                answer = answer == &state.output ? &state.scratch : &state.output;
            }
        }

        state.reset = false;
        state.frames++;

        if (evaluated != 1)
        {
            LOG_ERROR("DLSS-NR Vulkan: evaluate returned {}", evaluated);
            Fail("the model refused to evaluate");
            return false;
        }

        // -----------------------------------------------------------------------------------------
        // Resolve: proxy + the model's answer + the untouched copy -> the frame
        // -----------------------------------------------------------------------------------------

        DlssNrConstants resolve = encode;
        resolve.Mode = DlssNrMode_Resolve;

        // Supersampling down-leg (Vulkan). Average the Nx model answer back to native with the chosen
        // filter so the resolve composites a native answer against the native proxy 1:1 -- not the single
        // bilinear tap the Nx answer would otherwise get, which aliases the model's detail into noise. On
        // failure it falls back to the Nx pair (modelInput + output), the old behaviour.
        OwnedImage* resolveProxy = modelInput;
        OwnedImage* resolveAnswer = answer;

        if (workScale > 1.0f && state.superDown && state.superDown->IsInit() && state.outputNative.Valid())
        {
            Transition(cmdBuffer, *answer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, state.outputNative, VK_IMAGE_LAYOUT_GENERAL);

            VkImageInfo dsin = ImageInfoOf(*answer);
            VkImageInfo dsout = ImageInfoOf(state.outputNative);

            if (state.superDown->Dispatch(cmdBuffer, dsin, dsout))
            {
                resolveProxy = &state.proxy;
                resolveAnswer = &state.outputNative;
            }
        }

        Transition(cmdBuffer, *resolveProxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, *resolveAnswer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, state.keep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        if (!state.pass->Dispatch(cmdBuffer, resolve, width, height, resolveProxy->view, resolveAnswer->view,
                                  state.keep.view, VK_NULL_HANDLE, target.ImageView, VK_NULL_HANDLE,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
        {
            Fail("the resolve dispatch failed");
            return false;
        }

        // Close it, and read the pair from three frames ago -- retired by now, so the read does not wait.
        if (state.queryPool != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, state.queryPool, timingSlot * 2 + 1);
            state.timedFrames++;

            if (state.timedFrames > kTimingSlots)
            {
                const uint32_t readSlot = (uint32_t) (state.timedFrames % kTimingSlots);
                uint64_t ticks[2] = {};

                // Without WAIT: a slot this old is retired, and if it somehow is not, NOT_READY is the
                // right answer rather than a stall.
                if (vkGetQueryPoolResults(device, state.queryPool, readSlot * 2, 2, sizeof(ticks), ticks,
                                          sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                    ticks[1] > ticks[0])
                {
                    const double ms = (double) (ticks[1] - ticks[0]) * (double) state.timestampPeriod / 1e6;

                    // A pass that appears to have taken over a second did not; the queue was reset under
                    // it or the pair straddled a device change.
                    if (ms > 0.0 && ms < 1000.0)
                        state.lastGpuTime = ms;
                }
            }
        }

        if (!reported && state.frames > 2)
        {
            reported = true;
            LOG_INFO("DLSS-NR Vulkan: running {} SR at {}x{}, guides {}x{}", beforeSr ? "before" : "after", width,
                     height, guideWidth, guideHeight);
        }
        return true;
    }

    void Shutdown()
    {
        // The device is alive (real teardown): drain before freeing so nothing the GPU is still using is
        // destroyed under it, the same rule as the resize path.
        if (state.device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(state.device);

        if (state.feature != nullptr && state.release != nullptr)
            state.release(state.feature);

        state.feature = nullptr;

        for (auto& feature : state.laterFeatures)
        {
            if (feature && state.release)
                state.release(feature);
            feature = nullptr;
        }
        state.activePasses = 0;
        if (state.creationReady != VK_NULL_HANDLE)
            vkDestroyEvent(state.device, state.creationReady, nullptr);
        state.creationReady = VK_NULL_HANDLE;
        state.creationPending = false;

        DestroyImage(state.output);
        DestroyImage(state.scratch);
        DestroyImage(state.proxy);
        DestroyImage(state.proxySmall);
        DestroyImage(state.outputNative);
        DestroyImage(state.keep);
        DestroyImage(state.meter);
        DestroyMeterReadback();

        state.superUp.reset();
        state.superDown.reset();
        state.nrScaler = Scaler::Count;

        if (state.capabilityParams != nullptr)
        {
            NVSDK_NGX_VULKAN_DestroyParameters(state.capabilityParams);
            state.capabilityParams = nullptr;
        }

        if (state.queryPool != VK_NULL_HANDLE && state.device != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(state.device, state.queryPool, nullptr);
            state.queryPool = VK_NULL_HANDLE;
        }

        state.timedFrames = 0;
        state.lastGpuTime.reset();

        state.device = VK_NULL_HANDLE;
        state.width = 0;
        state.height = 0;
        state.ngxInitialised = false;
        state.reset = true;
        if (state.forwarder)
            FreeLibrary(state.forwarder);
        state.forwarder = nullptr;
        state.probe = nullptr;
        state.init = nullptr;
        state.create = nullptr;
        state.evaluate = nullptr;
        state.release = nullptr;
    }
};

ModelVk::ModelVk(DlssNr_Vk& shader) : _impl(std::make_unique<Impl>(shader)) {}
ModelVk::~ModelVk() = default;
bool ModelVk::Evaluate(VkCommandBuffer cmd, const VkImageInfo& colour, const VkImageInfo& depth,
                       const VkImageInfo& motion, const VkImageInfo& output, const DlssNrFrameInfo_Vk& frame,
                       VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, VkImageLayout inputLayout)
{
    return _impl->Evaluate(cmd, colour, depth, motion, output, frame, instance, physicalDevice, device, inputLayout);
}
} // namespace DlssNr
