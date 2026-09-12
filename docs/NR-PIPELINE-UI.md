# Neural Rendering pipeline controls

The NR panel shows the configured colour/edit path. Click a box to open its settings below
the chart. Muted game stages provide context; selecting a box changes only the panel being
viewed. Runtime status remains visible because unsupported configurations can fall back or
fail. The diagram is not a claim that every configured stage is currently executing.

Linear routes stay in one vertical column. Only routes carrying a separate NR edit split
left/right and rejoin at application. Inspection is a separate tool button below the chart.
Enable Neural Rendering and Generate model before upscale sit together above it; their
labels wrap in narrow overlays. The generation toggle keeps the existing before-SR setting
and is checked and disabled while deferred generation forces that placement.

| Section | Existing controls | Role |
| --- | --- | --- |
| Top controls / status | Enable Neural Rendering; Generate model before upscale; runtime status and retry | Starts/stops NR work and chooses generation placement. Timing retains its existing meaning. |
| Game input / placement | Finished-picture mode; generate before/apply after; carry edit across RR; accumulation rate; shortcut guidance | Chooses where colour or the separately generated edit joins the game pipeline. |
| Prepare NR input | Model resolution; downscaler; enlargement; HDR mapping; white-point source; paper white; exposure trim | Prepares the model's working image and controls how its result is resized. Reversible HDR mapping also determines the reconstruction/composition method. |
| Exposure calibration within input preparation | Scan meter; anchors and their white points; scan trim; inversion; advanced candidate readouts | Calibrates the scanned exposure source. These are attached to input preparation, rather than a separate rendering pass. |
| NR model | Pass count; pass 1 and pass 2 style, intensity, local structure, local tone, skin structure and automatic skin mask; resets/inheritance | Generates the edit. The chart shows the configured pass count; a second pass has its own settings/history and additional cost. |
| Apply NR edit | Apply model; detail/colour strengths; highlight guard; separate skin/environment detail and colour; skin-colour permission; mask preview | Controls how the result changes the image. Hiding the edit still runs the model. |
| Inspect NR | Hold frame; comparison mode; swap/labels/label size; side-by-side zoom; wipe split; debug view | Observes the NR boundary through a separate tool button. Later game rendering can still change the displayed image. |

## Routes

- **Normal before SR:** input -> prepare -> model -> apply -> SR/RR -> game effects/HUD -> output.
- **Normal after SR:** input -> SR/RR -> prepare -> model -> apply -> game effects/HUD -> output.
- **Generate before/apply after:** input branches into game SR and NR. NR's edit is upscaled separately and joins after game SR.
- **Carry across RR:** the NR branch accumulates the edit using motion vectors and joins after RR/SR.
- **Finished picture:** preparation, model and application follow the game's effects/HUD.
- **Generate early/apply to finished picture:** NR generates and upscales its edit in a separate branch, then joins after game effects/HUD.
- **NR disabled:** only the linear game path is drawn; NR configuration buttons remain available below it.

The output box represents continuation to frame generation/presentation. It does not move
or configure those systems. Depth and motion guide NR but are not separate colour stages.
Existing restrictions, fallback messages, INI defaults, limits, reset behavior and delayed
slider commits are preserved. Advanced INI-only settings are not newly exposed or removed.

## Validation

All 49 NR option references from the preceding menu remain present. The production chart
was rendered with the repository's ImGui DX11 backend on WARP for all six enabled routes
and the disabled state. The four editable stage boxes and Inspect tool passed click-selection
checks. Both top controls accept clicks on their boxes and wrapped labels; a forced generation
setting stays disabled on both click targets.
Rendering algorithms, parameter routing, shaders, timing and INI serialization are unchanged.

## Finished-picture backend support

The finished-picture stage now accepts native DX12, DX11 through its DX12 bridge, and Vulkan.
Vulkan uses native NR at presentation even for a Vulkan/DX12 upscaler. Early generation with
application to the finished picture remains a DX12/DX11-bridge mode; Vulkan reports that
limitation instead of silently using a different placement. See `NR-FINISHED-BRIDGES.md`.
