#include "pch.h"
#include "DlssNr_GpuLifetime.h"
#include <Util.h>
#include <algorithm>
#include <vector>
#include <wrl/client.h>

namespace DlssNr
{
namespace
{
template <typename T> T* Identity(T* object)
{
    T* real = nullptr;
    return object && Util::CheckForRealObject(__FUNCTION__, object, (IUnknown**) &real) ? real : object;
}
}

struct GpuLifetime::Impl
{
    struct Timeline
    {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        UINT64 value = 0;
        bool failed = false;
        ~Timeline()
        {
            const auto completed = fence ? fence->GetCompletedValue() : 0;
            if (failed || completed == UINT64_MAX || completed < value)
            {
                fence.Detach();
                queue.Detach();
            }
        }
    };
    struct Completion
    {
        std::shared_ptr<Timeline> timeline;
        UINT64 value;
        bool Complete() const
        {
            const auto completed = timeline->fence->GetCompletedValue();
            return completed != UINT64_MAX && completed >= value;
        }
    };
    struct Recording
    {
        ID3D12CommandList* commands = nullptr; // identity only, never dereferenced
        bool open = true;
        bool signalFailed = false;
        // Only the latest value on each queue is needed, including when the list is replayed.
        std::vector<Completion> completions;
        bool Complete() const
        {
            // A closed list can be replayed until reset, even after its first execution completes.
            return !open && !signalFailed &&
                   std::all_of(completions.begin(), completions.end(), [](const auto& c) { return c.Complete(); });
        }
    };
    using Uses = std::vector<std::shared_ptr<Recording>>;
    struct Retired { Uses uses; std::function<void()> destroy; };
    Uses recordings;
    std::vector<Retired> retired;
    std::vector<std::shared_ptr<Timeline>> timelines;
    static bool Complete(const Uses& uses)
    {
        return std::all_of(uses.begin(), uses.end(), [](const auto& use) { return use->Complete(); });
    }
    std::shared_ptr<Timeline> QueueTimeline(ID3D12CommandQueue* queue)
    {
        if (!queue) return {};
        for (const auto& timeline : timelines)
            if (timeline->queue.Get() == queue) return timeline;
        auto timeline = std::make_shared<Timeline>();
        timeline->queue = queue; // keep queue identity stable for the lifetime of its fence
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&timeline->fence))))
            timeline->failed = true;
        timelines.push_back(timeline);
        return timeline;
    }
};

GpuLifetime::GpuLifetime() : impl(std::make_unique<Impl>()) {}
GpuLifetime::~GpuLifetime()
{
    Collect();
    // Pending/unsubmitted, failed-signal and removed-device ownership is intentionally abandoned.
    // Neither elapsed CPU time nor a new queue signal can prove that a recorded list is finished.
}
void GpuLifetime::Record(ID3D12GraphicsCommandList* commands)
{
    if (!commands) return;
    commands = Identity(commands);
    Collect();
    for (const auto& use : impl->recordings)
        if (use->open && use->commands == commands) return;
    auto use = std::make_shared<Impl::Recording>();
    use->commands = commands;
    impl->recordings.push_back(std::move(use));
}
void GpuLifetime::Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (impl->recordings.empty()) return;
    std::vector<Impl::Recording*> matched;
    for (UINT i = 0; i < count; ++i)
    {
        const auto* commands = Identity(lists[i]);
        for (const auto& use : impl->recordings)
            if (use->open && use->commands == commands &&
                std::find(matched.begin(), matched.end(), use.get()) == matched.end())
                matched.push_back(use.get());
    }
    if (!matched.empty())
    {
        const auto timeline = impl->QueueTimeline(Identity(queue));
        // One signal covers every matched list in this actual ExecuteCommandLists notification.
        const bool signalled = timeline && !timeline->failed && timeline->value != UINT64_MAX - 1 &&
                               SUCCEEDED(timeline->queue->Signal(timeline->fence.Get(), ++timeline->value));
        if (timeline && !signalled) timeline->failed = true;
        for (auto* use : matched)
        {
            if (!signalled)
            {
                use->signalFailed = true;
                continue;
            }
            auto& completions = use->completions;
            auto found = std::find_if(completions.begin(), completions.end(),
                                     [&](const auto& c) { return c.timeline == timeline; });
            if (found == completions.end())
                completions.push_back({ timeline, timeline->value });
            else
                found->value = timeline->value;
        }
    }
    Collect();
}
void GpuLifetime::ResetRecording(ID3D12CommandList* commands)
{
    commands = Identity(commands);
    for (auto& use : impl->recordings)
        if (use->commands == commands) use->open = false;
    Collect();
}
void GpuLifetime::Retire(std::function<void()> destroy)
{
    impl->retired.push_back({ impl->recordings, std::move(destroy) });
    Collect();
}
void GpuLifetime::Collect()
{
    std::erase_if(impl->retired, [](auto& item)
    {
        if (!Impl::Complete(item.uses)) return false;
        item.destroy();
        return true;
    });
    std::erase_if(impl->recordings, [](const auto& use) { return use->Complete(); });
}
bool GpuLifetime::Idle()
{
    Collect();
    return impl->recordings.empty();
}
}
