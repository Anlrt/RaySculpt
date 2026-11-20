# A01 optical reference

Build Release with `BUILD_TESTING=ON` on Windows x64 using the project's
Python environment and dependencies. From the repository root, run:

```powershell
.\.venv\Scripts\python.exe tests/fixtures/reproduce.py build/A01-reference
```

Use a new output directory on each invocation. The runner creates the six
scenes, synthesizes profiles where needed, and writes the trace settings,
profiles, PNG previews, raw linear fields, and reports. For the flat cases,
the settings and a dash profile select the native flat plate.

The test-only executable `caustic_reference.exe` uses the production
trace-scene path with 12 threads and records each screen's linear irradiance
as a headerless little-endian float64 array in row-major order. Screen
dimensions are recorded in the trace settings. The runner traces each pose
twice and requires exact file agreement on the same build and machine.

The checked-in manifest records the run on the audit workstation (Windows 11,
MSVC 19.51, Release x64, Python 3.14.6). Use its counters and metrics as the
baseline for subsequent changes. Exact file hashes describe the audited run.
The relative angular order for the asymmetric target is -8°, 0°, +8°.

## B01 software viewport reference

```powershell
.\.venv\Scripts\python.exe tests/fixtures/viewport.py build/B01-viewport
```

The runner captures flat and portrait scenes at 1600 × 900, with and without
the grid. It saves the camera poses and representative PNGs for each 60-frame
series. For the 20 selection samples, `old_id` records the software picker and
`visible_id` gives the expected visible object for the GPU depth test.

Configure the native preview with `RAYSCULPT_BUILD_GPU_PREVIEW=ON` and a Qt 6
SDK that provides Widgets, OpenGL, and OpenGLWidgets. Run
`raysculpt_gpu_preview build/B01-viewport/portrait.json --benchmark` for the
unsynchronized GPU draw measurement. To check a selection scene, pass its
snapshot followed by `--picks build/B01-viewport/viewport.json screen-behind`,
substituting the same selection scene name in both arguments.

The existing editor can load the native widget when
`RAYSCULPT_GPU_WIDGET` points to `raysculpt_gpu_widget.dll`. Run
`python tests/fixtures/gpu_editor.py build/gpu/Release/raysculpt_gpu_widget.dll build/live-editor.png`
with the Windows Qt platform to exercise the live editor, including selection
after scene edits, Undo, and the software fallback used by the height map.

Configure with `RAYSCULPT_BUILD_MESH_INDEX=ON` to build the triangle index.
Set `RAYSCULPT_MESH_INDEX` to its DLL to enable indexed picking of the original
imported mesh. Run `python tests/fixtures/mesh_picking.py DLL NEW_OUTPUT` to
compare 100 picks each on 100,000 and 1,000,000 triangles.

## C01 and C02 Rust checks

Build with `cargo test --locked` and `cargo build --release --locked`.
Set `RAYSCULPT_WORKER` to the absolute path of the Release executable to run
the desktop and worker integration tests through the opt-in native tracing
path. The worker reads one JSON request with version, job ID, scene revision,
trace settings, profile and output path, then emits one JSON object per line.
Cancellation uses the same version, job ID and revision.
The optional `memory_limit_bytes` sets the Windows job limit for the worker
and engine together. It defaults to 1 GiB. The editor exposes an exact MiB
value and reports memory failures separately from timeouts and cancellation.

Run `python tests/fixtures/transport.py target/release/caustic_worker.exe build/C02-transport`
to compare both transport implementations. This writes the test maps, result
arrays, timings and final RMS. The optional `--transport-worker` argument of
`design_photo.py` selects the Rust implementation for controlled runs.
The same option accepts `caustic_transport.dll` for direct calls.

For C03, configure with `RAYSCULPT_BUILD_SPECTRAL_FILTER=ON` and OpenCV 4.10.0.
Run `python tests/fixtures/native_photo.py RUST_DLL OPENCV_ADAPTER_DLL NEW_OUTPUT`
to measure total elapsed time and peak working set of the photo-design process,
compare calibration, and check the profile at the center, edges and corners.

## D01 tracing performance

```powershell
python tests/fixtures/trace_timing.py build/A01-reference build/D01-timings
```

Run after compiling the Release build. The runner uses the original portrait
profile and records parse and reduction time independently of sampled
intersection and accumulation time. It compares 1, 4, 8, 12, and 24 threads.
For the 16,000-ray class, the same process also runs 20 short traces per
variant to compare fresh threads and buffers with a reusable workspace. The
two paths must produce identical linear fields at the same thread count.

## D02 native STEP and BREP

Configure with `RAYSCULPT_BUILD_OCCT_EXPORT=ON` and an Open CASCADE 8 SDK.

```powershell
python tests/fixtures/cad_export.py build/Release/caustic_cad_export.exe build/A01-reference/portrait/surface.txt build/D02-cad
```

The runner reopens both local CAD exports, reverses the pose of a rotated
export, and checks rollback on failure. Set `RAYSCULPT_CAD_EXPORT` to the
absolute exporter path to exercise the optional STEP/BREP plate export in
the existing editor.

## E03 native project and materials

`raysculpt_project_editor WORKER [PROJECT]` opens a CCAD project in the native
Qt window. It reads both CCAD versions through Rust, displays material cards
and wavelengths, and saves material changes as a separate project file.

Run `python tests/fixtures/project_roundtrip.py EDITOR WORKER NEW_OUTPUT`
to exercise both versions through the Qt window and reopen the copies in the
existing Python reader. The check verifies attachment hashes, material edits
and preservation of the source files.
