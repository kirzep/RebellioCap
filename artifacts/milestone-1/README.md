# Milestone 1 evidence

This directory contains auditable output for the native capture vertical slice.
Generated evidence is valid only for the clean source commit recorded in
`manifest.json`. The final evidence commit may be a descendant of that source
commit only when every intervening change is generated milestone evidence;
source-code, script, schema, or documentation changes invalidate the bundle.

`verify-milestone-1.ps1` requires every gate to be `passed`. A failed or
unavailable hardware run must be recorded as `failed` or `not-run`; neither is
accepted as completion. Raw NDJSON, ffprobe JSON, benchmark CSV files, hashes,
and the summary manifest are retained so calculated results can be reproduced.

Required generated files:

- `manifest.json`
- `doctor.ndjson`
- `capture-30m.ndjson`
- `ffprobe-mp4.json`
- `ffprobe-mkv.json`
- `ctest-unit.xml`
- `ctest-hardware.xml`
- `performance.json`

The manifest also hashes the complete `performance/` audit trail: the settings
declaration, six PresentMon CSVs, engine sampler CSV and NDJSON, and both saved
clip ffprobe JSON files. The verifier recomputes the summary from these files.

After all raw files exist, run `scripts/write-milestone-manifest.ps1`. It
refuses to overwrite an existing manifest and derives the source SHA, host,
tool, CTest, stream, capture, and performance fields before hashing every raw
artifact. Then run `scripts/verify-milestone-1.ps1` for the independent final
recalculation and gate.

The verifier parses packet-level ffprobe output rather than trusting declared
stream fields: it checks one H.264 stream, three named AAC streams, positive
durations, monotonic per-stream DTS, and start/end A/V boundary spread. It also
cross-checks raw 30-minute engine metrics and the auditable performance summary
against the manifest thresholds.

Run `scripts/collect-performance.ps1 -PreflightOnly` before the operator
session. Collection refuses to overwrite prior evidence and requires a written
settings declaration. Its CSV-analysis mode is covered by deterministic tests;
the six real benchmark runs remain a hardware/operator gate.
