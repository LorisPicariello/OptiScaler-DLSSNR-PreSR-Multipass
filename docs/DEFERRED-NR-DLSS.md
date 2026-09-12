# Generate NR before SR; apply its contribution after SR

Experimental third placement mode. The private contribution upscaler can use **DLSS (default),
FSR 2.2, the installed FidelityFX upscaling runtime, or XeSS**. It has its own context and history;
selecting it never changes the game's main upscaler.

## Enable

Under **DLSS Neural Rendering**, enable **Generate before upscale, apply after upscale**, then
choose **Private NR upscaler**. This also carries the edit across Ray Reconstruction. Enable
**Apply NR to the finished picture** to apply after game effects/HUD instead of immediately after
SR or RR+SR. Or configure:

```ini
[DlssNr]
Enabled=true
DeferredDLSS=true
PrivateUpscaler=0
WorkingScale=1.0
Passes=1
```

Keep **Apply model** on; turn off frame hold, debug/compare views and skin-mask preview. For a first
comparison keep the other rendering settings fixed and use the same model profile, exposure and strengths. Model resolution
is relative to the active render raster, not final output: 100% for a 1080p input runs NR at 1080p.
Existing per-pass controls remain effective. The menu reports the private SR path separately.

`PrivateUpscaler`: `0` = DLSS, `1` = built-in FSR 2.2, `2` = FidelityFX, `3` = XeSS.
Missing, `auto` and invalid values preserve the DLSS default. `DeferredDLSS` keeps its legacy key name.
FidelityFX uses the provider selected by its loaded runtime for the device, independently of the
main pass's provider override. This does not guarantee FSR4 availability. XeSS selects a quality
mode whose supported input range contains the actual carrier dimensions; unsupported sizes fail
cleanly. No sharpening, automatic exposure, game masks or automatic backend fallback are applied.
The same selector applies when generating before SR and applying to the finished picture on D3D12
or its D3D11 bridge, including when the game uses RR. The old RR residual accumulator is replaced
by this same private-upscaler path.

Deferred processing evaluates NR on every rendered frame.

Requires the NVIDIA NR runtime and support for the selected private upscaler. FSR 2.2 reuses the
already-linked library; DLSS, FidelityFX and XeSS reuse OptiScaler's runtime loading infrastructure.
Changing the private upscaler does not remove NR's own hardware/runtime requirements. This mode is
on the D3D12 seam, including the existing D3D11/Vulkan-to-D3D12 bridges.
Native Vulkan skips NR with a diagnostic rather than silently substituting a different placement.
RR uses the same private SR adapter; it never runs another RR feature or modifies RR's input.

## What it computes

1. Copy only the active original colour rectangle to an owned UAV; do not edit the game's colour.
2. Run the existing NR model/passes and low-resolution composition on that copy. The resulting
   difference includes the existing intensity/colour/skin controls; they aren't applied twice.
3. For immediate post-upscale composition, encode `d = (NR-composed - original) / preExposure` as `0.5 + 0.5*d/(1+abs(d))` into an RGBA16F
   carrier. Neutral grey means zero change; values below grey carry darkening, above grey brightening.
4. Let the game's SR or RR+SR operate on the untouched colour. Then run the selected private SR backend on the carrier,
   using separately allocated parameters and history, copied jitter/motion/depth data and unit exposure.
   Calls use the existing runtime proxies or linked FSR2 API, bypassing game-facing feature wrappers.
   The NR-local adapter owns input translation and restores the game's guide resource states. FSR
   camera guides are copied before main SR; missing guides use OptiScaler's existing camera defaults.
5. Decode the enlarged carrier and add the signed edit to a copy of the clean final-resolution raster,
   preserving its alpha. Copy back only after successful private SR evaluation and composition.

This is not a separate physical lighting or shadow buffer. NR returns an edited RGB image; the layer
is inferred from its difference to the original. The signed compression is deliberately experimental.
The private upscaler sees biased/compressed data rather than natural colour and may smooth, distort or temporally
destabilize it. FP16 carrier precision and the nonlinear inverse can amplify errors. The inverse is
clamped to signed magnitude 0.999 before decoding (about 999 times pre-exposure); this prevents poles,
but does not guarantee desirable brightness. Negative final RGB is clamped to zero. No promise of
matching full-resolution NR or restoring the reported gun-rack shadows is made.

For finished-picture composition, step 3 instead encodes a bounded relative RGB change as log-gain
around neutral grey. After private upscaling, the matching edit and optional clean scene reference
are retained in fence-protected presentation slots. SDR, scRGB and HDR10 use the existing
linear-light transfer. No edit is copied to the game's SR/RR output on that route.

## Failure and lifetime behaviour

- Unsupported layouts, non-zero subrect offsets, missing guides, allocation/runtime/evaluation failure
  or unmatched before/after calls retain the clean main-SR result. No alternative residual upscaler runs.
- Private creation and evaluation are separated by a submission epoch. Camera cuts, disabled/missed
  frames and generation changes reset private history. Main-game parameters/handles are never edited.
- Backend/resolution/format/device/queue, RR-mode and composition-destination changes create a new generation. Retired histories, shaders and buffers
  are released only after the last recorded GPU timestamp completion marker is visible. Completion slots
  also limit outstanding work; retired generations are bounded, with clean-frame fallback under backlog.
- Only one upscale per submission epoch and a known same-device direct queue are supported. Multi-view,
  asynchronous-compute and unusual engine submission patterns require further work/testing.
- If GPU work cannot be confirmed complete at shutdown, its generation is retained for process teardown
  instead of releasing in-flight resources. A private runtime failure latches for its generation; restart
  the game to retry reliably. Turning the option off restores the selected existing placement.
- NR's existing GPU timer measures NR work, **not** the extra private SR pass and final-resolution copies/
  composition. Compare total frame time; this mode costs more than ordinary pre-SR NR and uses more VRAM.

## Validation

- Private backend extension: Release x64 build passed. A headless hardware harness included the actual
  NR adapter with only loader/config/parameter dependency seams. FSR 2.2, FidelityFX and XeSS each
  created two live contexts and evaluated 1080p-to-4K neutral/signed carriers across eight frames.
  All returned exact band-centre values of `0.5` and `0.25/0.5/0.75`. Guide textures starting in UAV
  state exercised transition/restoration, and missing-depth evaluation was rejected. The D3D12 debug
  layer was unavailable; checks used API results, GPU fences and output readback. This verifies the
  adapter with real upscalers, not in-game scheduling, motion quality or OptiScaler's runtime loading.

- Shared HLSL WARP smoke tests: signed shadow/brightening roundtrip, neutral identity, alpha preservation,
  non-finite/overshoot guards and fixed unit exposure passed; existing skin-control tests also passed.
- Headless RTX 5090 test using the installed NVIDIA NGX driver and an existing, signature-verified official
  SR DLL: two distinct DLSS feature handles created, and 1080p carriers upscaled to 4K over eight frames.
  Neutral samples stayed exactly 0.5; dark/neutral/bright band centres returned 0.25/0.5/0.75.
- That hardware test uses synthetic static inputs and real DLSS, not an NR model, game injection or the
  complete before/after hook. Moving-scene alignment, private-pass scheduling in games, FG compatibility,
  visual quality and performance require separate live-game validation. Experimental builds have been
  installed locally in BG3 and Jedi Survivor; those installations are not a validation of this cleanup.

Reproduce from an x64 VS developer prompt:

```bat
cl /nologo /std:c++20 /EHsc /Iexternal\nvngx_dlss_sdk tests\nr_residual_dlss_smoke.cpp /Fe:x64\nr_residual_dlss_smoke.exe /Fo:x64\nr_residual_dlss_smoke.obj /link d3d12.lib dxgi.lib
x64\nr_residual_dlss_smoke.exe "FULL PATH TO INSTALLED nvngx.dll" "DIRECTORY CONTAINING YOUR OFFICIAL nvngx_dlss.dll"
```

The test loads user-supplied local DLLs and does not download, redistribute or inject them into a game.
