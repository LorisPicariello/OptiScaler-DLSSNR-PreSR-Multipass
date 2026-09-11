# NR fixes carried from v0.7.6

Retained NR work includes the NGX input-routing fixes, deferred before/after pairing, and residual
composition validation discussed in [ISSUE-REVIEW-2026-09-10.md](ISSUE-REVIEW-2026-09-10.md).
The original release's optional runtime-loader and hybrid-asset packaging are no longer included.

Use the [current setup guide](../INSTALL-DLSSNR.md) and package rather than the historical archive's
payload list. Supply the GPU-appropriate NR runtime separately. Historical targeted tests are not
new gameplay validation of the current branch.
