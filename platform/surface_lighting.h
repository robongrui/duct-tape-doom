#ifndef DOOM_SURFACE_LIGHTING_H
#define DOOM_SURFACE_LIGHTING_H
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <vector>

// Identity follows the map surface and texture repeat, independent of view.
using SurfaceLightKey=std::array<int,6>;
struct SurfaceLight {
    SurfaceLightKey key;
    float x,y,z,radius,strength;
    std::array<float,3> color;
    int facing=0; // Merge group: 0 floor, 1 ceiling, 2+ wall normal octant.
    float extent=0; // Wall panels: half-height of the glowing span around z.
    int group=0; // Tiles of one merge cell that see each other (surfaceCell).
};
// The merge cell of a tile: 256-unit cell, surface direction, hue and
// visibility group. Emissive tiles repeat every 64 units, so one pool or
// panel run yields dozens of overlapping same-colored lights; each cell folds
// into one wider light. Cells are world-fixed, so merged identities stay
// stable as the view moves. The group keeps tiles in rooms that cannot see
// each other (two sides of a wall) from merging into a light inside it.
inline SurfaceLightKey surfaceCell(const SurfaceLight &light,float cell=256) {
    int hue=0;
    for(float c:light.color) hue=hue*4+std::clamp((int)std::lround(c*3),0,3);
    return {2,(int)std::floor(light.x/cell),(int)std::floor(light.y/cell),(int)std::floor(light.z/cell),light.facing,hue*256+light.group};
}
// Fold each merge cell (surfaceCell) into one wider light.
inline std::vector<SurfaceLight> mergeSurfaceLights(const std::vector<SurfaceLight> &sources,float cell=256) {
    struct Sum {float x=0,y=0,z=0,weight=0,strength=0,radius=0;std::array<float,3> color={};
                std::array<float,3> low={1e9f,1e9f,1e9f},high={-1e9f,-1e9f,-1e9f};float bottom=1e9f,top=-1e9f;int count=0;};
    std::map<SurfaceLightKey,Sum> cells;
    for(const auto &light:sources) {
        Sum &sum=cells[surfaceCell(light,cell)];
        float w=std::max(light.strength,0.001f),p[3]={light.x,light.y,light.z};
        sum.x+=light.x*w;sum.y+=light.y*w;sum.z+=light.z*w;sum.weight+=w;
        for(int c=0;c<3;++c) {
            sum.color[c]+=light.color[c]*w;
            sum.low[c]=std::min(sum.low[c],p[c]);sum.high[c]=std::max(sum.high[c],p[c]);
        }
        sum.strength=std::max(sum.strength,light.strength);
        sum.radius=std::max(sum.radius,light.radius);
        sum.bottom=std::min(sum.bottom,light.z-light.extent);sum.top=std::max(sum.top,light.z+light.extent);
        ++sum.count;
    }
    std::vector<SurfaceLight> merged;merged.reserve(cells.size());
    for(const auto &entry:cells) {
        const Sum &sum=entry.second;
        // Widen with the members' extent so the cell's edges stay lit.
        float spread=std::hypot(sum.high[0]-sum.low[0],sum.high[1]-sum.low[1],sum.high[2]-sum.low[2])*0.5f;
        float z=sum.z/sum.weight;
        merged.push_back({entry.first,sum.x/sum.weight,sum.y/sum.weight,z,sum.radius+spread*0.5f,
                          sum.strength*std::clamp(std::sqrt((float)sum.count)*0.5f,1.0f,1.5f),
                          {sum.color[0]/sum.weight,sum.color[1]/sum.weight,sum.color[2]/sum.weight},entry.first[4],
                          entry.first[4]>=2?std::max(sum.top-z,z-sum.bottom):0.0f});
    }
    return merged;
}
struct SurfaceLightSlot { SurfaceLight light; float gain=0; };
class SurfaceLightSelection {
public:
    std::vector<SurfaceLightSlot> slots;
    size_t capacity=24; // Lights kept at once; extra ones fade out.
    void clear() { slots.clear(); }
    void update(const std::vector<SurfaceLight> &sources,const float *eye,float seconds) {
        struct Candidate {const SurfaceLight *light;float score,target;};
        std::set<SurfaceLightKey> retained;
        for(const auto &slot:slots) retained.insert(slot.light.key);
        std::vector<Candidate> candidates;
        std::map<SurfaceLightKey,const SurfaceLight *> current;
        for(const auto &light:sources) {
            current[light.key]=&light;
            float dx=light.x-eye[0],dy=light.y-eye[1],dz=light.z-eye[2];
            float distance=std::sqrt(dx*dx+dy*dy+dz*dz);
            // Fade before leaving range; reserve a margin to prevent ranking jitter.
            float t=std::clamp((distance-640.0f)/256.0f,0.0f,1.0f);
            float target=1-t*t*(3-2*t);
            if(target>0) candidates.push_back({&light,distance-(retained.count(light.key)?64.0f:0.0f),target});
        }
        std::sort(candidates.begin(),candidates.end(),[](const Candidate &a,const Candidate &b) {
            if(a.score!=b.score) return a.score<b.score;
            return a.light->key<b.light->key;
        });
        if(candidates.size()>capacity) candidates.resize(capacity);
        std::map<SurfaceLightKey,float> targets;
        for(const auto &candidate:candidates) targets[candidate.light->key]=candidate.target;
        float step=std::clamp(seconds,0.0f,0.05f)/0.2f;
        for(auto &slot:slots) {
            auto source=current.find(slot.light.key);
            if(source!=current.end()) slot.light=*source->second;
            float target=targets.count(slot.light.key)?targets[slot.light.key]:0;
            slot.gain+=std::clamp(target-slot.gain,-step,step);
        }
        slots.erase(std::remove_if(slots.begin(),slots.end(),[](const SurfaceLightSlot &slot) {
            return slot.gain<=0;
        }),slots.end());
        retained.clear();for(const auto &slot:slots) retained.insert(slot.light.key);
        // Outgoing lights finish fading before their slots are reused.
        for(const auto &candidate:candidates) {
            if(slots.size()>=capacity) break;
            if(!retained.count(candidate.light->key))
                slots.push_back({*candidate.light,std::min(step,candidate.target)});
        }
    }
};
#endif
