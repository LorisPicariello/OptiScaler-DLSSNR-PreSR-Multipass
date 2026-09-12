# Finished-picture NR bridges

The finished-picture option processes the completed game image after effects and HUD.
It remains owned by the upscaler's NR shader; presentation hooks route work to that owner.
No extra DLL is introduced.

## DirectX 11

An ordinary DX11 swapchain uses a dedicated shared colour texture. DX11 copies the backbuffer
and signals a shared fence; DX12 waits, processes the picture in COMMON state, and signals back.
DX11 waits before copying a successful result to the backbuffer. The queue ordering protects
texture reuse without a per-frame CPU wait. Resize/teardown drains the final DX11 fence.
The upscaler's texture cache is not reused for presentation.

For the existing DX11-to-DX12 FG swapchain, the completed picture already reaches a DX12
backbuffer. NR runs after the interop copy wait on the presenting queue. This covers FG enabled,
paused and disabled states without creating a second frame-generation feature.

## Vulkan

The Vulkan NR shader owns a presentation stage that copies depth and motion at the upscaler
seam. Submit/Submit2 notifications mark captures submitted; command-buffer/pool reset and free
invalidate abandoned recordings. Each slot has completion protection. Presentation waits on
the game's semaphores and returns an image-indexed semaphore for the original Present call.
NR executes before OptiScaler's overlay. The Vulkan/DX12 upscaler also captures Vulkan guides
for this native presentation stage.

Supported screen encodings are SDR UNORM/sRGB, HDR10 and scRGB. Vulkan blits through a floating
point working image; HDR10 uses the same conversion shader source as DX12. Original images
are retained during model warmup, hidden edits and failed colour preparation.

Vulkan currently requires a tracked primary graphics command buffer on the presenting queue,
a single swapchain in Present, and supported transfer/blit usage. Early generation followed by
application to the finished picture remains limited to DX12 and the DX11 bridge. These conditions
are implementation limits, not fundamental NR restrictions.

## Validation

- Release x64 solution: passed, 26 existing warnings and zero errors.
- Proxy/pipeline regressions: passed.
- RTX 5090 DX11/DX12 transfer regression: unchanged images, edited images, skipped copy-back,
  consecutive reuse, size changes, and RGBA8/RGB10A2/RGBA16F formats passed.
- WARP finished-colour regression: PQ reference luminance, HDR10 round trip, wide gamut, alpha,
  highlights and early-generation residual composition passed.
- Production Vulkan objects linked into a standalone presentation harness: 12 frames each of
  SDR UNORM, SDR sRGB, HDR10 and scRGB passed guide capture, model creation/evaluation and
  semaphore-ordered presentation on RTX 5090. The Khronos validation layer was not installed.
  Flat synthetic images establish execution, not model image quality or game compatibility.
- BG3 DX11 character creation: the installed test build applied NR to the finished 3840x2160
  HDR picture for over 900 frames with FG enabled and over 2,100 additional frames with FG
  disabled. The scene and interface remained visible. HDR desktop captures clip highlights,
  so this does not establish colour accuracy. The FG-disabled session logged frame-count
  warnings; their cause has not been isolated. No existing save or mod configuration was changed.
- BG3 Vulkan in-game verification is still pending.

Local evidence and harness source are under `x64/nr-dx11-finished/`.
