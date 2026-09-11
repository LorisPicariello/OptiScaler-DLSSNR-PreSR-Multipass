# Historical v0.7.0 NR integration notes

The useful NR changes carried forward from this preview are native Vulkan pre/post placement,
multipass profiles, active padded colour handling, the optional extended pass range, and D3D12
private-DLSS residual composition. Vulkan/Proton device identification, resource restoration and
menu interlocks are listed with attribution in [VULKAN-PARITY-REVIEW.md](VULKAN-PARITY-REVIEW.md).

Current installation and packaging instructions are in [INSTALL-DLSSNR.md](../INSTALL-DLSSNR.md).
The old preview's package layout and experimental extras are not the current package contract.
NVIDIA NR/SR/RR/FG runtimes are supplied separately; this branch's package includes no downloader.

Historical Release and shader checks did not establish native Vulkan gameplay, RR/Proton quality,
or regression-free performance. Use the current build's validation record for any release claim.
