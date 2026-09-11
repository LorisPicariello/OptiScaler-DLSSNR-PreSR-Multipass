# Driver-dispatched Neural Rendering — evidence log

## Current implementation

Neural Rendering now uses the installed NVIDIA NGX core's feature-18 dispatcher through
OptiScaler's `NVNGXProxy` entry points. The core discovers and calls the separately supplied
`nvngx_dlssnr.dll`. NR has no separate helper DLL, direct-snippet fallback, or embedded/extracted
forwarder. The former forwarder source and build project have been removed.

The old `UseProxy` setting no longer selects an alternative backend. The driver route is mandatory.
Ordinary OptiScaler dependencies still serve its other upscalers and graphics APIs; they are not
additional NR caller shims.

A Cyberpunk 2077 test reached successful model creation and evaluation using the typed driver
context with the former helper DLL physically absent. This establishes that those calls can
succeed without the helper in that setup. It does not establish visual correctness, full gameplay
stability or a general driver/GPU/runtime support matrix.

Subsequent hardware probes on RTX 5090 passed two identical-profile feature creations with distinct
handles and six fenced evaluations on each of DX12 and native Vulkan. Vulkan also passed creation
of both features on one command buffer followed by the production-style completion event. A test
compiling the production DX12 `DlssNr_Proxy.cpp` against real driver exports passed independent
contexts, same-epoch evaluation suppression and tuning-triggered recreation. Synthetic input
textures establish API execution and lifecycle behavior, not model image quality.

## Typed parameters and lifetime

The original experiment guessed parameter virtual-table slots from source declaration order.
A local MSVC 14.44 x64 assembly probe using this repository's `nvsdk_ngx_params.h` produced:

| Parameter type | Emitted dispatch | Vtable slot |
| --- | --- | --- |
| `float` | `[rax+48]` | 6 |
| `unsigned int` | `[rax+32]` | 4 |
| `ID3D12Resource*` | `[rax+8]` | 1 |

Slot 6 for floats therefore did not demonstrate a different private ABI. The old unsigned setter
called the signed-integer slot, while the old resource setter called the `void*` slot. The current
backend uses the SDK's typed setters and owns its capability/evaluation parameter maps.

Feature creation errors remain visible. A failed model is not made usable merely because a handle
was returned, and retry does not switch to a hidden fallback backend. Creation/submission separation
and retirement remain part of the owning shader/model context; an evaluate-count delay alone is not
a GPU completion fence.

The focused harness at `tests/dlssnr_proxy/run.ps1` compiles production context code against mock
NGX entry points and the actual SDK parameter interface. Mock tests establish API and lifetime
behavior, not NVIDIA model output or game compatibility.

## Historical observations

Earlier direct-snippet experiments encountered a caller-module check requiring `nvngx.dll` in the
caller's module path. The old project therefore supplied a helper named `nvngx.dll_dlssnr.dll`.
That workaround is historical and is not part of the current implementation.

Earlier dispatcher attempts returned `0xBAD0000B` (`FAIL_UnableToInitializeFeature`). That result
alone did not prove snippet discovery or explain the failure. Repeating creation and reinitializing
an already initialized core did not resolve those tests. The later helper-absent creation/evaluation
result supersedes the claim that a helper is always required; it does not explain every old failure.

## RenoDX attribution and source limits

RenoDX attribution applies to the colour-composition design. A previous version of this document
also asserted that RenoDX avoids a helper by detouring core creation/evaluation. That assertion was
not verified and has been removed.

The inspected public RenoDX tree at commit `cd32113a98608e63027d40910cfe296a14dfe228` contains ordinary
[NGX forwarding hooks](https://github.com/clshortfuse/renodx/blob/cd32113a98608e63027d40910cfe296a14dfe228/src/utils/dlss/nvngx.hpp)
but no public feature-18 addon implementation. Logs in [RenoDX issue 651](https://github.com/clshortfuse/renodx/issues/651)
show a successful initialization/creation/evaluation sequence, but do not establish which module's
entry points were called or how caller validation was handled. Our dispatcher route is documented
from this project's implementation and tests, not as a verified reproduction of RenoDX internals.
