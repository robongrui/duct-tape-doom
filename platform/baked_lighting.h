/* Lighting baked at level load on the 2.5D map: rays walk from sector to
 * sector through linedef crossings, like doom_flash_at, for sun visibility,
 * static point and pool lights and bounce light. Data-free so tests/baked_lighting.cpp
 * can check it without a WAD. */
#ifndef DOOM_BAKED_LIGHTING_H
#define DOOM_BAKED_LIGHTING_H
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// things: decorations standing in (or reaching into) the sector; see BakeThing.
struct BakeSector { float floor,ceiling; bool sky; std::vector<int> lines; std::vector<int> things={}; };
// A middle texture on a two-sided line, which blocks rays where it is opaque:
// palette index/coverage pairs, row-major. It repeats along the line (texture
// u = u + direction*distance from a) and spans bottom..top once, with row
// anchor - height.
struct BakeMask { const unsigned char *pixels=nullptr; int width=0,height=0; float u=0,direction=1,anchor=0,bottom=0,top=0; };
struct BakeLine { float ax,ay,bx,by; int front,back; BakeMask mask={}; }; // -1: no sector on that side.
// A decoration's sprite as an occluder: a billboard standing at (x,y) that
// turns to face each ray, so its shadow is the outline seen from the light.
// One texel per unit, columns from left (relative to x), rows down from top;
// pixels are palette index/coverage pairs.
struct BakeThing { float x,y,top,left; int width,height; const unsigned char *pixels; };
struct BakeMap { std::vector<BakeSector> sectors; std::vector<BakeLine> lines; std::vector<BakeThing> things={}; };
// What stopped a ray. Walls belong to the side facing sector, the one the ray
// came from; part is 0 for middle (one-sided or masked), 1 upper, 2 lower.
// A thing hit has no line.
struct BakeHit { enum Kind { none, floor, ceiling, wall, thing } kind=none; int sector=-1,line=-1,part=0; float x=0,y=0,z=0; };

// True where the line's middle texture covers the point at fraction u along it.
inline bool maskBlocks(const BakeLine &line,float u,float height) {
    const BakeMask &m=line.mask;
    if(!m.pixels||m.width<1||m.height<1||height<m.bottom||height>=m.top)return false;
    float distance=u*std::hypot(line.bx-line.ax,line.by-line.ay);
    int x=(int)std::floor(m.u+m.direction*distance)%m.width;if(x<0)x+=m.width;
    int y=std::clamp((int)std::floor(m.anchor-height),0,m.height-1);
    return m.pixels[2*((size_t)y*m.width+x)+1]!=0;
}

// Where a ray (from origin, along direction, between from and to) passes the
// billboard's plane, in ray length; true where the sprite covers that point.
inline bool thingBlocks(const BakeThing &thing,float x,float y,float z,const float direction[3],float from,float to,float &at) {
    float h2=direction[0]*direction[0]+direction[1]*direction[1];
    if(h2<1e-6f)return false;
    float t=((thing.x-x)*direction[0]+(thing.y-y)*direction[1])/h2;
    if(t<=from+1e-3f||t>=to)return false;
    float px=x+direction[0]*t,py=y+direction[1]*t,pz=z+direction[2]*t;
    float lateral=((px-thing.x)*direction[1]-(py-thing.y)*direction[0])/std::sqrt(h2);
    int column=(int)std::floor(lateral-thing.left),row=(int)std::floor(thing.top-pz);
    if(column<0||row<0||column>=thing.width||row>=thing.height)return false;
    if(!thing.pixels[2*((size_t)row*thing.width+column)+1])return false;
    at=t;return true;
}
// How a ray treats sky ceilings. Doom draws nothing above one, so a sun ray
// escapes through it, and a light ray passes over it as if it were open air.
enum class SkyCeiling { escape, open };
// True when the ray from (x,y,z) inside sector along a unit direction travels
// length without hitting a wall, floor or ceiling, or (escape) leaves through
// a sky ceiling. Upper walls under a non-sky ceiling block; crossing above a
// sky neighbor's ceiling from a sky sector does not. A blocked ray reports
// what stopped it in hit.
inline bool traceRay(const BakeMap &map,int sector,float x,float y,float z,const float direction[3],float length,SkyCeiling sky,BakeHit *hit=nullptr) {
    float t=0;int previous=-1;
    auto stop=[&](BakeHit::Kind kind,float at,int line,int part) {
        if(hit) *hit={kind,sector,line,part,x+direction[0]*at,y+direction[1]*at,z+direction[2]*at};
        return false;
    };
    for(int step=0;step<256&&sector>=0;++step) {
        const BakeSector &s=map.sectors[sector];
        bool openTop=s.sky&&sky==SkyCeiling::open;
        float exit=INFINITY,best=INFINITY,along=0;int crossed=-1;
        if(direction[2]>0&&!openTop) exit=(s.ceiling-z)/direction[2];
        if(direction[2]<0) exit=(s.floor-z)/direction[2];
        for(int index:s.lines) {
            if(index==previous)continue;
            const BakeLine &line=map.lines[index];
            float ex=line.bx-line.ax,ey=line.by-line.ay,ox=line.ax-x,oy=line.ay-y;
            float det=direction[0]*ey-direction[1]*ex;
            if(std::fabs(det)<1e-9f)continue;
            float crossing=(ox*ey-oy*ex)/det,u=(ox*direction[1]-oy*direction[0])/det;
            if(crossing>t+1e-4f&&crossing<best&&u>=0&&u<=1){best=crossing;along=u;crossed=index;}
        }
        float nearest=std::min({exit,best,length}),hitThing=INFINITY,at;
        for(int index:s.things)
            if(thingBlocks(map.things[index],x,y,z,direction,t,nearest,at)&&at<hitThing)hitThing=at;
        if(hitThing<INFINITY)return stop(BakeHit::thing,hitThing,-1,0);
        if(length<=exit&&length<=best)return true;
        if(exit<=best) {
            if(direction[2]>0&&s.sky&&sky==SkyCeiling::escape)return true;
            return stop(direction[2]>0?BakeHit::ceiling:BakeHit::floor,std::max(exit,t),-1,0);
        }
        const BakeLine &line=map.lines[crossed];
        int next=line.front==sector?line.back:line.front;
        if(next<0)return stop(BakeHit::wall,best,crossed,0);
        const BakeSector &n=map.sectors[next];
        float height=z+direction[2]*best;
        if(height<n.floor)return stop(BakeHit::wall,best,crossed,2);
        if(height>n.ceiling) {
            if(!(s.sky&&n.sky))return stop(BakeHit::wall,best,crossed,1);
            if(sky==SkyCeiling::escape)return true;
        }
        if(maskBlocks(line,along,height))return stop(BakeHit::wall,best,crossed,0);
        sector=next;previous=crossed;t=best;
    }
    return false;
}
inline bool sunReachesSky(const BakeMap &map,int sector,float x,float y,float z,const float direction[3]) {
    return traceRay(map,sector,x,y,z,direction,INFINITY,SkyCeiling::escape);
}
// Fraction of four rays, spread over a few degrees of sun disc on a rotated
// grid, that reach the sky: a stepped five-level penumbra that widens with
// distance from the occluder. Rays start at the cell center so they never
// begin outside their sector.
inline float sunVisibility(const BakeMap &map,int sector,float x,float y,float z,const float direction[3]) {
    static const float offsets[4][2]={{-0.125f,-0.375f},{0.375f,-0.125f},{0.125f,0.375f},{-0.375f,0.125f}};
    int open=0;
    for(const auto &o:offsets) {
        float d[3]={direction[0]+o[0]*0.1f,direction[1]+o[1]*0.1f,direction[2]};
        float length=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
        for(float &c:d)c/=length;
        open+=sunReachesSky(map,sector,x,y,z,d);
    }
    return open/4.0f;
}
// A static light with the same falloff and facing as flashAtOpen and
// flashFacing in platform/shaders/lighting.glsl; without a normal (sprites)
// there is no facing. Returns the amount added (before color); direction,
// if given, receives the unit vector toward the light when it reaches the point.
struct BakeLight { float x,y,z,radius,strength,directionality; float color[3]; };
inline float addBakedLight(const BakeMap &map,const BakeLight &light,int sector,float x,float y,float z,const float normal[3],float out[3],float *direction=nullptr) {
    float d[3]={light.x-x,light.y-y,light.z-z};
    float distance=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    if(distance>=light.radius)return 0;
    float falloff=1-distance/light.radius,amount=light.strength*falloff*falloff*(3-2*falloff);
    float facing=1;
    if(distance>0.001f) {
        for(float &c:d)c/=distance;
        if(normal)facing=1+light.directionality*(std::max(0.0f,normal[0]*d[0]+normal[1]*d[1]+normal[2]*d[2])-1);
        // Stop just short of the light so its own surface never blocks it.
        if(amount*facing<1/512.0f||!traceRay(map,sector,x,y,z,d,distance-0.5f,SkyCeiling::open))return 0;
    } else d[0]=d[1]=0,d[2]=1;
    for(int c=0;c<3;++c)out[c]+=light.color[c]*amount*facing;
    if(direction)for(int c=0;c<3;++c)direction[c]=d[c];
    return amount*facing;
}
// An emissive liquid floor (or ceiling) lit as one light, so a pool glows as
// a whole instead of as a grid of points: light comes from the nearest point
// of its convex pieces (BSP leaves), lifted off the surface like the point
// lights that would otherwise tile it. Only the side it faces receives light.
// Its glow spills over a pit's rim: rays test against a point clearance
// above the surface (below, for ceilings) rather than the lifted point.
struct BakeArea {
    std::vector<std::vector<std::array<float,2>>> pieces;
    float z=0,clearance=48; bool ceiling=false;
    float radius=256,strength=1,directionality=0.35f; float color[3]={1,1,1};
    float low[2]={1e9f,1e9f},high[2]={-1e9f,-1e9f}; // Bounds of the pieces.
};
constexpr float areaLift=8;
// Squared distance in the plane from (x,y) to the area (0 inside), and the
// nearest point. Pieces may wind either way.
inline float areaNearest(const BakeArea &area,float x,float y,float &nx,float &ny) {
    float best=INFINITY;nx=x;ny=y;
    for(const auto &piece:area.pieces) {
        size_t n=piece.size();if(n<3)continue;
        bool positive=false,negative=false;float local=INFINITY,px=x,py=y;
        for(size_t i=0;i<n;++i) {
            const auto &a=piece[i],&b=piece[(i+1)%n];
            float ex=b[0]-a[0],ey=b[1]-a[1],cross=ex*(y-a[1])-ey*(x-a[0]);
            positive|=cross>0.01f;negative|=cross<-0.01f;
            float length2=ex*ex+ey*ey,t=length2>0?std::clamp(((x-a[0])*ex+(y-a[1])*ey)/length2,0.0f,1.0f):0;
            float qx=a[0]+ex*t,qy=a[1]+ey*t,d=(x-qx)*(x-qx)+(y-qy)*(y-qy);
            if(d<local) {local=d;px=qx;py=qy;}
        }
        if(!(positive&&negative)) {nx=x;ny=y;return 0;}
        if(local<best) {best=local;nx=px;ny=py;}
    }
    return best;
}
// Like addBakedLight for an area. A ray to the nearest point can graze the
// pool's rim, so points 32 and 96 units further in light what sees past it;
// the first one that reaches the point counts. inside, if given, tells
// whether the point lies over the area itself (the pool's own surface).
inline float addBakedArea(const BakeMap &map,const BakeArea &area,int sector,float x,float y,float z,const float normal[3],float out[3],float *direction=nullptr,bool *inside=nullptr) {
    if(inside)*inside=false;
    if((area.ceiling?area.z-z:z-area.z)<=0)return 0;
    float bx=std::max({area.low[0]-x,0.0f,x-area.high[0]}),by=std::max({area.low[1]-y,0.0f,y-area.high[1]});
    if(bx*bx+by*by>=area.radius*area.radius)return 0;
    float nx,ny,plane=std::sqrt(areaNearest(area,x,y,nx,ny));
    if(plane>=area.radius)return 0;
    float ux=plane>0.001f?(nx-x)/plane:0,uy=plane>0.001f?(ny-y)/plane:0;
    float ez=area.z+(area.ceiling?-areaLift:areaLift);
    for(float reach:{0.0f,32.0f,96.0f}) {
        if(reach>0&&plane<=0.001f)break;
        float tx=nx+ux*reach,ty=ny+uy*reach,ix,iy;
        if(reach>0&&areaNearest(area,tx,ty,ix,iy)>0)break;
        float d[3]={tx-x,ty-y,ez-z},distance=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
        if(distance>=area.radius)break;
        float falloff=1-distance/area.radius,amount=area.strength*falloff*falloff*(3-2*falloff),facing=1;
        if(distance>0.001f) {
            for(float &c:d)c/=distance;
            if(normal)facing=1+area.directionality*(std::max(0.0f,normal[0]*d[0]+normal[1]*d[1]+normal[2]*d[2])-1);
            if(amount*facing<1/512.0f)continue;
            float tz=area.z+(area.ceiling?-1:1)*std::max(area.clearance,areaLift);
            float v[3]={tx-x,ty-y,tz-z},length=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
            for(float &c:v)c/=std::max(length,0.001f);
            if(length>0.5f&&!traceRay(map,sector,x,y,z,v,length-0.5f,SkyCeiling::open))continue;
        } else d[0]=d[1]=0,d[2]=area.ceiling?-1.0f:1.0f;
        for(int c=0;c<3;++c)out[c]+=area.color[c]*amount*facing;
        if(direction)for(int c=0;c<3;++c)direction[c]=d[c];
        if(inside)*inside=plane<=0.001f;
        return amount*facing;
    }
    return 0;
}
// Bounce gathering direction around a unit normal: (u,v) in [0,1)^2 maps to
// a cosine-weighted hemisphere, so averaging what the rays hit is the
// irradiance up to albedo.
inline void cosineDirection(const float normal[3],float u,float v,float out[3]) {
    float radius=std::sqrt(u),angle=6.28318531f*v;
    float a=radius*std::cos(angle),b=radius*std::sin(angle),c=std::sqrt(std::max(0.0f,1-u));
    float helper[3]={std::fabs(normal[2])<0.9f?0.0f:1.0f,0,std::fabs(normal[2])<0.9f?1.0f:0.0f};
    float tangent[3]={helper[1]*normal[2]-helper[2]*normal[1],helper[2]*normal[0]-helper[0]*normal[2],helper[0]*normal[1]-helper[1]*normal[0]};
    float length=std::sqrt(tangent[0]*tangent[0]+tangent[1]*tangent[1]+tangent[2]*tangent[2]);
    for(float &t:tangent)t/=length;
    float bitangent[3]={normal[1]*tangent[2]-normal[2]*tangent[1],normal[2]*tangent[0]-normal[0]*tangent[2],normal[0]*tangent[1]-normal[1]*tangent[0]};
    for(int i=0;i<3;++i)out[i]=tangent[i]*a+bitangent[i]*b+normal[i]*c;
}
#endif
