# Private Ray Reconstruction for NR residuals

When the private upscaler is DLSS, native DX12 games already providing RR inputs now
use an independent Ray Reconstruction feature for the encoded residual image.
This applies to both post-upscale and finished-picture residual application. The
separate-edit placement option remains manual; there is no new setting or helper DLL.

The pre-upscale seam snapshots diffuse/specular albedo, normals, roughness,
reflection motion and hit-distance guides, their offsets, and camera matrices.
The private feature receives the encoded residual as colour and its own output,
unit exposure and temporal history. Game colour layers and game output resources
are not forwarded as private RR colour/output. Borrowed guide states are restored;
aliased inputs transition once. Creation properties and guide availability changes
retire the previous generation through the existing GPU lifetime mechanism.

Missing or incompatible RR guides use ordinary private DLSS SR. DX11/Vulkan bridges
do not currently transfer RR material/reflection guides, so they retain SR. Native
Vulkan's separate private implementation is unchanged. Other selected private
upscalers are unchanged. RR creation/evaluation failures retain the clean game frame,
and status/log messages identify the actual private DLSS SR or DLSS RR path.

The NGX keys and required inputs follow NVIDIA's
[RR integration guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md)
and `nvsdk_ngx_defs_dlssd.h`. Applying RR to encoded edits is experimental: the game
guides describe the scene, rather than the residual signal.

Validation: the production adapter hardware smoke passed for two independent RR
contexts at 1920x1080 -> 3840x2160. Neutral 0.5 produced 0.498779; signed test regions
0.25/0.5/0.75 produced 0.249023/0.498779/0.749512. This checks finite output and broad
signal preservation, not exact neutrality or temporal quality. Snapshot ownership,
missing guides and guide bounds were checked. The ordinary DLSS fallback hardware
smoke also passed. The D3D12 debug layer was unavailable. In-game image quality,
motion stability and VRAM behaviour remain unverified.

The Cyberpunk run starting 22:24:34 on 12 September reported private RR creation
failure at 22:50:27 with RR guides available. That build discarded the NGX return
code, so its log cannot establish the reason. The follow-up aligns private NGX
creation with the main DLSSD heap-capture bypass and reports allocation/creation/
evaluation return codes. NGX messages are promoted only on the creation thread
inside that call; routine per-frame driver logging remains unchanged.

The updated hardware test uses Cyberpunk's application ID, 2560x1440 -> 3840x2160
Quality mode, inverted depth, and coexisting HDR scene/LDR residual RR instances.
Both creations and evaluations succeeded. High-resolution motion flags also passed
in a separate probe. Thus this test has not reproduced the game failure. The heap
bypass is a compatibility correction, not a verified explanation of that failure;
an in-game retry is needed, with detailed diagnostics if it still fails.

The next run identified `CreateFeature` result `0xBAD00005` (invalid parameter).
Its logged resolution, quality, depth and roughness settings match the passing
offscreen test. Private RR creation now explicitly sets output-subrect support to
zero and writes feature flags with the signed integer overload, matching NVIDIA's
RR creation helper. This removes a dependency on missing-field defaults; whether
it resolves Cyberpunk's creation rejection still requires an in-game retry.
The later access violation was mapped separately to reentrant GPU resource cleanup;
see `NR-GPU-RETIREMENT.md`.
