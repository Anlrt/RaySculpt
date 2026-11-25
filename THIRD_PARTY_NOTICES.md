# Sources and third-party components

RaySculpt's original software is offered under the
[PolyForm Noncommercial License 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0).
Third-party components and artwork retain their respective terms.

The prototype uses the C++ standard library and the [Windows Imaging Component](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-creating-decoder)
provided by Windows. The native desktop dependencies and contributed code are listed below.

Research references that informed the design:

- Ching-Kuang Shene, [B-spline derivatives and continuity](https://pages.mtu.edu/~shene/COURSES/cs3621/NOTES/spline/bspline-derv.html), Michigan Technological University.
- Yuliy Schwartzburg et al., [High-contrast Computational Caustic Design](https://theialab.ca/pubs/schwartzburg2014caustics.pdf), 2014.
- Thomas Kiser and Mark Pauly, [Caustic Art](https://infoscience.epfl.ch/record/196165/files/caustic_art.pdf), 2012.
- Mitsuba 3 team, [Caustics Optimization tutorial](https://mitsuba.readthedocs.io/en/stable/src/inverse_rendering/caustics_optimization.html).
- Budianto Tandianus et al., [Caustic Object Construction Based on Multiple Caustic Patterns](https://dspace.zcu.cz/items/1d6f2b70-c0aa-4be7-8159-46eaa4306bf9/full), 2012.
- Christoph Bösel and Herbert Gross, [Single freeform surface design for prescribed input wavefront and target irradiance](https://arxiv.org/abs/1705.04540), 2017. This is background for ray mapping and normal integration. The numerical implementation here is independent.

The Windows package uses the shared libraries in the conda-forge builds of [Qt 6.12.1](https://www.qt.io/licensing/), [Open CASCADE Technology 8.0.2](https://dev.opencascade.org/resources/licensing), [OpenCV 5.1.0](https://opencv.org/license/) and [libarchive 3.8.11](https://github.com/libarchive/libarchive). The 3MF writer follows the [3MF Core Specification](https://github.com/3MFConsortium/spec_core).

The package's `licenses/` directory identifies each redistributed dependency by version and build and includes the license files supplied by its upstream package, plus the Rust toolchain notices. The editor uses Qt Widgets, Qt Fusion and system fonts. The previous 0.2.0 implementation used PySide6, Shiboken6, NumPy, Pillow and CadQuery OCP bindings. The version-specific notices are retained in the 0.2.0 source history.

Material properties in the local catalog are taken from the manufacturers.

- [Röhm PLEXIGLAS GS / XT material properties, reference 211-1](https://www.plexiglas.de/files/plexiglas-content/pdf/technische-informationen/211-1-PLEXIGLAS-GS-XT-EN.pdf). Clear grades, density and refractive index at the sodium D line.
- [SCHOTT N-BK7 datasheet](https://media.schott.com/api/public/content/41e799d0bf874807a0bb8e702fbb75b5?v=54856406). Density and tabulated visible indices.
- [SCHOTT N-SF11 datasheet](https://media.schott.com/api/public/content/78e83df5ca2c4da4ad4490a52c80a146?v=1a468147). Density and tabulated visible indices.

The Mona Lisa reproduction in the example project is the file supplied by the project owner.

## Additions in 0.2.0

The solar source uses an apparent solar diameter of 1,919 arcseconds at 1 AU.
[NASA Sun Fact Sheet](https://nssdc.gsfc.nasa.gov/planetary/factsheet/sunfact.html).

Mesh picking uses an independently written vectorized implementation of the
ray–triangle intersection described by Tomas Möller and Ben Trumbore (1997).
[Fast, Minimum Storage Ray–Triangle Intersection](https://doi.org/10.1080/10867651.1997.10487468).

The current editor uses Open CASCADE directly for subshape selection, BREP validity, distance queries and Boolean operations.

## Native desktop migration

Yorodoshi contributed the Rust transport kernels and the OpenCV spectral callback
on branch A01. The C++ optical spline construction and CAD body builder are also
adapted from that contribution. Original source and authorship are preserved in
[RaySculpt commit f3db583](https://github.com/Anlrt/RaySculpt/commit/f3db583347ac02badcc0bdb00b3e14f4d0e39054).

The native editor links Qt Widgets, Open CASCADE Technology, OpenCV and libarchive.
The Rust standard library is linked into the executable. Source dependencies and
runtime redistributions retain their respective upstream notices.

## Third-party patent references

EPFL lists two patent families relevant to computational caustic design,
*Design of a refractive surface* (EP2963464B1, US10732405B2) and *Method of
producing a reflective or refractive surface* (EP2711745B1, US9576553B2).
The PolyForm patent grant extends only to claims the software licensor can
license. Assessing a particular implementation or manufactured optic requires
considering the relevant claims and their status in the country of use.
[EPFL patent list](https://www.epfl.ch/labs/gcm/tech-transfer/) ·
[European patent specification](https://data.epo.org/publication-server/rest/v1.2/publication-dates/20170823/patents/EP2963464NWB1/document.html) ·
[US patent claims](https://patents.google.com/patent/US10732405B2/en#claims)
