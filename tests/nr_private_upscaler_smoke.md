# Private NR upscaler GPU smoke

Run from PowerShell with Visual Studio C++ build tools and the Windows SDK installed:

```powershell
./tests/run_nr_private_upscaler_smoke.ps1 -Backend FSR22
./tests/run_nr_private_upscaler_smoke.ps1 -Backend FFX -Runtime 'C:/installed/amd_fidelityfx_upscaler_dx12.dll'
./tests/run_nr_private_upscaler_smoke.ps1 -Backend XeSS -Runtime 'C:/installed/libxess.dll'
./tests/run_nr_private_upscaler_smoke.ps1 -Backend DLSS -Runtime 'C:/installed/nvngx.dll' -SrDirectory 'C:/installed/DLSS'
```

The runner discovers Visual Studio through `vswhere`; `-VcVars` can specify a different `vcvars64.bat`. It builds into ignored `x64/nr-private-upscaler-smoke`. Supply your existing official runtime binaries; nothing is downloaded or installed. FSR 2.2 uses the repository's linked libraries. The DLSS directory must contain `nvngx_dlss.dll`. DLSS selects NVIDIA hardware; other backends select the first non-software D3D12 adapter and may reject unsupported hardware/runtime versions.

The harness compiles the exact production `DlssNr_Upscaler_Dx12.cpp` via an include. Its include path substitutes empty application PCH/proxy headers, and its small loader seams call the supplied DLL exports directly. The application's global spoofing/heap-capture guards are inert in this standalone process. These seams bypass application loader discovery and hooking only; backend creation, resource transitions, frame translation, evaluation and destruction are production code. The runner therefore does not validate the game's runtime discovery or hook interaction.

Two simultaneous contexts process a neutral 0.5 carrier and signed 0.25/0.5/0.75 regions from 1080p to 4K. Eight evaluations per case exercise reset and persistent history. Inputs use fixed unit exposure, zero motion and no sharpening. Guides arrive in UAV state to exercise the explicit transition/restoration contract. Readback checks finite pixels, neutral preservation and signed-region preservation. A missing-guide dispatch must fail. Every submission waits on a fence before allocator reset and context destruction. D3D12 debug messages are checked when the SDK debug layer is available; otherwise the runner clearly reports its absence.

This is a static synthetic smoke test, not moving-scene quality, game integration, backend-switch stress, or delayed/replayed-command lifetime coverage. The NR owner must keep a generation alive until its commands complete, including failed creation/evaluation.

Validated locally on 2026-09-12 with installed runtimes: DLSS, linked FSR 2.2, FidelityFX upscale API and XeSS all passed the production-adapter smoke. The D3D12 debug layer was unavailable for these runs, so validation was GPU completion and output readback only.

The DLSS loader seam additionally asserts that both private NGX contexts are created as
`NVSDK_NGX_Feature_SuperSampling`, never Ray Reconstruction. RR is owned only by the game.
