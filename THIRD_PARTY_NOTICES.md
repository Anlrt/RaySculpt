# Sources and third-party components

The prototype uses the C++ standard library and the [Windows Imaging Component](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-creating-decoder)
provided by Windows. No third-party source code has been copied into the
repository.

Research references that informed the design:

- Ching-Kuang Shene, [B-spline derivatives and continuity](https://pages.mtu.edu/~shene/COURSES/cs3621/NOTES/spline/bspline-derv.html), Michigan Technological University.
- Yuliy Schwartzburg et al., [High-contrast Computational Caustic Design](https://theialab.ca/pubs/schwartzburg2014caustics.pdf), 2014.
- Thomas Kiser and Mark Pauly, [Caustic Art](https://infoscience.epfl.ch/record/196165/files/caustic_art.pdf), 2012.
- Mitsuba 3 team, [Caustics Optimization tutorial](https://mitsuba.readthedocs.io/en/stable/src/inverse_rendering/caustics_optimization.html).
- Budianto Tandianus et al., [Caustic Object Construction Based on Multiple Caustic Patterns](https://dspace.zcu.cz/items/1d6f2b70-c0aa-4be7-8159-46eaa4306bf9/full), 2012.
- Christoph Bösel and Herbert Gross, [Single freeform surface design for prescribed input wavefront and target irradiance](https://arxiv.org/abs/1705.04540), 2017. This is background for ray mapping and normal integration. The numerical implementation here is independent.

The optional photographic workflow imports [NumPy](https://numpy.org/),
[OpenCV](https://opencv.org/) and [Pillow](https://python-pillow.org/).
NumPy uses the BSD-3-Clause license, OpenCV 4.5 and later uses Apache-2.0,
and Pillow uses the HPND license. These libraries are installed separately.
The full-frame verification used NumPy 2.3.4, OpenCV 4.10.0 and Pillow 12.0.0.

Smooth solid exchange uses [cadquery-ocp-novtk](https://pypi.org/project/cadquery-ocp-novtk/)
and cadquery-ocp-proxy version 8.0.1.1.0 (Python bindings, Apache-2.0).
They provide [Open CASCADE Technology](https://dev.opencascade.org/resources/licensing)
under its LGPL-2.1 license with the OCCT exception. The dependency is installed
separately in the build directory. The mesh package writer follows the
[3MF Core Specification](https://github.com/3MFConsortium/spec_core).

If a library or code fragment is added later, record its exact version, source,
license, and any required notices here before committing it.

The desktop editor uses [Qt for Python / PySide6 Essentials](https://pypi.org/project/PySide6-Essentials/6.12.0/)
and Shiboken6 6.12.0. The locally installed wheels include the Qt/PySide notices
and license texts. The interface uses Qt Fusion, Qt standard actions and system fonts.

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

Subshape selection, BREP validity, distance queries and Boolean operations use
the installed Open CASCADE bindings listed above. The Windows package contains
the project code, the compiled project engine and the examples. The launcher
obtains the dependencies as wheels, including their upstream notices, using pip.
