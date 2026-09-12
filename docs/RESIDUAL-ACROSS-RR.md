# Carrying a pre-upscale NR edit across RR

Use **Generate before upscale, apply after upscale** for both SR and RR. NR edits an owned copy
of the original input. The game processes clean Color through SR or RR+SR, while a separate
DLSS, FSR 2.2, FidelityFX or XeSS context upscales the NR edit. DLSS uses private RR when compatible
native DX12 RR guides are available, otherwise SR. Both paths share the same
**Private NR upscaler** selector and generation/history lifecycle.

Also enable **Apply NR to the finished picture** to defer composition until presentation, after
game effects and HUD. The game's RR output stays clean. This uses the existing bounded log-gain
carrier and SDR/scRGB/HDR10 transfer, including optional HDR response matching.

On the RR route, the v0.7.7 motion-guided accumulator runs before private upscaling. It forms the
signed scene-linear difference `NR edited - original`, reprojects the previous difference using
the game's motion vectors, then blends in the current difference. `ResidualAcrossRRBlend` controls
this blend (default 0.08, range 0.01..1). Cold/reset history and invalid motion fade in from zero.
Two render-resolution FP32 histories belong to each generation and retire with its GPU resources.
At 2560x1440 these add 112.5 MiB. Camera cuts, skipped/unmatched evaluations and mode changes use
the existing private-generation reset rules, so the accumulator cannot reuse invalid history.

The accumulated difference is composed at the same input resolution and passed through the existing
signed/log-gain carrier encoder. The private upscaler performs the actual enlargement; the old
bilinear render-to-output enlargement is not used. The game receives its unchanged input and the
enlarged edit is applied afterward. The finished-picture HDR transfer remains available.

Old `RunBeforeSR=true` plus `ResidualAcrossRR=true` INIs select this separate-edit route too.
Ordinary pre-upscale NR remains manual and does not acquire residual accumulation automatically.
The separate accumulator and private upscaler each maintain their own temporal history. As in
v0.7.7 there is no depth-based disocclusion rejection; smearing, lag and the additional temporal
filtering need in-game evaluation.

The private path runs on D3D12 and its bridges. Native Vulkan has no private SR adapter and
reports that limitation. Early edit plus finished-picture composition supports D3D12 and the
D3D11 bridge. Unsupported offsets, missing guides or unmatched/failed evaluations retain the
clean game frame. See [private edit upscaling](DEFERRED-NR-DLSS.md) for configuration and limits.

Synthetic checks cover matching seams, motion reprojection, cold/warm/reset history, input-resolution
composition, independent private SR/RR contexts, signed/neutral carrier reconstruction, and HDR/SDR transfer. They do
not establish moving-scene quality with noisy ray-traced input; gameplay verification remains necessary.
