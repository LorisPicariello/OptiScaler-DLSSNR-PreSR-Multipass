# NR integration and compatibility prerequisites

Game-validation builds include all compatibility fixes together with NR. KCD2 driver-query/
waitable-swapchain changes, Streamline binding/capability fixes, Vulkan overlay/FG interlocks and
DXGI presentation support remain enabled. They are separate upstream review units, not optional
omissions from installed test builds. `codex/nr-game-validation` is the combined build branch.

- Streamline feature functions are resolved through the active interposer after device binding.
  NVIDIA driver overrides can initialize a different plugin from the bundled DLL even with
  application OTA disabled. Calling the bundled Reflex export then crashes during startup.
  The installed-runtime regression is `tests/streamline_active_plugin_smoke.cpp`; it reproduces
  the old access violation and checks the active Reflex call without launching a game.
- D3D11 saved bindings retain COM references through restoration. D3D12/Vulkan bridges replace
  optional exposure/reactive inputs with the correct API resource or null before dispatch, then
  restore the game's values. This prevents a foreign-API pointer reaching a D3D12 consumer.
- DXGI window-sized/composition-swapchain support remains a prerequisite of the finished-picture
  routes. It resolves dimensions against the real window, preserves caller descriptors and
  composition flags, and calls the original factory once. Composition window association is
  best effort; multi-window applications need runtime validation. This work incorporates
  [janblade's PR #2](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/pull/2).
- Successful real presentations advance the wrapped counter; `DXGI_PRESENT_TEST` does not.
  Deferred Before/After pairs retain their logical identity across a mid-evaluate Present.
  Provenance: [PR #8](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/pull/8)
  (`4a96e741`) and [PR #11](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/pull/11)
  (`c9978c78`). GPU completion markers remain separate from this logical frame identity.
- Vulkan physical-device identification and framebuffer checks include contributions from
  [y4my4my4m's fork](https://github.com/y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG)
  (`7b7220bb`, including `7c3b65dc`). Optional NR preparation is gated before device/swapchain
  creation; late enabling may require restarting. See [native Vulkan integration](NR-VULKAN.md).
- Ordinary output scaling retains feature target/display dimensions. NR explicitly calls
  `DispatchResources` for its private textures, with a per-instance filter override.
- NR's create/evaluate state envelope restores caller bindings on failures and successful calls.
  The Onimusha defaults and remaining RE Engine limits are described in
  [NR-COMPATIBILITY.md](NR-COMPATIBILITY.md).

The focused DXGI regression covers real WARP swapchains, window sizing and composition:

```powershell
cl /nologo /std:c++20 /EHsc tests\dxgi_window_size_smoke.cpp /Fe:x64\dxgi_window_size_smoke.exe /Fo:x64\dxgi_window_size_smoke.obj /link d3d11.lib dxgi.lib dcomp.lib user32.lib
x64\dxgi_window_size_smoke.exe
```

Synthetic checks do not establish compatibility with every game. The game-specific Cyberpunk
proxy workaround is documented in the [installation guide](../INSTALL-DLSSNR.md).
Core provenance and licenses are retained in [CREDITS.md](CREDITS.md); guide-region attribution
is recorded in [NR-MOTION-METADATA.md](NR-MOTION-METADATA.md).
