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

## Across-RR residual

`ResidualAcrossRR` with pre-SR placement generates an edit while preserving RR's original Color
input. A signed residual history is reprojected through the active motion-vector rectangle and
accumulated before composition at the post-RR seam. Pairing requires the producing command list,
parameters and output identity; resets and failed dispatches invalidate the residual.
See [the residual contract](../../../docs/RESIDUAL-ACROSS-RR.md).

The separate deferred private-upscaler path applies only to supported SR routes and has its own
history/generation ownership. See [deferred NR](../../../docs/DEFERRED-NR-DLSS.md).
