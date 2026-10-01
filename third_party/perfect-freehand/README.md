# perfect-freehand attribution

Source: https://github.com/steveruizok/perfect-freehand

Revision: `176e00f2399f4969e1b0965c5921d96a3e50ce9f` (retrieved 2026-10-01).

`src/freehand.h` adapts `getStrokeOutlinePoints.ts`, `simulatePressure.ts`,
`getStrokeRadius.ts`, constants and vector operations to C++ under the included MIT license.
Mouse/touch collection in `src/main.cpp` adapts the streamline and initial noise filtering
of `getStrokePoints.ts` to incremental input.

Scope: simulated pressure, default thinning/smoothing/streamline of 0.5,
identity pressure easing, round start/end caps and sharp corner handling.
No configurable taper, custom easing, real-pressure input or flat caps are exposed.
Single-point strokes use a 26-vertex circle; short two-point input is handled directly,
rather than upstream's synthetic four subdivisions. Completed strokes use the actual endpoint.
Outline rendering uses closed quadratic midpoint segments.

Desktop-specific adaptation: freeze 256-point chunks, retain the shared endpoint
and incoming pressure, and only regenerate the bounded live tail. This is not a
bit-for-bit port of the full batch library. Chunk joins and startup behavior require
manual visual confirmation. Pressure simulation follows upstream point spacing,
not timestamp-normalized physical velocity.
