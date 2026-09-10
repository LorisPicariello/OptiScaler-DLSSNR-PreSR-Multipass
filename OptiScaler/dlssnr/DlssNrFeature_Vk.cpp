#include "pch.h"

#include "DlssNrFeature_Vk.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <NVNGX_Parameter.h>
#include <nvsdk_ngx_vk.h>

#include <shaders/dlssnr/DlssNr_Vk.h>

#include <memory>
#include <mutex>
#include <dlssnr/DlssNr_Status.h>
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

struct ModelSettings
{
    unsigned int preset, style;
    float intensity, localStructure, localTone, skinStructure;
    bool autoMask;
    bool operator==(const ModelSettings&) const = default;
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
    NVSDK_NGX_Parameter* capabilityParams = nullptr;

    // What the model writes, the proxy it is shown, and the frame as the upscaler left it.
    OwnedImage output;
    OwnedImage proxy;
    OwnedImage keep;

    DlssNr_Vk* pass = nullptr;
    bool saidExposure = false;
    bool saidEncoding = false;
    bool reported = false;
    std::optional<bool> beforeUpscale;

    uint32_t width = 0;
    uint32_t height = 0;
    bool reset = true;
    std::optional<ModelSettings> settings;
    unsigned long long frames = 0;

    // Timing. A pair of timestamps per frame across a ring, read back three frames later: a query
    // read the frame it was written stalls the CPU on the GPU, which would cost more than the pass
    // it is measuring. Vulkan reports ticks, and timestampPeriod is how many nanoseconds a tick is.
    VkQueryPool queryPool = VK_NULL_HANDLE;
    float timestampPeriod = 0.0f;
    unsigned long long timedFrames = 0;
    std::optional<double> lastGpuTime;

    // Whether the game hands over an exposure texture. Observed, not consumed -- see where it is set.
    bool exposureOffered = false;
};

// Four frames of pairs. Three would do, four keeps the modulo cheap and the slot being written well
// clear of the slot being read.
constexpr uint32_t kTimingSlots = 4;

struct ModelVk::Impl
{
    VkState state;
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
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

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
            return state.create != nullptr;

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
        state.evaluate = (PFN_VkEvaluate) GetProcAddress(state.forwarder, "dlssnr_vk_evaluate");
        state.release = (PFN_VkRelease) GetProcAddress(state.forwarder, "dlssnr_vk_release");

        if (state.init == nullptr || state.create == nullptr || state.evaluate == nullptr)
        {
            Fail("the forwarder is missing its Vulkan entry points");
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

    bool Evaluate(VkCommandBuffer cmdBuffer, const VkImageInfo& input, const VkImageInfo& depthInfo,
                  const VkImageInfo& motionInfo, const VkImageInfo& target, const DlssNrFrameInfo_Vk& frame,
                  VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, VkImageLayout inputLayout)
    {
        auto& cfg = *Config::Instance();

        if (!cfg.DlssNrEnabled.value_or_default())
            return false;

        if (cmdBuffer == VK_NULL_HANDLE || device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE)
            return false;

        std::lock_guard<std::mutex> lock(mutex);

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
        struct ReportStatus
        {
            Impl* owner;
            ~ReportStatus()
            {
                const auto& state = owner->state;
                ExposureStatus exposure {};
                exposure.seenFrames = state.frames;
                exposure.offeredNow = exposure.everOffered = state.exposureOffered;
                PublishStatus(owner, Backend::Vulkan,
                              { state.feature != nullptr && !state.failed, state.reason, state.lastGpuTime,
                                state.frames, exposure, false });
            }
        } report { this };
        if (state.failed)
            return false;

        auto wrap = [](const VkImageInfo& image)
        {
            NVSDK_NGX_Resource_VK resource {};
            resource.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
            resource.Resource.ImageViewInfo = { image.ImageView, image.Image, image.SubresourceRange,
                                                image.Format,    image.Width, image.Height };
            return resource;
        };
        auto colourResource = wrap(input);
        auto depthResource = wrap(depthInfo);
        auto motionResource = wrap(motionInfo);
        depthResource.ReadWrite = frame.DepthReadWrite;
        motionResource.ReadWrite = frame.MotionReadWrite;
        auto* colour = &colourResource;
        auto* depth = &depthResource;
        auto* motion = &motionResource;
        state.exposureOffered = frame.ExposureTexture != nullptr;
        if (!input.Image || !target.Image || !depthInfo.Image || !motionInfo.Image)
            return false;

        const uint32_t width = frame.Width ? frame.Width : input.Width;
        const uint32_t height = frame.Height ? frame.Height : input.Height;
        const uint32_t guideWidth = frame.GuideWidth ? frame.GuideWidth : depthInfo.Width;
        const uint32_t guideHeight = frame.GuideHeight ? frame.GuideHeight : depthInfo.Height;

        if (!width || !height || width > input.Width || height > input.Height || width > target.Width ||
            height > target.Height || guideWidth > depthInfo.Width || guideHeight > depthInfo.Height ||
            guideWidth > motionInfo.Width || guideHeight > motionInfo.Height)
            return false;

        state.instance = instance;
        state.physicalDevice = physicalDevice;

        // A device change invalidates everything. Rebuild rather than reuse handles from a dead device.
        if (state.device != device)
        {
            Shutdown();
            state.device = device;
            state.instance = instance;
            state.physicalDevice = physicalDevice;
        }

        if (!LoadForwarder())
            return false;

        // Initialise NGX on this device, once. The snippet path is the model itself; the forwarder loads
        // it so the caller gate sees a module named nvngx.dll.
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
        if (state.beforeUpscale != frame.BeforeUpscale || frame.Reset)
            state.reset = true;
        state.beforeUpscale = frame.BeforeUpscale;

        const ModelSettings settings {
            cfg.DlssNrPreset.value_or_default(),    cfg.DlssNrStyle.value_or_default(),
            cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
            cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
            cfg.DlssNrAutoMask.value_or_default()
        };
        const bool resized = state.width != width || state.height != height;
        // The model reads these settings at creation. Retire outstanding work before releasing any
        // feature or image, including when a pre/post placement change changes the working resolution.
        if (resized || state.settings != settings)
        {
            if ((state.feature || state.output.Valid()) && vkDeviceWaitIdle(device) != VK_SUCCESS)
            {
                Fail("the Vulkan device could not retire the previous model resources");
                return false;
            }
            if (state.feature != nullptr && state.release != nullptr)
            {
                state.release(state.feature);
                state.feature = nullptr;
            }

            const VkFormat working = VK_FORMAT_R16G16B16A16_SFLOAT;

            if (resized && (!CreateImage(state.output, width, height, working, true) ||
                            !CreateImage(state.proxy, width, height, working, true) ||
                            !CreateImage(state.keep, width, height, working, true)))
            {
                Fail("the pass could not allocate its own surfaces");
                return false;
            }

            state.settings = settings;
            state.width = width;
            state.height = height;
            state.reset = true;
        }

        if (state.feature == nullptr)
        {
            state.feature = state.create(
                (void*) cmdBuffer, state.capabilityParams, width, height, (int) cfg.DlssNrPreset.value_or_default(),
                cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
                cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
                cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1);

            if (state.feature == nullptr)
            {
                Fail("the model would not build a feature on this device");
                return false;
            }

            LOG_INFO("DLSS-NR Vulkan: feature up at {}x{}", width, height);
            state.reset = true;
        }

        // -----------------------------------------------------------------------------------------
        // Encode: the frame the upscaler wrote -> a display-referred proxy, plus an untouched copy
        // -----------------------------------------------------------------------------------------

        const bool gameSaysHdr = frame.ColourIsLinearHdr;
        const bool depthInverted = frame.DepthInverted;

        // Both have to agree. A game can set the HDR flag on a buffer that cannot hold open-ended light,
        // and encoding an already tone-mapped frame a second time looks washed out and banded.
        const bool linearHdr = gameSaysHdr && FormatCanHoldLinearHdr(colour->Resource.ImageViewInfo.Format);

        // The slider only. The exposure source rides on the D3D12 meter's readback, which has no Vulkan
        // counterpart yet, so this path is deliberately manual rather than quietly reading nothing.
        const float whitePoint = cfg.DlssNrWhitePointScale.value_or_default();

        if (!state.saidEncoding)
        {
            state.saidEncoding = true;
            LOG_INFO("DLSS-NR Vulkan: the game's buffer is {} (flag {}, format {}), depth {}",
                     linearHdr ? "linear HDR" : "already tone-mapped", gameSaysHdr ? "set" : "clear",
                     (int) colour->Resource.ImageViewInfo.Format, depthInverted ? "inverted" : "normal");
        }

        auto encode = DlssNr_Common::MakeConstants(DlssNrMode_Encode, width, height, whitePoint, linearHdr, cfg);
        encode.GuideWidth = guideWidth;
        encode.GuideHeight = guideHeight;
        encode.MvScaleX = frame.MvScaleX;
        encode.MvScaleY = frame.MvScaleY;

        const VkImageSubresourceRange colourRange = colour->Resource.ImageViewInfo.SubresourceRange;

        // Open the measurement. Reset immediately before writing: a query pool slot must be reset before
        // it is written again, and doing it here rather than at the end keeps the two in one place.
        const uint32_t timingSlot = (uint32_t) (state.timedFrames % kTimingSlots);

        if (state.queryPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(cmdBuffer, state.queryPool, timingSlot * 2, 2);
            vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, state.queryPool, timingSlot * 2);
        }

        // The pipeline supplies GENERAL images. Match sampled descriptors and restore the input even
        // if model creation or dispatch fails; the output remains GENERAL for the following pass.
        TransitionForeign(cmdBuffer, input.Image, input.SubresourceRange, inputLayout,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        struct RestoreInput
        {
            Impl* owner;
            VkCommandBuffer cmd;
            VkImageInfo image;
            VkImageLayout layout;
            ~RestoreInput()
            {
                owner->TransitionForeign(cmd, image.Image, image.SubresourceRange,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layout);
            }
        } restore { this, cmdBuffer, input, inputLayout };
        Transition(cmdBuffer, state.proxy, VK_IMAGE_LAYOUT_GENERAL);
        Transition(cmdBuffer, state.keep, VK_IMAGE_LAYOUT_GENERAL);

        if (!state.pass->Dispatch(cmdBuffer, encode, width, height, colour->Resource.ImageViewInfo.ImageView,
                                  VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, state.proxy.view, state.keep.view))
        {
            Fail("the encode dispatch failed");
            return false;
        }

        // -----------------------------------------------------------------------------------------
        // The model
        // -----------------------------------------------------------------------------------------

        Transition(cmdBuffer, state.output, VK_IMAGE_LAYOUT_GENERAL);

        const int evaluated =
            state.evaluate((void*) cmdBuffer, state.feature, state.capabilityParams, &state.proxy.ngx, depth, motion,
                           &state.output.ngx, width, height, guideWidth, guideHeight, depthInverted ? 1 : 0,
                           state.reset ? 1 : 0, cfg.DlssNrIntensity.value_or_default(),
                           (int) cfg.DlssNrStyle.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                           cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                           cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, frame.MvScaleX, frame.MvScaleY);

        state.reset = false;

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

        Transition(cmdBuffer, state.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, state.output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, state.keep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        if (!state.pass->Dispatch(cmdBuffer, resolve, width, height, state.proxy.view, state.output.view,
                                  state.keep.view, VK_NULL_HANDLE, target.ImageView, VK_NULL_HANDLE))
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

        state.frames++;
        if (!state.reported && state.frames > 2)
        {
            state.reported = true;
            LOG_INFO("DLSS-NR Vulkan: running natively at {}x{}, guides {}x{}", width, height, guideWidth, guideHeight);
        }
        return true;
    }

    void Shutdown()
    {
        if (state.feature != nullptr && state.release != nullptr)
            state.release(state.feature);

        state.feature = nullptr;

        DestroyImage(state.output);
        DestroyImage(state.proxy);
        DestroyImage(state.keep);

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
        state.settings.reset();
        state.reset = true;
        if (state.forwarder != nullptr)
            FreeLibrary(state.forwarder);
        state.forwarder = nullptr;
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
