#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <STEPControl_Reader.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepLib.hxx>
#include <BRepTools.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <NCollection_Array1.hxx>
#include <NCollection_Array2.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
struct Profile {
    int grid;
    double width, thickness;
    bool linear;
    std::vector<double> relief;
};

Profile read_profile(const std::filesystem::path& path, bool linear) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    std::istringstream header(line);
    Profile profile{};
    profile.linear=linear;
    std::array<double,5> metadata{};
    if (!(header >> profile.grid >> profile.width >> profile.thickness))
        throw std::runtime_error("invalid profile header");
    for (auto& value : metadata) {
        if (!(header >> value)) throw std::runtime_error("expected eight header fields");
    }
    header >> std::ws;
    if (!header.eof() || profile.grid < 3 || profile.grid > 1024 ||
        !std::isfinite(profile.width) || profile.width <= 0 ||
        !std::isfinite(profile.thickness) || profile.thickness <= 0)
        throw std::runtime_error("invalid profile geometry");
    profile.relief.resize(std::size_t(profile.grid)*profile.grid);
    for (auto& value : profile.relief)
        if (!(file >> value) || !std::isfinite(value) || profile.thickness + value <= 0)
            throw std::runtime_error("invalid profile coefficient");
    file >> std::ws;
    if (!file.eof()) throw std::runtime_error("unexpected trailing profile data");
    return profile;
}

std::vector<double> clamp_axis(const std::vector<double>& values, bool linear) {
    const double near = values.front(), far = values.back();
    const double second = values[1], penultimate = values[values.size()-2];
    std::vector<double> clamped{linear ? near : (5*near + second)/6,
                                (2*near + second)/3};
    clamped.insert(clamped.end(), values.begin()+1, values.end()-1);
    clamped.push_back((2*far + penultimate)/3);
    clamped.push_back(linear ? far : (5*far + penultimate)/6);
    return clamped;
}

occ::handle<Geom_BSplineSurface> optical_face(const Profile& profile) {
    const int n = profile.grid;
    const double half = profile.width/2;
    std::vector<double> axis(n);
    for (int i = 0; i < n; ++i)
        axis[i] = i == n-1 ? half : -half + i*(profile.width/(n-1));
    const auto poles = clamp_axis(axis, true);
    std::vector<std::vector<double>> clamped;
    for (int i = 0; i < n; ++i) {
        const auto start = profile.relief.begin() + i*n;
        clamped.push_back(clamp_axis(std::vector<double>(start, start+n),profile.linear));
    }
    NCollection_Array2<gp_Pnt> points(1,n+2,1,n+2);
    NCollection_Array1<double> knots(1,n);
    NCollection_Array1<int> multiplicities(1,n);
    for (int i = 1; i <= n; ++i) {
        knots.SetValue(i,axis[i-1]);
        multiplicities.SetValue(i,i == 1 || i == n ? 4 : 1);
    }
    for (int u = 0; u < n+2; ++u) {
        std::vector<double> column(n);
        for (int row = 0; row < n; ++row) column[row] = clamped[row][u];
        const auto heights = clamp_axis(column,profile.linear);
        for (int v = 0; v < n+2; ++v)
            points.SetValue(u+1,v+1,gp_Pnt(poles[u],poles[v],profile.thickness+heights[v]));
    }
    return new Geom_BSplineSurface(points,knots,knots,multiplicities,multiplicities,3,3);
}

TopoDS_Edge reverse(const TopoDS_Edge& edge) {
    return TopoDS::Edge(edge.Reversed());
}

TopoDS_Face planar_face(const std::array<TopoDS_Edge,4>& edges) {
    BRepBuilderAPI_MakeWire wire;
    for (const auto& edge : edges) wire.Add(edge);
    return BRepBuilderAPI_MakeFace(wire.Wire(),true).Face();
}

TopoDS_Solid make_solid(const Profile& profile) {
    const auto surface=optical_face(profile);
    const double hi=profile.width/2, lo=-hi;
    const std::array<gp_Pnt,4> lower{gp_Pnt(lo,lo,0),gp_Pnt(hi,lo,0),
                                     gp_Pnt(hi,hi,0),gp_Pnt(lo,hi,0)};
    std::array<TopoDS_Edge,4> top{
        BRepBuilderAPI_MakeEdge(surface->VIso(lo)).Edge(), BRepBuilderAPI_MakeEdge(surface->UIso(hi)).Edge(),
        reverse(BRepBuilderAPI_MakeEdge(surface->VIso(hi)).Edge()),
        reverse(BRepBuilderAPI_MakeEdge(surface->UIso(lo)).Edge())};
    std::array<TopoDS_Edge,4> bottom, vertical;
    for (unsigned i = 0; i < 4; ++i) {
        bottom[i] = BRepBuilderAPI_MakeEdge(lower[i],lower[(i+1)%4]).Edge();
        vertical[i] = BRepBuilderAPI_MakeEdge(lower[i],surface->Value(lower[i].X(),lower[i].Y())).Edge();
    }
    BRepBuilderAPI_Sewing sew(1e-7);
    sew.Add(BRepBuilderAPI_MakeFace(surface,1e-7).Face());
    sew.Add(planar_face(bottom).Reversed());
    for (unsigned i = 0; i < 4; ++i)
        sew.Add(planar_face({bottom[i],vertical[(i+1)%4],reverse(top[i]),reverse(vertical[i])}));
    sew.Perform();
    TopExp_Explorer shells(sew.SewedShape(),TopAbs_SHELL);
    if (!shells.More() || sew.NbFreeEdges() || sew.NbMultipleEdges())
        throw std::runtime_error("CAD faces did not form one watertight shell");
    auto solid = BRepBuilderAPI_MakeSolid(TopoDS::Shell(shells.Current())).Solid();
    shells.Next();
    if (shells.More() || !BRepLib::OrientClosedSolid(solid) || !BRepCheck_Analyzer(solid).IsValid())
        throw std::runtime_error("invalid optical solid");
    return solid;
}

TopoDS_Shape apply_pose(const TopoDS_Shape& body, const std::filesystem::path& path) {
    std::ifstream file(path);
    std::array<double,12> p{};
    for (auto& item : p) {
        if (!(file >> item) || !std::isfinite(item))
            throw std::runtime_error("expected a finite 3 by 4 pose matrix");
    }
    file >> std::ws;
    if (!file.eof()) throw std::runtime_error("unexpected trailing pose data");
    gp_Trsf pose;
    pose.SetValues(p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11]);
    return BRepBuilderAPI_Transform(body,pose,true).Shape();
}

TopoDS_Shape transform(const TopoDS_Shape& shape, const std::array<double,12>& p) {
    gp_Trsf pose;
    pose.SetValues(p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11]);
    return BRepBuilderAPI_Transform(shape,pose,true).Shape();
}

double number(const QJsonValue& value) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()))
        throw std::runtime_error("expected a finite CAD number");
    return value.toDouble();
}

double positive(const QJsonObject& values, const char* key) {
    const auto value = number(values[key]);
    if (value <= 0) throw std::runtime_error("expected a positive CAD dimension");
    return value;
}

gp_Pnt point(const QJsonArray& p) {
    if (p.size()!=3) throw std::runtime_error("expected three CAD coordinates");
    return gp_Pnt(number(p[0]),number(p[1]),number(p[2]));
}

std::array<double,12> matrix(const QJsonArray& basis, const QJsonArray& offset) {
    if (basis.size()!=3 || offset.size()!=3) throw std::runtime_error("invalid CAD pose");
    std::array<double,12> values{};
    for (int i=0;i<3;++i) {
        const auto row=basis[i].toArray();
        if (row.size()!=3) throw std::runtime_error("invalid CAD basis");
        for (int j=0;j<3;++j) values[4*i+j]=number(row[j]);
        values[4*i+3]=number(offset[i]);
    }
    return values;
}

std::filesystem::path path_from_json(const QJsonValue& value) {
#ifdef _WIN32
    return value.toString().toStdWString();
#else
    return value.toString().toStdString();
#endif
}

TopoDS_Face sketch(const QJsonObject& item) {
    BRepBuilderAPI_MakeWire wire;
    if (item.contains("circle")) {
        const auto circle=item["circle"].toArray();
        if (circle.size()!=3 || number(circle[2])<=0) throw std::runtime_error("invalid circle");
        const gp_Ax2 axes(gp_Pnt(number(circle[0]),number(circle[1]),0),gp_Dir(0,0,1));
        wire.Add(BRepBuilderAPI_MakeEdge(gp_Circ(axes,number(circle[2]))).Edge());
    } else {
        const auto outline=item["outline"].toArray();
        if (outline.size()<3) throw std::runtime_error("invalid sketch outline");
        const auto arcs=item["arcs"].toObject();
        std::vector<gp_Pnt> points;
        for (const auto value : outline) {
            const auto p=value.toArray();
            if (p.size()!=2) throw std::runtime_error("invalid sketch point");
            points.emplace_back(number(p[0]),number(p[1]),0);
        }
        for (std::size_t i=0;i<points.size();++i) {
            const auto& a=points[i];
            const auto& b=points[(i+1)%points.size()];
            const auto rise=arcs[QString::number(i)].toDouble();
            if (std::abs(rise)>1e-8) {
                const gp_Vec delta(a,b);
                const auto midpoint=gp_Pnt((a.X()+b.X())/2-rise*delta.Y()/delta.Magnitude(),
                                           (a.Y()+b.Y())/2+rise*delta.X()/delta.Magnitude(),0);
                GC_MakeArcOfCircle arc(a,midpoint,b);
                if (!arc.IsDone()) throw std::runtime_error("invalid sketch arc");
                wire.Add(BRepBuilderAPI_MakeEdge(arc.Value()).Edge());
            } else {
                wire.Add(BRepBuilderAPI_MakeEdge(a,b).Edge());
            }
        }
    }
    BRepBuilderAPI_MakeFace face(wire.Wire(),true);
    if (!face.IsDone() || !BRepCheck_Analyzer(face.Face()).IsValid())
        throw std::runtime_error("invalid sketch wire or self-intersection");
    return face.Face();
}

template<typename Operation>
TopoDS_Shape solid_operation(Operation&& op) {
    if (!op.IsDone() || !TopExp_Explorer(op.Shape(),TopAbs_SOLID).More()
        || !BRepCheck_Analyzer(op.Shape()).IsValid())
        throw std::runtime_error("CAD operation did not produce a valid solid");
    return op.Shape();
}

TopoDS_Shape build_body(const QJsonObject& item, const std::filesystem::path& assets) {
    const auto kind=item["kind"].toString();
    const auto dims=item["dims"].toObject();
    TopoDS_Shape shape;
    if (kind=="plate" && item.contains("profile")) {
        shape=make_solid(read_profile(assets/path_from_json(item["profile"]),true));
    } else if (kind=="plate" || kind=="box") {
        const auto w=positive(dims,"w"), h=positive(dims,"h");
        const auto d=dims.contains("d") ? positive(dims,"d") : w;
        shape=BRepPrimAPI_MakeBox(gp_Pnt(-w/2,-d/2,0),w,d,h).Shape();
    } else if (kind=="sphere") {
        shape=BRepPrimAPI_MakeSphere(positive(dims,"r")).Shape();
    } else if (kind=="cylinder") {
        shape=BRepPrimAPI_MakeCylinder(positive(dims,"r"),positive(dims,"h")).Shape();
    } else if (kind=="prism") {
        shape=BRepPrimAPI_MakePrism(sketch(item),gp_Vec(0,0,positive(dims,"h"))).Shape();
    } else if (kind=="cad") {
        const auto file=assets/path_from_json(item["asset"]);
        const auto raw=file.u8string();
        const std::string name(raw.begin(),raw.end());
        if (file.extension()==".brep") {
            if (!BRepTools::Read(shape,name.c_str(),BRep_Builder()))
                throw std::runtime_error("BREP import failed");
        } else {
            STEPControl_Reader reader;
            if (reader.ReadFile(name.c_str())!=IFSelect_RetDone || !reader.TransferRoots())
                throw std::runtime_error("STEP import failed");
            shape=reader.OneShape();
        }
    } else {
        throw std::runtime_error("unsupported CAD solid kind");
    }
    if (item.contains("cut")) {
        Bnd_Box bounds;
        BRepBndLib::Add(shape,bounds);
        const auto low=bounds.CornerMin(), high=bounds.CornerMax();
        const double cut=number(item["cut"]);
        if (!(low.Z()<cut && cut<high.Z())) throw std::runtime_error("cut is outside the solid");
        const double margin=.02*std::max({1.,high.X()-low.X(),high.Y()-low.Y(),high.Z()-low.Z()});
        const auto mask=BRepPrimAPI_MakeBox(
            gp_Pnt(low.X()-margin,low.Y()-margin,low.Z()-margin),
            gp_Pnt(high.X()+margin,high.Y()+margin,cut)).Shape();
        shape=solid_operation(BRepAlgoAPI_Common(shape,mask));
    }
    for (const auto value : item["operations"].toArray()) {
        const auto op=value.toObject();
        if (op["suppressed"].toBool()) continue;
        const auto type=op["kind"].toString();
        if (type=="section") {
            const auto origin=point(op["origin"].toArray());
            const auto n=point(op["normal"].toArray());
            const gp_Dir normal(n.X(),n.Y(),n.Z());
            const auto face=BRepBuilderAPI_MakeFace(gp_Pln(origin,normal)).Face();
            const auto mask=BRepPrimAPI_MakeHalfSpace(face,origin.Translated(-gp_Vec(normal))).Solid();
            shape=solid_operation(BRepAlgoAPI_Common(shape,mask));
        } else {
            const auto tool=transform(build_body(op["operand"].toObject(),assets),
                                      matrix(op["basis"].toArray(),op["offset"].toArray()));
            if (type=="union") shape=solid_operation(BRepAlgoAPI_Fuse(shape,tool));
            else if (type=="subtract") shape=solid_operation(BRepAlgoAPI_Cut(shape,tool));
            else if (type=="intersect") shape=solid_operation(BRepAlgoAPI_Common(shape,tool));
            else throw std::runtime_error("unsupported CAD feature");
        }
    }
    return shape;
}

TopoDS_Shape body_request(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    if (!file) throw std::runtime_error("could not open CAD request");
    const std::string text{std::istreambuf_iterator<char>(file),{}};
    const auto root=QJsonDocument::fromJson(QByteArray(text.data(),qsizetype(text.size())));
    if (!root.isObject()) throw std::runtime_error("invalid CAD request");
    const auto request=root.object();
    return transform(build_body(request["body"].toObject(),path_from_json(request["assets"])),
                     matrix(request["basis"].toArray(),request["offset"].toArray()));
}


void write_solid(const TopoDS_Shape& shape, const std::filesystem::path& output, bool optical=true) {
    const auto encoded = output.u8string();
    const std::string path(encoded.begin(),encoded.end());
    if (output.extension() == ".brep") {
        if (!BRepTools::Write(shape,path.c_str())) throw std::runtime_error("BREP write failed");
    } else if (output.extension() == ".step" || output.extension() == ".stp") {
        STEPControl_Writer writer;
        Interface_Static::SetCVal("write.step.unit","MM");
        if (writer.Transfer(shape,optical ? STEPControl_ManifoldSolidBrep : STEPControl_AsIs) != IFSelect_RetDone
            || writer.Write(path.c_str()) != IFSelect_RetDone)
            throw std::runtime_error("STEP write failed");
    } else {
        throw std::invalid_argument("expected STEP or BREP output");
    }
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        if (argc < 4 || argc > 5)
            throw std::invalid_argument("caustic_cad_export PROFILE linear|clamped OUTPUT [POSE]");
        if (std::filesystem::path(argv[1])=="--body" && argc==4) {
            if (std::filesystem::exists(argv[3])) throw std::invalid_argument("output already exists");
            write_solid(body_request(argv[2]),argv[3],false);
            return 0;
        }
        const std::filesystem::path boundary = argv[2], output = argv[3];
        if (boundary != "linear" && boundary != "clamped")
            throw std::invalid_argument("invalid spline end convention");
        if (std::filesystem::exists(output))
            throw std::invalid_argument("export output must be a new file");
        const auto solid = make_solid(read_profile(argv[1], boundary == "linear"));
        write_solid(argc == 5 ? apply_pose(solid,argv[4]) : TopoDS_Shape(solid),output);
    } catch (const Standard_Failure& failure) {
        std::cerr << failure.GetMessageString() << '\n';
        return 1;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
    return 0;
}
