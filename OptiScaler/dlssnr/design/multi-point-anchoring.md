# Exposure-scan calibration anchors

The D3D12 exposure scanner reports candidate scalar values. Calibration maps a selected scan
value to a white point for the color codec; it does not assume every candidate is linear exposure.

One anchor uses a ratio law, with the configured direction:

```
white = anchorWhite * (anchorScan / scanNow) * trim
```

The inverted direction uses `scanNow / anchorScan`. With two or more anchors, interpolate
`log(white)` against `log(scan)` between adjacent sorted points. Clamp outside the calibrated
range to the endpoint white point. This avoids extrapolating into unmeasured lighting conditions.

The table holds up to eight points. Adding a scan value within two percent of an existing point
replaces its white point, avoiding a near-zero interpolation denominator. White points and scan
values must be positive and plausible. Trim is bounded at use and the final divisor is clamped.

Anchors persist as semicolon-separated `scan:white` pairs. Malformed tokens are skipped. A legacy
single-anchor configuration migrates to one table entry. The table has its own mutex and does
not share ownership of the scanner's GPU resources.

Implementation: `DlssNr_ExposureAnchors.cpp`. Candidate discovery and reporting live in
`DlssNr_ExposureScan.cpp`; GPU copies and ring ownership live in `DlssNr_ExposureReadback.cpp`.
