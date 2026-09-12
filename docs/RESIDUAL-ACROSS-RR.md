# Carrying a pre-upscale NR edit across RR

Use **Generate before upscale, apply after upscale** for both SR and RR. NR edits an owned copy
of the original input. The game processes clean Color through SR or RR+SR, while a separate
non-RR DLSS, FSR 2.2, FidelityFX or XeSS context upscales the NR edit. Both paths share the same
**Private NR upscaler** selector and generation/history lifecycle.

Also enable **Apply NR to the finished picture** to defer composition until presentation, after
game effects and HUD. The game's RR output stays clean. This uses the existing bounded log-gain
carrier and SDR/scRGB/HDR10 transfer, including optional HDR response matching.

The former **Carry the pre-SR edit across RR** checkbox and motion-guided accumulator are replaced
by this unified route. Old `RunBeforeSR=true` plus `ResidualAcrossRR=true` INIs select it too.
`ResidualAcrossRRBlend` is retained when saving old INIs but no longer controls processing;
temporal reconstruction belongs to the private upscaler. Its histories are independent of RR.

The private path runs on D3D12 and its bridges. Native Vulkan has no private SR adapter and
reports that limitation. Early edit plus finished-picture composition supports D3D12 and the
D3D11 bridge. Unsupported offsets, missing guides or unmatched/failed evaluations retain the
clean game frame. See [private edit upscaling](DEFERRED-NR-DLSS.md) for configuration and limits.

Synthetic checks cover routing, matching seams, independent private contexts using SR-only NGX
features, signed/neutral carrier reconstruction, and finished-picture HDR/SDR transfer. They do
not establish moving-scene quality with noisy ray-traced input; gameplay verification remains necessary.
