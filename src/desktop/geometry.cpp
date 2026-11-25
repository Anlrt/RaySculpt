#include "geometry.hpp"
#include "height_surface.hpp"
#include "surface_mesh.hpp"
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <Poly_Triangulation.hxx>
#include <RWStl.hxx>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <iomanip>
#include <memory>
#include <cmath>

namespace raysculpt {
OpticalProfile profile(const QString &path) {
    std::ifstream f(std::filesystem::path(path.toStdWString()));
    OpticalProfile p{};
    auto &h = p.plate;
    int n;
    if (!(f >> n >> h.width_mm >> h.thickness_mm >> h.refractive_index >> p.source >> p.screen >> p.width >>
          p.height) ||
        n < 3 || n > 1024 || !(h.width_mm > 0 && h.thickness_mm > 0 && h.refractive_index > 1))
        fail("Некорректный профиль");
    h.samples_x = h.samples_y = n;
    h.height_mm = h.width_mm;
    h.interpolation = caustic::HeightInterpolation::CubicLinearEnds;
    h.offsets_mm.resize(size_t(n) * n);
    for (auto &z : h.offsets_mm)
        if (!(f >> z) || !std::isfinite(z) || h.thickness_mm + z <= 0)
            fail("Ошибка высоты рельефа");
    return p;
}
caustic::Mesh surfaceMesh(const caustic::HeightField &h, int cells) {
    if (cells < 1 || cells > 2048)
        fail("Некорректная точность сетки");
    caustic::HeightField sampled = h;
    sampled.interpolation = caustic::HeightInterpolation::Bilinear;
    sampled.samples_x = sampled.samples_y = cells + 1;
    sampled.offsets_mm.resize(size_t(cells + 1) * (cells + 1));
    for (int y = 0; y <= cells; ++y)
        for (int x = 0; x <= cells; ++x)
            sampled.offsets_mm[size_t(y) * (cells + 1) + x] =
                caustic::sample_spline(h, (double(x) / cells - .5) * h.width_mm,
                                       (double(y) / cells - .5) * h.height_mm)
                    .height;
    return caustic::make_height_field_solid(sampled);
}
static void append(caustic::Mesh &out, const occ::handle<Poly_Triangulation> &tri, const gp_Trsf &loc,
                   bool flip) {
    if (tri.IsNull())
        return;
    auto first = uint32_t(out.vertices.size());
    for (int i = 1; i <= tri->NbNodes(); ++i) {
        auto p = tri->Node(i).Transformed(loc);
        out.vertices.push_back({p.X(), p.Y(), p.Z()});
    }
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
        int a, b, c;
        tri->Triangle(i).Get(a, b, c);
        if (flip)
            std::swap(b, c);
        out.triangles.push_back({first + a - 1, first + b - 1, first + c - 1});
    }
}
caustic::Mesh shapeMesh(const TopoDS_Shape &body) {
    BRepMesh_IncrementalMesh mesher(body, .12, false, .25, true);
    caustic::Mesh out;
    for (TopExp_Explorer it(body, TopAbs_FACE); it.More(); it.Next()) {
        TopLoc_Location loc;
        auto face = TopoDS::Face(it.Current());
        auto tri = BRep_Tool::Triangulation(face, loc);
        append(out, tri, loc.Transformation(), face.Orientation() == TopAbs_REVERSED);
    }
    if (out.triangles.empty())
        fail("Нет треугольников поверхности");
    return out;
}
caustic::Mesh meshFile(const QString &path) {
    caustic::Mesh mesh;
    std::string error;
    if (path.endsWith(".obj", Qt::CaseInsensitive)) {
        if (!caustic::load_obj(path.toStdWString(), mesh, error))
            fail(QString::fromStdString(error));
    } else if (path.endsWith(".stl", Qt::CaseInsensitive)) {
        append(mesh, RWStl::ReadFile(path.toUtf8().constData()), gp_Trsf(), false);
        if (mesh.triangles.empty())
            fail("Пустая сетка STL");
    } else {
        auto o = newBody("cad");
        o["asset"] = QFileInfo(path).fileName();
        mesh = shapeMesh(build_body(o, QFileInfo(path).absolutePath().toStdWString()));
    }
    return mesh;
}
static TopoDS_Shape meshFace(const caustic::Mesh &mesh) {
    occ::handle<Poly_Triangulation> tri =
        new Poly_Triangulation(int(mesh.vertices.size()), int(mesh.triangles.size()), false);
    for (int i = 0; i < int(mesh.vertices.size()); ++i) {
        auto p = mesh.vertices[i];
        tri->SetNode(i + 1, gp_Pnt(p.x, p.y, p.z));
    }
    for (int i = 0; i < int(mesh.triangles.size()); ++i) {
        auto t = mesh.triangles[i];
        tri->SetTriangle(i + 1, Poly_Triangle(t.a + 1, t.b + 1, t.c + 1));
    }
    TopoDS_Face face;
    BRep_Builder().MakeFace(face, tri);
    return face;
}
TopoDS_Shape displayShape(const QJsonObject &o, const QString &folder) {
    auto k = o["kind"].toString();
    if (k == "mesh")
        return meshFace(meshFile(assetFile(folder, o["asset"].toString())));
    if (k == "plate" && o.contains("profile")) {
        auto h = profile(assetFile(folder, o["profile"].toString())).plate;
        return meshFace(surfaceMesh(h, std::min(80u, h.samples_x)));
    }
    if (solid(k))
        return build_body(o, folder.toStdWString());
    if (k == "screen" || k == "area") {
        auto d = o["dims"].toObject();
        double w = d["w"].toDouble() / 2, h = d["d"].toDouble() / 2;
        BRepBuilderAPI_MakePolygon p;
        for (auto pt : {gp_Pnt(-w, -h, 0), gp_Pnt(w, -h, 0), gp_Pnt(w, h, 0), gp_Pnt(-w, h, 0)})
            p.Add(pt);
        p.Close();
        return BRepBuilderAPI_MakeFace(p.Wire()).Face();
    }
    return BRepPrimAPI_MakeSphere(2.).Shape();
}
void exportMesh(const caustic::Mesh &original, const QString &path, const gp_Trsf &tr) {
    auto mesh = original;
    for (auto &v : mesh.vertices) {
        auto p = gp_Pnt(v.x, v.y, v.z).Transformed(tr);
        v = {p.X(), p.Y(), p.Z()};
    }
    QTemporaryDir tmp;
    QString staged = tmp.filePath("body." + QFileInfo(path).suffix().toLower());
    if (staged.endsWith(".stl")) {
        std::string error;
        if (!caustic::save_binary_stl(staged.toStdWString(), mesh, error))
            fail(QString::fromStdString(error));
    } else if (staged.endsWith(".obj")) {
        std::ofstream out(std::filesystem::path(staged.toStdWString()));
        out << std::setprecision(17);
        for (auto v : mesh.vertices)
            out << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';
        for (auto t : mesh.triangles)
            out << "f " << t.a + 1 << ' ' << t.b + 1 << ' ' << t.c + 1 << '\n';
        out.close();
        if (!out)
            fail("Ошибка записи OBJ");
    } else if (staged.endsWith(".3mf")) {
        using Writer = std::unique_ptr<archive, decltype(&archive_write_free)>;
        Writer w(archive_write_new(), archive_write_free);
        archive_write_set_format_zip(w.get());
        if (archive_write_open_filename_w(w.get(), staged.toStdWString().c_str()) != ARCHIVE_OK)
            fail("Ошибка 3MF");
        auto add = [&](const char *name, const QByteArray &data) {
            std::unique_ptr<archive_entry, decltype(&archive_entry_free)> e(archive_entry_new(),
                                                                            archive_entry_free);
            archive_entry_set_pathname(e.get(), name);
            archive_entry_set_filetype(e.get(), AE_IFREG);
            archive_entry_set_perm(e.get(), 0644);
            archive_entry_set_size(e.get(), data.size());
            if (archive_write_header(w.get(), e.get()) != ARCHIVE_OK ||
                archive_write_data(w.get(), data.data(), data.size()) != data.size())
                fail("Ошибка записи 3MF");
        };
        add("[Content_Types].xml",
            R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)");
        add("_rels/.rels",
            R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="r1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)");
        QFile spool(tmp.filePath("mesh.xml"));
        if (!spool.open(QIODevice::ReadWrite))
            fail(spool.errorString());
        QByteArray xml =
            R"(<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><resources><object id="1" type="model"><mesh><vertices>)";
        auto flush = [&] {
            if (spool.write(xml) != xml.size())
                fail(spool.errorString());
            xml.clear();
        };
        for (auto v : mesh.vertices) {
            xml += "<vertex x=\"" + QByteArray::number(v.x, 'g', 17) + "\" y=\"" +
                   QByteArray::number(v.y, 'g', 17) + "\" z=\"" + QByteArray::number(v.z, 'g', 17) + "\"/>";
            if (xml.size() >= 262144)
                flush();
        }
        xml += "</vertices><triangles>";
        for (auto t : mesh.triangles) {
            xml += "<triangle v1=\"" + QByteArray::number(t.a) + "\" v2=\"" + QByteArray::number(t.b) +
                   "\" v3=\"" + QByteArray::number(t.c) + "\"/>";
            if (xml.size() >= 262144)
                flush();
        }
        xml += "</triangles></mesh></object></resources><build><item objectid=\"1\"/></build></model>";
        flush();
        if (!spool.seek(0))
            fail(spool.errorString());
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(),
                                                                            archive_entry_free);
        archive_entry_set_pathname(entry.get(), "3D/3dmodel.model");
        archive_entry_set_filetype(entry.get(), AE_IFREG);
        archive_entry_set_perm(entry.get(), 0644);
        archive_entry_set_size(entry.get(), spool.size());
        if (archive_write_header(w.get(), entry.get()) != ARCHIVE_OK)
            fail("Ошибка заголовка 3MF");
        while (!spool.atEnd()) {
            auto block = spool.read(262144);
            if (block.isEmpty() ||
                archive_write_data(w.get(), block.constData(), block.size()) != block.size())
                fail("Ошибка записи модели 3MF");
        }
        if (archive_write_close(w.get()) != ARCHIVE_OK)
            fail("Ошибка завершения 3MF");
    } else
        fail("Неизвестный формат");
    QFile f(staged);
    QSaveFile dest(path);
    if (!f.open(QIODevice::ReadOnly) || !dest.open(QIODevice::WriteOnly))
        fail("Ошибка экспорта");
    while (!f.atEnd()) {
        auto block = f.read(65536);
        if (block.isEmpty() || dest.write(block) != block.size())
            fail("Ошибка копирования модели");
    }
    if (!dest.commit())
        fail(dest.errorString());
}
} // namespace raysculpt
