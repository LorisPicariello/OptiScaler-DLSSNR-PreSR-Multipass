# Pre-SR placement and multipass

`RunBeforeSR` selects an owned color input before SR, including combined RR+SR. Post-SR is the
default. Invalid pre-SR active rectangles select the ordinary post-SR fallback where possible.
Color/output composition requires an origin-zero rectangle; depth and motion have independent
valid rectangles and origins. See [padded color](../../../docs/PADDED-PRESR.md).

Each layer owns a persistent NGX feature and temporal history. `Passes` selects the count;
`UnlockPasses` enables additional layers up to the module limit. Later layers inherit the base
profile unless overridden, with local tone defaulting to zero. A failed extra layer leaves the
ready contiguous prefix active rather than reusing another layer's temporal history.

The codec encodes once. Pass zero reads that immutable base and writes output A. Subsequent
layers alternate A/B. Composition applies the final answer minus the original base once, so
color/transfer controls are not compounded across layers.

Profile, placement, format and working-size changes rebuild the affected model resources.
D3D12 GPU markers protect model creation readiness and retirement; logical submission epochs
coordinate Before/After pairing but do not establish GPU completion. Vulkan uses a creation
event and drains the device before replacing owned model resources.

## Separate edit upscaling, including RR

`DeferredDLSS` runs NR before SR/RR on owned Color and upscales only the edit with an independent
non-RR backend. It applies the result after the game upscaler, or saves it for presentation when
`FinishedPicture` is enabled. `RunBeforeSR` plus legacy `ResidualAcrossRR` selects the same route.
The former temporal accumulator is replaced by the private upscaler's independent history.
See [private edit upscaling](../../../docs/DEFERRED-NR-DLSS.md).
