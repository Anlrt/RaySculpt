#include "geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#define RAYSCULPT_EXPORT __declspec(dllexport)
#else
#define RAYSCULPT_EXPORT
#endif

namespace {
using caustic::Vec3;
using caustic::cross;
using caustic::dot;

double axis(Vec3 p,int i) {
    return i==0 ? p.x : i==1 ? p.y : p.z;
}

struct Bounds {
    static constexpr double inf=std::numeric_limits<double>::infinity();
    std::array<double,3> low{inf,inf,inf}, high{-inf,-inf,-inf};
    void add(Vec3 point) {
        for (int i=0;i<3;++i) {
            low[i]=std::min(low[i],axis(point,i));
            high[i]=std::max(high[i],axis(point,i));
        }
    }
    bool intersects(Vec3 origin,Vec3 direction,double furthest) const {
        double near=-inf,far=furthest;
        for (int i=0;i<3;++i) {
            const auto p=axis(origin,i), d=axis(direction,i);
            if (d==0.) {
                if (p<low[i] || p>high[i]) return false;
            } else {
                const auto a=(low[i]-p)/d,b=(high[i]-p)/d;
                near=std::max(near,std::min(a,b));
                far=std::min(far,std::max(a,b));
                if (near>far) return false;
            }
        }
        return true;
    }
};

struct Triangle {
    Vec3 a,b,c,center;
};

struct Node {
    Bounds bounds;
    std::uint32_t first=0,count=0,left=0,right=0;
};

class Index {
    std::vector<Triangle> triangles;
    std::vector<std::uint32_t> order;
    std::vector<Node> nodes;
    std::uint32_t build(std::uint32_t first,std::uint32_t end) {
        Node node;
        Bounds centers;
        for (auto i=first;i<end;++i) {
            const auto& t=triangles[order[i]];
            node.bounds.add(t.a);
            node.bounds.add(t.b);
            node.bounds.add(t.c);
            centers.add(t.center);
        }
        const auto id=std::uint32_t(nodes.size());
        nodes.push_back(node);
        if (end-first<=8) {
            nodes[id].first=first;
            nodes[id].count=end-first;
        } else {
            int split=0;
            for (int i=1;i<3;++i)
                if (centers.high[i]-centers.low[i]>centers.high[split]-centers.low[split]) split=i;
            const auto middle=first+(end-first)/2;
            std::nth_element(order.begin()+first,order.begin()+middle,order.begin()+end,
                [&](auto a,auto b) { return axis(triangles[a].center,split)<axis(triangles[b].center,split); });
            const auto left=build(first,middle), right=build(middle,end);
            nodes[id].left=left;
            nodes[id].right=right;
        }
        return id;
    }

    void query(std::uint32_t node_id,Vec3 origin,Vec3 direction,
               std::uint32_t& nearest,double& distance) const {
        const auto& node=nodes[node_id];
        if (!node.bounds.intersects(origin,direction,distance)) return;
        if (!node.count) {
            query(node.left,origin,direction,nearest,distance);
            query(node.right,origin,direction,nearest,distance);
            return;
        }
        for (auto i=node.first;i<node.first+node.count;++i) {
            const auto index=order[i];
            const auto& t=triangles[index];
            const auto e1=t.b-t.a,e2=t.c-t.a,h=cross(direction,e2);
            const auto det=dot(e1,h);
            if (std::abs(det)<=1e-12) continue;
            const auto inv=1./det;
            const auto offset=origin-t.a;
            const auto u=dot(offset,h)*inv;
            if (u<0 || u>1) continue;
            const auto q=cross(offset,e1);
            const auto v=dot(direction,q)*inv;
            if (v<0 || u+v>1) continue;
            const auto ray_distance=dot(e2,q)*inv;
            if (ray_distance<distance || (ray_distance==distance && index<nearest)) {
                nearest=index;
                distance=ray_distance;
            }
        }
    }
public:
    Index(const double* values,std::size_t vertices,const std::uint32_t* indices,std::uint32_t faces) {
        const auto vertex=[&](std::uint32_t i) {
            if (i>=vertices) throw std::invalid_argument("invalid mesh index");
            const auto offset=3*std::size_t(i);
            return Vec3{values[offset],values[offset+1],values[offset+2]};
        };
        triangles.reserve(faces);
        order.resize(faces);
        for (std::size_t face=0;face<faces;++face) {
            const auto a=vertex(indices[3*face]),b=vertex(indices[3*face+1]),c=vertex(indices[3*face+2]);
            triangles.push_back({a,b,c,{(a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3}});
        }
        std::iota(order.begin(),order.end(),0);
        build(0,faces);
    }

    bool nearest(Vec3 origin,Vec3 direction,std::uint32_t& triangle,double& distance) const {
        triangle=std::numeric_limits<std::uint32_t>::max();
        distance=Bounds::inf;
        query(0,origin,direction,triangle,distance);
        return triangle!=std::numeric_limits<std::uint32_t>::max();
    }
};
}

extern "C" RAYSCULPT_EXPORT void* raysculpt_mesh_index_create_v1(
    const double* vertices,std::size_t vertex_count,const std::uint32_t* faces,std::uint32_t face_count) noexcept {
    if (!vertices || !faces || !vertex_count || !face_count || face_count>std::numeric_limits<std::uint32_t>::max()/2)
        return nullptr;
    try {
        return new Index(vertices,vertex_count,faces,face_count);
    } catch (...) {
        return nullptr;
    }
}

extern "C" RAYSCULPT_EXPORT int raysculpt_mesh_index_pick_v1(
    const void* handle,const double* origin,const double* direction,std::uint32_t* face,double* distance) noexcept {
    if (!handle || !origin || !direction || !face || !distance) return -1;
    return static_cast<const Index*>(handle)->nearest({origin[0],origin[1],origin[2]},
        {direction[0],direction[1],direction[2]},*face,*distance) ? 1 : 0;
}

extern "C" RAYSCULPT_EXPORT void raysculpt_mesh_index_release_v1(void* handle) noexcept {
    delete static_cast<Index*>(handle);
}
