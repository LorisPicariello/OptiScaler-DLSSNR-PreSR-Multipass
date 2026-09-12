# Matched residual + DLSS

Select **Matched residual + DLSS** under Enlargement with model resolution below 100%.
Use ordinary post-upscale NR or **Apply NR to the finished picture**. Keep **Generate model
before upscale** and the separate generate-before/apply-after placement off for this mode.
The INI value is `[DlssNr] Transfer=2`; the default remains spatial matched residual (`1`).

The reduced model runs on a downsampled version of the already reconstructed picture. Its
linear-proxy difference from its own input is encoded around neutral 0.5, enlarged by an
independent DLSS **SR** feature, decoded, and added to the full-resolution proxy. Existing
composition, HDR transforms, strength and skin controls then operate on that matched pair.
The full-resolution original supplies base detail. Neutwo/Hybrid replace modes decode the
reconstructed full-resolution proxy. Model debug view still shows the actual model answer.
At 100% the private context is released and ordinary full-resolution NR is used.

This is separate from pre-upscale residual-across-RR accumulation. It does not invoke private
RR or change the game's upscaler. Classic and spatial matched residual remain available.

The private pass owns its parameters/history and has unit exposure, no sharpening and no
auto-exposure. Depth and motion are resampled from their active guide regions to model
resolution. Motion is converted to pixels at that resolution. Jitter is zero because the
input picture has already been reconstructed by the game. Finished-picture processing uses
the captured matching depth/motion, reset and frame-time metadata. Frame hold zeroes velocity.
Reset accounting uses NR evaluations, not generated presentation frames.

Initialization learns the producer queue from the actual submission of its command list; the
swapchain's potentially separate Streamline presentation queue is not used as a gate.
Resizes/explicit queue changes and Retry retire the previous context using a dedicated instance
of NR's GPU lifetime tracker, covering only that context's recordings. Unrelated NR recordings
cannot pin it. Active and retired contexts continue receiving submission/reset notifications.
Outstanding retired contexts are bounded. Allocation, initialization
or evaluation failures retain the clean frame and report a status; there is no automatic
spatial fallback. Native Vulkan and pre-upscale placement report unsupported use of this mode.
DX12-based bridges use the shared DX12 implementation. No helper DLL is added.

The new shader operations are appended as 9 and 10; existing operation numbers and constant
layout are unchanged. Matching DX12 and Vulkan binaries/headers are regenerated. WARP tests
cover carrier reconstruction, neutral identity, guide offsets/scales, and existing shader
behavior. Vulkan shader regressions, NR lifetime/proxy tests and the actual private DLSS SR
adapter test are also exercised. Moving-scene quality, finished-picture alignment and bridge
gameplay remain to be verified in-game. The extra DLSS pass costs GPU time and VRAM; the NR
model timer continues to measure the model rather than this additional pass.
