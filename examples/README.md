# Example optical surface

`wave_plate_40mm.stl` is the same sinusoidal plate used by `trace_plate.cpp`.
The preview image shows its solid shape, exit-surface height map, and a centre
cross-section.

- External dimensions: 40 x 40 mm.
- Flat entrance: z = 0 mm.
- Exit: nominal z = 5 mm, varying from 4.70 to 5.30 mm.
- Height formula: `0.30 cos(2 pi u) cos(2 pi v)` mm over the unit square.
- Sampling: 256 x 256 points, exported as a closed binary STL with millimetre
  coordinates. STL itself does not encode units; select mm in the print tool.

This is a forward-tracing demonstration, not an inverse-designed image lens.
The STL is a triangle approximation of the bilinear optical height field. It
has not been checked against printer tolerances, optical material data, or a
physical prototype. Run `build\Release\caustic_trace_demo.exe demo_caustic.pgm`
from the project root to regenerate the corresponding STL and light projection.
