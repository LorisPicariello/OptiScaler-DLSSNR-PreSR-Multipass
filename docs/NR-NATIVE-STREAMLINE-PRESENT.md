# Finished-picture NR with native Streamline frame generation

Cyberpunk's first finished-picture test froze after a GPU fence timeout. Removing waits on
unfinished cross-queue NR input avoided that hazard, but the next run recorded thousands of
successful dispatches while the visible effect appeared only intermittently. The dispatch
log confirmed that the producer and presentation queues differed.

Streamline's interposer passes its base swapchain to plugin before-present callbacks and
redirects `GetBuffer` and `GetCurrentBackBufferIndex` through the DLSSG plugin. The base
swapchain's display buffers are therefore not the game's finished buffers during native FG.
Writing the display buffer underneath Streamline can be overwritten by its later copy.
See NVIDIA's [interposer implementation](https://github.com/NVIDIA-RTX/Streamline/blob/main/source/core/sl.interposer/dxgi/dxgiSwapchain.cpp)
and [hook ABI definitions](https://github.com/NVIDIA-RTX/Streamline/blob/main/source/core/sl.api/internal.h).

The native game DLSSG plugin now receives NR wrappers for its HWND creation and Present/Present1
callbacks. Successful handled creation records the application's original DX12 queue on the
swapchain. Before DLSSG's present callback runs, NR obtains the current app-facing index and
buffer through that plugin's own hooks and processes it on the recorded game queue. The
underlying display-swapchain NR call is suppressed for these chains. If either buffer hook
declines, NR does not substitute the display buffer. Test presents and disabled NR do no work.
OptiScaler's separate local DLSSG plugin hook is unchanged.

Native handoff selects the latest matching submitted NR input using queue order/completion,
rather than expiring it against the inner display Present counter, which includes generated
frames. Other presentation paths retain their epoch check. Cross-queue unfinished input still
cannot introduce a presentation wait; this change does not restore the deadlock-prone wait.

Validation: Release x64 build, proxy/pipeline regressions and queue-readiness WARP regression
passed. `tests/nr_streamline_picture_smoke.cpp` exercises rotating app buffers on WARP,
demonstrates the old display edit being overwritten and the app-buffer edit surviving the
subsequent FG copy, and rejects declined plugin-buffer hooks without a display fallback.
The GPU test simulates the FG copy; it does not load NVIDIA's model or validate Cyberpunk.
Actual FG interpolation quality, HDR appearance and stable in-game operation require user
testing. This hook covers native DX12 HWND chains; other existing presentation bridges remain.
