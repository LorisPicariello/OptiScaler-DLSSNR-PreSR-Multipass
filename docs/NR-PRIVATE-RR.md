# Private Ray Reconstruction for NR residuals

When the private upscaler is DLSS, native DX12 games supplying valid RR inputs use
an independent Ray Reconstruction feature for the encoded residual image. This
applies to post-upscale and finished-picture residual application. Separate-edit
placement remains manual; no new setting or helper DLL is required.

The pre-upscale seam snapshots diffuse/specular albedo, normals, roughness,
reflection motion and hit-distance guides, offsets, and camera matrices. Private
RR receives the encoded residual as colour and its own output and history. It is
created in **linear HDR mode with exposure fixed at 1**. The carrier stays bounded
and its encoding/decoding is unchanged; this does not apply PQ or game exposure to
it. Game colour layers and game output resources are not used as private RR inputs
or outputs. Borrowed guide states are restored and aliased inputs transition once.

Missing/incompatible RR guides use ordinary private DLSS SR. DX11/Vulkan bridges
currently do not transfer RR material/reflection guides and retain SR. Native
Vulkan's separate private implementation and other selected private upscalers are
unchanged. Changes to creation properties retire the prior generation through the
existing GPU lifetime mechanism. Failure retains the clean game frame; status/logs
identify the backend and exact NGX error.

## Cyberpunk initialization rejection

Cyberpunk's executable-name NVIDIA profile rejects LDR RR creation with
`0xBAD00005` (`NVSDK_NGX_Result_FAIL_InvalidParameter`). The same call succeeds
under the standalone test's normal executable name. Application ID, resolution,
quality, depth flags, and basic RR coexistence tests alone did not reproduce it.

The production-adapter regression now supports `-CyberpunkProfile`, running the
**offscreen harness** as `Cyberpunk2077.exe` in the test output folder. It does not
launch the game. Against the prior adapter the second RR creation fails with the
same `BAD00005`. Adding `NVSDK_NGX_DLSS_Feature_Flags_IsHDR` for private RR makes both
creations and evaluations succeed, with a clean process exit. Ordinary SR retains
its existing LDR creation mode. Explicit creation fields and heap-capture bypass
continue to match the NVIDIA helper and game-facing DLSSD path.

## Validation and limitations

The fixed profile test uses Cyberpunk's application ID, 2560x1440 -> 3840x2160
Quality mode, inverted depth, and two independent RR contexts. Neutral 0.5 returned
0.500488/0.499268/0.499268 at the sampled positions. Signed test regions
0.25/0.5/0.75 returned 0.250000/0.500000/0.749512. This validates creation and broad
signal preservation, not exact neutrality or temporal image quality. The D3D12
debug layer was unavailable. In-game quality, performance and VRAM behaviour still
require testing.

RR on encoded residuals is experimental: game guides describe the scene rather
than the residual. NGX keys and required guides follow NVIDIA's
[RR integration guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md)
and `nvsdk_ngx_defs_dlssd.h`.
