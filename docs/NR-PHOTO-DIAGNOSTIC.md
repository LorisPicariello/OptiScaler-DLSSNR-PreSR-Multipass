# Gameplay / photo-mode pipeline capture

Diagnostic capture for the ordinary native DX12 pre-upscale NR path. It does not change NR or RR
settings, reset their histories, or evaluate another upscaler. No helper DLL is required.

1. Enable NR and **Generate model before upscale**. Disable the separate generate-before/apply-after
   option, finished-picture application, frame hold, and comparison/debug views. Keep the model,
   exposure, pass count and game graphics settings unchanged throughout the comparison.
2. In gameplay where the missing effect is visible, press **Ctrl+F8** once. Remain in gameplay for
   at least five seconds to let the readbacks finish.
3. Enter the live photo-mode view, preferably retaining the same framing. Once the expected NR effect
   is visible, press **Ctrl+F9** once. Remain there for at least five seconds.
4. Exit the game normally and inspect `bin/x64/nr-pipeline-captures` and `OptiScaler.log`.

The shortcuts only operate while the game has focus and ordinary native DX12 pre-upscale NR is active.
Each request records four successive eligible evaluations into its own timestamped folder. The labels
are supplied by the shortcuts: the diagnostic does not claim to detect the game's photo-mode state.
There are at most two requests per process and 256 MiB of image readbacks per frame (2 GiB total).
Capturing and saving can briefly hitch; captured runs are not suitable for performance measurements.
Existing captures are preserved.

Each frame directory contains raw GPU textures and `manifest.txt`:

- `before_nr`: original scene-linear colour before NR preparation.
- `after_nr`: the exact colour texture supplied to the main SR/RR evaluation after NR composition.
- `after_rr`: the main upscaler's output before OptiScaler's subsequent shader passes. The filename
  is fixed; consult `rr` in the manifest to distinguish SR from RR.
- `motion`, `depth`, `exposure`: the source guide textures, where supported and present.

Use each manifest's `storedFormat`, dimensions and `rowPitch` to decode the raw data. The entire texture
allocation is copied; the logged render subrect identifies the active raster. Metadata includes game
reset, NR's pending history reset, model-evaluated status, input replacement status, jitter, motion scale,
exposure factors, frame-time input, feature flags and selected RR guide identities/descriptors.
Missing/unsupported resources and allocation/budget skips are explicit in the manifest.

Readbacks share production NR lifetime tracking: they are mapped only after recordings close and all
their submissions complete. An end timestamp distinguishes executed work from discarded recordings.
No frame-count delay is used as evidence of GPU completion. Failed evaluations remain marked failed.

Validation: `tests/run_nr_pipeline_capture.ps1` executes the production capture and lifetime code on
WARP, verifying stage pixel values, a blocked GPU queue, retirement and a discarded recording. This
does not establish the cause of the Cyberpunk visual difference; the two in-game captures are required.
