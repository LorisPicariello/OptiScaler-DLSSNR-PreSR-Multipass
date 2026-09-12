# Frame hold

D3D12 NR can freeze its input while model and composition settings change. Ordinary hold owns
copies of the input color and required guides, restores the original NGX parameter values after
evaluation, and leaves the game's original resources owned by the game. The color codec also
keeps a native color snapshot for repeated composition. Releasing hold resumes live inputs.

The exposure snapshot travels with the held image, so changing light in the running game cannot
change the held comparison. Live exposure sampling is disabled while using that snapshot.
Resource shape or placement changes invalidate the held inputs. A reset command list discards
an unsubmitted hold capture. Owned snapshots retire through the owner's GPU-completion tracker.

Finished-picture hold owns one clean presentation snapshot per feature, independently of the
rotating swapchain buffers and presentation slots. Its queue fence protects reuse. See
[finished-picture routes](../../../docs/NR-FINISHED-BRIDGES.md).

Hold is a comparison aid, not a simulation pause: the game can continue updating while the
NR input stays frozen. Native Vulkan does not implement the D3D12 input-hold path.
