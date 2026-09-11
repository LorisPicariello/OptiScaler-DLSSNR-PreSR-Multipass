# Compatibility changes retained with NR

These changes are separable from Neural Rendering's model behavior. Retaining them does not imply
that every affected game or platform has been validated on the combined branch.

- Vulkan creation queries the actual physical device rather than re-entering DXGI on Proton/DXVK.
  Vulkan framebuffer teardown checks the correct handle, and the Vulkan overlay retains its FG
  interlocks. These include changes reviewed from [y4my4my4m's fork](https://github.com/y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG)
  (`7b7220bb`, including `7c3b65dc`).
- Upscaler output bindings restore on success and failure. D3D11 saved bindings retain COM
  references through restoration; D3D12/Vulkan bridges replace optional exposure/reactive inputs
  with the correct API resource or null before dispatch, then restore the game's parameter values.
  The optional-resource fix was motivated by the Nioh 2 report; see [the issue review](ISSUE-REVIEW-2026-09-10.md).
- The DXGI sizing helpers preserve valid window/backbuffer sizing and the wrapped Present counter
  advances only after a successful real presentation, excluding `DXGI_PRESENT_TEST`. Deferred
  before/after pairs preserve their logical identity even if Present changes between the seams.
  See [the PR review and attribution](PR-REVIEW-20260909.md).
- Reflex/Streamline calls resolve the active loaded feature/interface rather than trusting a stale
  bundled pointer. Struct fields are read only when the available interface version supplies them.
  These bindings remain useful with ordinary frame generation and NR's late/exposure observations.
- Output scaling dispatch dimensions come from the resources supplied to the pass, allowing the
  same scaler to serve ordinary output scaling and NR's private work textures.
- The retail and demo Onimusha executables share the applicable RE Engine state-restoration defaults.
  The model's create/evaluate envelope covers failures as well as success; this remains a candidate
  compatibility fix, not proof that every loading crash is solved. See [NR-COMPATIBILITY.md](NR-COMPATIBILITY.md).
- A reported Cyberpunk Ray Reconstruction/Streamline conflict with the `d3d12.dll` proxy is documented
  in [the installation guide](../INSTALL-DLSSNR.md). That workaround is game-specific.

Core provenance remains [OptiScaler](https://github.com/optiscaler/OptiScaler),
[Dagherbou's NR integration](https://github.com/Dagherbou/OptiScaler_DLSSNR), and the contributors
listed in [CREDITS.md](CREDITS.md). Guide-region attribution is recorded separately in
[NR-MOTION-METADATA.md](NR-MOTION-METADATA.md).
