# Compatibility fix carried from v0.7.1

The useful retained fix resolves Reflex calls through the active loaded Streamline feature rather
than a stale bundled interface pointer. The historical startup reproducer failed through the old
pointer and succeeded with active-feature resolution; that isolated result did not establish
complete BG3 gameplay acceptance.

The model-selector and hybrid-kernel packaging experiments from that release were removed.
See [COMPATIBILITY-CHANGES.md](COMPATIBILITY-CHANGES.md) for retained integration work and
[INSTALL-DLSSNR.md](../INSTALL-DLSSNR.md) for the current runtime/package instructions.
