# Caustic CAD modeling

Windows desktop CAD for designing transparent shapes that project monochrome
caustic images onto one or more receiver surfaces. The Russian desktop editor
uses C++20, Qt Widgets and an OpenGL viewport provided by Open CASCADE. The ray tracer runs in C++ and the photographic transport solver runs in Rust.

## Лицензия RaySculpt

Оригинальный программный код RaySculpt распространяется по **PolyForm Noncommercial
1.0.0**. Для разрешённых лицензией целей, включая личные некоммерческие проекты,
код можно использовать, изменять и распространять. При передаче копии сохраните
текст или официальный адрес лицензии и предусмотренные ею уведомления.
[Текст в репозитории](LICENSE) · [Официальные условия](https://polyformproject.org/licenses/noncommercial/1.0.0)

Сторонние библиотеки и изображения сохраняют собственные условия. Патентная
оговорка лицензии охватывает только те права, которыми вправе распоряжаться
лицензиар. Сведения о компонентах и патентных публикациях приведены в
[перечне источников](THIRD_PARTY_NOTICES.md).

## Запуск редактора

Распакуйте весь архив Windows x64 версии **0.3.0-dev** и откройте **RaySculpt.exe**. Библиотеки находятся рядом с программой. Установка Python и подключение к интернету для запуска не требуются. Для проверки выберите **Файл → Открыть** и откройте **mona_lisa.ccad** из того же каталога. В проекте сохранены исходное изображение, оптическая поверхность и материалы.

Дорелизные сборки доступны в [заданиях сборки ветки prerelease/0.3.0](https://github.com/Anlrt/RaySculpt/actions/workflows/release.yml?query=branch%3Aprerelease%2F0.3.0). У опубликованной ранее версии 0.2.0 свои инструкции в [релизах](https://github.com/Anlrt/RaySculpt/releases). При работе с исходным кодом соберите редактор по разделу **Build**, затем запустите [Caustic CAD.cmd](<Caustic CAD.cmd>). Пример в исходном дереве находится в [examples/mona_lisa.ccad](examples/mona_lisa.ccad).

- Выберите тело в дереве или мышью. В параметрах справа задаются размеры, положение,
  углы, материал и справочная длина волны. Правой кнопкой вращается вид, средней
  кнопкой он перемещается, колесом меняется масштаб. Shift с левой кнопкой перемещает
  выбранный объект. F вписывает сцену, Shift+F приближает выбранный объект.
- Кнопка **Переместить** (M) показывает на выбранном объекте стрелки X, Y, Z.
  Потяните стрелку для перемещения вдоль мировой оси или само тело для перемещения
  в плоскости вида. Точные координаты вводятся в разделе **Положение**.
  Для связанного объекта меняйте положение опоры или параметры сопряжения.
- В меню **Вид** задайте тип элемента. Укажите две вершины, ребра, грани или
  центра и нажмите **Измерить**. Для **Сопряжения** сначала укажите неподвижную
  опору, затем перемещаемый элемент. Смещение измеряется вдоль нормали опоры,
  угол задаётся вокруг её локальной оси X (для ребра это касательная).
- Меню **Модель** создаёт эскиз на грани или плоскости XY, XZ, YZ,
  произвольный плоский срез и булевы операции. Эскиз может содержать отрезки
  и круговые дуги или одну окружность. В дереве раскройте тело и дважды нажмите
  на построение, чтобы изменить размеры. Срезы и булевы операции можно подавлять.
- **Проверить геометрию** проверяет исходные CAD-тела, открытые кромки, дефекты
  сеток, пересечение тел CAD и попадание экранов внутрь фактического рельефа.
  Для полигональных моделей проверяются пересечения граней с экраном.
  При проверке проекции редактор автоматически проверяет точную геометрию,
  если экран или площадной источник может пересекать рельеф.
- **Подобрать расстояния** подбирает положение выбранного экрана для лампы или
  прямого солнца. Начальный диапазон экрана составляет 30–180 мм от номинальной
  выходной плоскости. У лампы задайте размер светящейся области и доступное
  расстояние. До расчёта рельефа показывается оценка размытия, для готовой
  пластины проверяются лучи и, при назначенном изображении, ошибка яркости.
  Выберите предложение для применения, Ctrl+Z отменяет расстановку.
- Меню **Модель** создаёт тела и выдавливание замкнутого эскиза XY, выполняет срез
  плоскостью XY и задаёт жёсткую связь между объектами. Высота и вершины эскиза
  редактируются. Доступны отмена и возврат изменений.
- **Оптика → Материалы** открывает справочник. Новые карточки сохраняются в профиле
  пользователя. В проект записывается снимок характеристик. Масса рассчитывается по
  объёму и плотности. Каждая трассировка использует один выбранный показатель,
  внешняя среда — воздух с n = 1. Потери Френеля и полное внутреннее отражение
  учитываются, спектр белого света и объёмное поглощение требуют отдельной модели.
- **Файл → Открыть расчёт по профилю** открывает прежний фотографический
  `surface.txt` и восстанавливает осевую установку. Смена материала сохраняет форму.
  **Оптика → Проверить проекцию** трассирует эту форму. **Построить рельеф** повторно
  подбирает поверхность для выбранного материала. Для сложной сцены сохраняйте
  проект CCAD, чтобы сохранить положение всех экранов, источника и углы.
- В новой сцене **Проверить проекцию** (F5) показывает световое пятно плоской
  пластины. **Назначить цель** открывает выбор фотографии для выбранного экрана
  или первого свободного экрана. Для нескольких экранов задайте по одной цели.
  **Вписать целиком** сохраняет размеры экрана и пропорции изображения.
  **Заполнить экран** обрезает края. Третий вариант явно меняет высоту экрана.
  В свойствах экрана можно изменить размещение уже загруженного изображения.
  Для детального синтеза по одной фотографии выберите точечный источник на оси пластины.
  Для двух экранов назначьте каждому свою цель и нажмите **Построить рельеф**. На вкладке **Проекция** переключайтесь между экранами по имени. При осевом точечном источнике экраны могут быть наклонены друг к другу: для отдельных, неперекрывающихся отпечатков на плоскости пластины учитывается глубина каждой части цели. Для остальных конфигураций используется ограниченный по времени совместный подбор. Команда **Оптика → Разместить экраны рядом** центрирует свободные экраны относительно оси на расстоянии первого экрана. Закреплённые экраны сохраняют своё положение при синтезе.
  Для серии видов выберите **Оптика → Виды 3D-объекта** и одну модель
  OBJ, STL, STEP или BREP. Модель цели использует вертикальную ось Y.
  Расчёт использует видимые грани, глубину и освещение. Одна пластина подбирается
  для всех заданных углов. На вкладке проекции доступны также контрольные
  промежуточные виды. Серия видов занимает один экран.
- В фотографическом режиме и при синтезе по раздельным отпечаткам экранов доступны три уровня детализации. Для площадного света, серии поворотов и более сложного взаимного положения экранов программа подбирает одну поверхность по сетке 7 × 7, 9 × 9 или 11 × 11. Отдельная трассировка показывает проекцию для каждого экрана и ошибку яркости.
- Оптическое тело расчёта — пластина с плоским входом и сплайновым рельефом.
  Добавляемые примитивы и импортируемые тела получают роль **Справочное тело · вне оптики**.
  Их можно моделировать и экспортировать. В оптической модели они пропускают лучи
  без затенения и преломления. Старые проекты сохраняют назначенные роли.
- Импорт принимает STEP, BREP, OBJ и STL в координатах модели. Единицы OBJ/STL —
  миллиметры. Экспорт выбранного тела учитывает его положение. Для пластины
  доступны STEP, BREP, STL, OBJ и 3MF. При экспорте рельефа в сетку задаётся
  число ячеек. Плотность сетки предпросмотра не задаёт точность STEP.
- **Файл → Сохранить** создаёт переносимый проект CCAD. Проверки и экспорт
  выполняются в фоновом процессе, командой **Остановить** расчёт можно прервать.
  Вкладка **Проекция** показывает цель, проекцию и карту расхождений в общем масштабе.
  Колесо увеличивает все три изображения вокруг выбранной точки, перетаскивание
  сдвигает их вместе. На карте недостаток света обозначен синим, избыток — оранжевым.
  Карта сравнивает относительные яркости по сохранённым изображениям, абсолютный
  поток смотрите в журнале. PNG сохраняет исходную проекцию трассировщика.
  После изменения оптики предыдущий результат остаётся доступен с пометкой.
  Переименование объекта сохраняет актуальность. При открытии проекта результат очищается.

## Implemented in this prototype

- Primitive cube and sphere meshes and OBJ mesh import (input coordinates are millimetres).
- Deterministic, multithreaded generation of grayscale target views from a mesh.
  Every view uses the same camera scale and brightness mapping. Visible surfaces
  are resolved with a depth buffer, then shaded using normals, depth and edges.
- Scene preflight diagnostics for geometry/input errors and bounded-work estimates.
- A multithreaded forward ray tracer for a height-field plate, point or rectangular
  source, and rectangular projection surfaces. The design workflow uses a
  cubic B-spline with continuous slope and curvature across interior knots.
- STEP and Open CASCADE BREP export of the smooth optical solid. Indexed OBJ,
  3MF and binary STL export of a closed mesh. Existing designs can be exported again.
- A bounded coordinate-search inverse baseline. It fits one shared low-resolution
  height field to multiple target angle/screen pairs. The command-line workflow
  uses a point light and one fixed screen. Target views must share a brightness
  scale across angles.
- Unit tests for core geometry, target images, preflight checks and ray tracing.

## Build

The Windows x64 editor builds with Visual Studio 2022 (MSVC C++ desktop tools and CMake), PowerShell 7 and stable Rust for `x86_64-pc-windows-msvc`. Use a conda-forge SDK with matching x64 Qt, OCCT, OpenCV and libarchive libraries. From the project root in PowerShell:

```powershell
micromamba create -y -p ./build/sdk --file ./tools/windows-sdk.lock
./tools/build_native.ps1 -Sdk ./build/sdk/Library
./tools/package_native.ps1 -Sdk ./build/sdk/Library
./tests/native_runtime.ps1 -Package ./build/native/portable
./tools/archive_native.ps1 -Package ./build/native/portable
```

Keep the package cache until packaging finishes so upstream notices can be included. The build script runs the CMake tests. Packaging uses a new destination directory. For the C++ numerical core alone, configure CMake with `-DRAYSCULPT_NATIVE_DESKTOP=OFF`. The command examples below run from the extracted Windows package.

## Save a model

The format can be chosen when designing a plate or afterwards. From the
project directory, the following commands use an existing design directory
called `painting-result` and create new output paths.

```powershell
.\caustic_cli.exe export-plate painting-result --out painting.step
.\caustic_cli.exe export-plate painting-result --out models --formats step,brep,3mf,obj,stl
.\caustic_cli.exe design-photo painting.jpg --out new-design --formats step,3mf
```

`design-plate` also accepts `--formats`. Earlier commands keep their STL
default. A standalone export to a new directory defaults to STEP. A file
extension selects a single format (`.stp` and `.step` are equivalent).
STEP and BREP contain one solid in mm with one bicubic optical face, a
planar entrance and four sides. A saved optical profile or a legacy design
report supplies the spline coefficients. Mesh formats reuse the saved
triangulation when available. Pass `--cells N` on re-export to sample a fresh
uniform mesh with N cells per side. During photographic design,
`--mesh-layout transport` places mesh vertices according to the optical map.
3MF explicitly records millimetres. Interpret STL and OBJ coordinates as mm.

To check a saved model against its original profile, run:

```powershell
.\caustic_cli.exe verify-solid painting-result painting.step
```

The verifier reopens the CAD file, checks the solid topology and spline
parameters, and samples surface heights and normals including the four edges.
For a photographic design, add `--target target.pgm --out replay --rays 64000000`
to trace the reopened spline with the original scene and linear target image.

## Generate target images from a 3D mesh

```powershell
.\caustic_cli.exe render-target --obj model.obj --out target.pgm --angle 30
.\caustic_cli.exe render-turntable --obj model.obj --out views --views 12
```

`--cube MM` or `--sphere MM` can replace `--obj`. Image dimensions use `--width`
and `--height` in pixels. `--ambient`, `--depth`, and `--edges` control the
monochrome style; the defaults balance surface detail and stable brightness.
Images are binary PGM (`P5`), which preserves the exact 8-bit target values.

## Design a plate from target images

```powershell
.\caustic_cli.exe design-plate --target arrow.png --out arrow_result
.\caustic_cli.exe design-plate --frame 0 first.png --frame 12 second.png --out turn_result
```

Use PNG, JPEG, BMP or binary PGM. Color files use sRGB luminance, with
transparent pixels contributing no light. The PGM produced by this tool already
contains linear target intensities and can be used directly. Images of one
screen must have matching aspect ratios. The command projects the top of the
image toward world +Y and uses the entrance plane as the distance origin.

The default scene uses a 40 mm square plate with nominal thickness 5 mm and
index 1.49, a point source 200 mm before the entrance, and a screen 150 mm
after the entrance with a 70 mm width. Screen height follows the image aspect.
See `caustic_cli.exe --help` for geometry, relief and calculation options.

The output directory contains `plate.stl`, target and projection previews for
each angle, plus `diagnostics.jsonl` and `report.json`. Previews compare the
input, a flat plate, and the calculated plate. The report measures relative
image error on new rays and records captured light separately. Each plate uses
one overall brightness adjustment across all its angles. An evaluation or
time limit can end the search with its best completed candidate. Check both
the projection and the error before using the STL. The current solver suits
simple, low-resolution targets. Results are numerical simulations, pending
physical validation.

The optical face in `plate.stl` comes from the same spline used by the ray
tracer. The mesh subdivision is selected using `--stl-height-mm` (default
0.005 mm vertical deviation) and `--stl-normal-deg` (default 1 degree between
the facet and spline normals). The report gives the conservative geometric
deviation bounds and the spline coefficients in row-major order. The limits
describe the conversion to mesh, not the machining or print tolerances.
Choose tighter values if the manufacturing workflow requires them.

## Trace an example optic

### Full-frame photographs

For one centred point source, one perpendicular screen and a flat entrance,
the photograph solver builds an inverse transport map, fits a continuous exit
surface to the refracted directions, and measures the result on fresh rays.
It preserves the whole picture and its aspect ratio. Run these commands from the portable package directory.

```powershell
.\caustic_cli.exe design-photo painting.jpg --out painting-result --grid 682 --map-size 768 --resolution 560 --cells 2047 --rays 64000000
.\caustic_cli.exe verify-stl painting-result --side 6000
.\caustic_cli.exe verify-stl painting-result --side 6000 --source-size 0.5
```

All distances are in mm. Defaults are a 40 mm square plate, 8 mm nominal
thickness, index 1.49, point source 600 mm from the entrance, and a 50 mm
wide image at z=260 mm. The 12.5% guard on each side records light spilling
past the picture. The target and previews use the same display transfer.
The shadow toe (default 0.08 times mean irradiance) limits extreme ray
expansion. Pass `--shadow-floor 0` to remove this toe. Calibration always
scores against the original linear grayscale photograph. The final trace
uses a different jitter sequence. The JSON report records the trial scores,
selected pass, flux in the guard and ray counts.

`02_plate.stl` is the triangulated plate in mm. `03_projection.png` shows the
smooth optical surface. The optional verification tool reads the actual
binary facets and generates `04_stl_projection.png`. Its source-size option
traces a square Lambertian emitter of the specified width. It writes a
separate projection and report per size. The verifier accepts the regular
grid and face ordering produced by the photo exporter. A budget of 2047 cells
with a 682-node grid produces about 420 MB of binary STL per plate.

### Sinusoidal example

```powershell
.\caustic_trace_demo.exe demo_caustic.pgm
```

This traces 5 million rays through a 40 mm sinusoidal plate onto one screen and
also writes `demo_caustic.stl` (a closed 3D model, dimensions in mm) and
`demo_caustic.pgm.jsonl` with the run statistics. Its flat entrance is at z=0;
the exit is a 5 mm thick plate with a ±0.3 mm wave. The STL is a triangulated
approximation of the optical height field, suitable for inspection and print
setup; it has not been validated against a fabricated optic. The demo shows
the forward model only; it does not design the plate from a target image.
The checked-in example is `examples/wave_plate_40mm.stl`; see
`examples/README.md` for its dimensions and limitations.

## Design constraints

One physical optic is optimized jointly across all requested rotation angles
and screens. Separate height fields for separate angles cannot be added
together. Quick optical estimates are warnings; only exact, validated faults
can block a solve. The current coordinate search is suitable only for small
control grids and needs validation on fresh rays. The production solver must
run in a separate, bounded worker process so that a stalled solve cannot
freeze the editor.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the planned scene model and
the next implementation steps. Research and third-party software are listed
in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
