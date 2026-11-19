# Architecture and next steps

## Units and scene

World-space positions and lengths use millimetres; UI angles use degrees.
Each scene has an optical object, a point or rectangular area emitter, one
rotation axis with chosen angles, and zero or more rectangular receivers.
Each receiver has an image and a physical width/height. A CCAD document packages
the scene, material cards, source images, 3D targets and original geometry.
STEP and BREP keep their original boundary representation. Display triangulations
are independent of optical tracing and export. Generated plates use the continuous
optical surface for tracing and smooth CAD exchange.

The desktop uses Qt Widgets and a separate C++ process for ray tracing. Scene
transforms, material selection and fixed parent-child relationships are serialized
in the document. Single-wavelength indices come from the selected material card.
The solver receives each source and receiver in the local axes of the active plate.
Inverse synthesis has two entry points, photographic transport for a single
on-axis target and a bounded joint fit for general layouts. The latter shares
one surface across all requested views and screens and verifies intermediate
angles using an independent sample of rays.

## 3D mesh to monochrome targets

Render a visible-surface G-buffer at each requested orientation: depth,
normal and object mask. Use one orthographic camera scale and one exposure
for the full angle sequence. Combine diffuse key/fill lighting with a modest
nearer-is-brighter cue and selective visible contours. Supersample before
downsampling. A depth-only mode is available by changing weights, but should
not be the default because it erases many shape cues. This target renderer is
independent from the physical caustic light source; it specifies the desired
image rather than simulating the glass.

Quality checks: same model scale and image brightness across angles, stable
output across thread counts, correct self-occlusion, visible silhouettes,
high contrast on representative asymmetric meshes, no angle-dependent auto
exposure. Target resolution must eventually be related to the measured optical
point-spread function and manufacturing tolerance.

## Diagnostics and responsiveness

Three levels are planned:

1. **Interactive:** numeric validity, cached bounding-volume and receiver
   collision checks, source and screen orientation checks. No whole-mesh
   topology scan on each mouse movement. Never wait for a ray trace in the UI
   thread.
2. **Asynchronous preview:** sparse deterministic rays from a snapshot of the
   current scene. Cancel stale revisions. Report partial screen coverage,
   occlusion and likely total-internal-reflection as warnings, since a sparse
   miss does not prove impossibility.
3. **Final solve:** exact mesh validity, estimate of work and memory, then
   coarse-to-fine joint optimization. Track per-angle/per-screen residual,
   escaped light, TIR, stagnation, elapsed time and memory. Validate on fresh
   rays and intermediate angles.

The editor should write structured diagnostic records with a code, severity,
scene revision, object ID, angle, measured value and suggested action. An
optical error and a proven invalid input must have different severities.

The final optimizer belongs in an external worker process. On Windows it can
be placed in a Job Object with memory limits and kill-on-close; cancellation
starts cooperatively and escalates to terminating the worker if needed. The
UI applies a result only if its scene revision matches the current scene.

## Placement suggestions

For each screen, sample permitted translations and rotations inside a user
specified search box, rejecting colliding candidates. Score each remaining
candidate with a bounded low-resolution trace over all selected optic angles:
target coverage, captured flux, incidence angle and screen-to-screen
occlusion. Refine the best few candidates and return their predicted scores.
The best candidate is the best **within the searched domain**, not a proof of
a global optimum.

## Inverse optimizer

The current baseline synthesizes a low-resolution height field by bounded
coordinate search. It uses one set of surface variables and a weighted sum of
normalized irradiance errors over all selected angles and receivers, plus a
smoothness penalty and thickness bounds. It is useful for correctness tests,
but a production optimizer needs coarse-to-fine controls, better search,
manufacturing constraints, and validation with independent ray samples and
intermediate angles. Forward ray tracing is the validation oracle, not proof
that an arbitrary target is physically reachable.

## Planned editor

C++20 with Qt Widgets and Open CASCADE for a CAD-style viewport and exact
primitive/Boolean operations; Assimp for polygonal exchange formats when
needed. STEP/IGES should be imported as CAD solids; Blender geometry via
GLB/OBJ/STL. Native SolidWorks and KOMPAS files require export to STEP from
their respective software. The project format will hold all editable scene
data and target images. The plate already exports separately as a smooth STEP/BREP
solid or a mesh in 3MF, OBJ and STL. Performance libraries such as Embree should only be
introduced after profiling complex imported meshes.
