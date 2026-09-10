#pragma once

// Experimental Neural Rendering backend using OptiScaler's existing NVNGXProxy dispatcher.
// Feature creation is still unverified on the driver core; keep the direct backend available
// until an in-game comparison succeeds. See FORWARDER_INVESTIGATION.md for the evidence.

#include <d3d12.h>

namespace DlssNr
{
namespace Proxy
{
// True when the driver's nvngx is initialised and exports what this path needs.
bool Available();

// Creates the feature if it does not exist, or if the resolution changed, and evaluates it.
// Returns the NGX creation/evaluation result, or 0 when nothing could be attempted.
// A successful creation defers evaluation until the next call. When provided, evaluated is
// true only when output was written successfully; false means the caller must skip composition.
unsigned int Run(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, ID3D12Resource* color, ID3D12Resource* depth,
                 ID3D12Resource* motion, ID3D12Resource* output, unsigned int width, unsigned int height,
                 unsigned int guideWidth, unsigned int guideHeight, bool depthInverted, bool reset, float mvScaleX,
                 float mvScaleY, bool* evaluated = nullptr);

// Retires the current feature and clears the failure latch without immediately freeing GPU work.
void RetryAfterFailure();

// Releases current and retired features and parameter maps at shutdown, after GPU work is complete.
void Release();
} // namespace Proxy
} // namespace DlssNr

