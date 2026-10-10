#include "../platform/surface_lighting.h"
#include <cstdio>
#include <cstdlib>

void check(bool condition,const char *message) {
    if(!condition) { std::fprintf(stderr,"%s\n",message);std::exit(1); }
}
SurfaceLight lamp(int id,float x) { return {{1,id,0,0,0,0},x,0,0,224,1,{1,0.5f,0.1f}}; }
void settle(SurfaceLightSelection &selection,const std::vector<SurfaceLight> &sources,const float *eye) {
    for(int n=0;n<30;++n) selection.update(sources,eye,1.0f/60);
}
int main() {
    float eye[3]={0,0,0};SurfaceLightSelection selection;
    std::vector<SurfaceLight> sources={lamp(0,0)};
    selection.update(sources,eye,1.0f/60);
    check(selection.slots.size()==1&&selection.slots[0].gain>0&&selection.slots[0].gain<0.1f,"New lamp must fade in");
    settle(selection,sources,eye);
    check(selection.slots[0].gain==1,"Nearby lamp should reach full strength");
    sources[0].z=32;selection.update(sources,eye,1.0f/60);
    check(selection.slots[0].light.z==32,"Moving sector must update lamp height without replacing identity");
    eye[0]=768;settle(selection,sources,eye);
    check(selection.slots[0].gain>0.45f&&selection.slots[0].gain<0.55f,"Old distance cutoff must lie inside a smooth fade");
    eye[0]=1000;selection.update(sources,eye,1.0f/60);
    check(!selection.slots.empty()&&selection.slots[0].gain>0,"Crossing range must fade out instead of dropping the lamp");
    settle(selection,sources,eye);check(selection.slots.empty(),"Out-of-range lamp must eventually release its slot");
    {
        // Two same-colored lamps in one merge cell, a wall between them.
        std::vector<SurfaceLight> pair={lamp(1,64),lamp(2,192)};
        check(mergeSurfaceLights(pair).size()==1,"Lamps in one cell must merge");
        pair[1].group=1;
        auto apart=mergeSurfaceLights(pair);
        check(apart.size()==2&&(apart[0].x==64||apart[1].x==64),"Lamps that cannot see each other must stay apart");
    }
    eye[0]=0;sources.clear();selection.clear();
    for(int n=0;n<25;++n)sources.push_back(lamp(n,100+n));
    settle(selection,sources,eye);check(selection.slots.size()==24,"Surface budget must stay bounded");
    sources[24].x=122;settle(selection,sources,eye);
    check(std::none_of(selection.slots.begin(),selection.slots.end(),[](const SurfaceLightSlot &s){return s.light.key[1]==24;}),"Small ranking changes must retain existing lamps");
    sources[24].x=0;selection.update(sources,eye,1.0f/60);
    check(selection.slots.size()==24,"Replacement must wait for a fading slot");
    check(std::any_of(selection.slots.begin(),selection.slots.end(),[](const SurfaceLightSlot &s){return s.gain>0&&s.gain<1;}),"Outgoing lamp must fade during budget replacement");
    settle(selection,sources,eye);
    check(std::any_of(selection.slots.begin(),selection.slots.end(),[](const SurfaceLightSlot &s){return s.light.key[1]==24&&s.gain==1;}),"Closer replacement must eventually become fully active");
    selection.update({},eye,1.0f/60);check(!selection.slots.empty(),"Removed material must fade out");
    settle(selection,{},eye);check(selection.slots.empty(),"Removed lights must release their slots");
    settle(selection,sources,eye);selection.clear();check(selection.slots.empty(),"Level reset must discard previous lights");
    settle(selection,sources,eye);selection.capacity=8;selection.update(sources,eye,1.0f/60);
    check(selection.slots.size()==24,"A lowered budget must fade extra lamps out, not drop them");
    settle(selection,sources,eye);
    check(selection.slots.size()==8&&std::all_of(selection.slots.begin(),selection.slots.end(),[](const SurfaceLightSlot &s){return s.light.x<108;}),
          "A lowered budget must keep the nearest lamps");
    selection.capacity=24;selection.clear();
    std::vector<SurfaceLight> pool;
    for(int u=0;u<4;++u) for(int v=0;v<4;++v) pool.push_back({{0,0,0,u,v,0},u*64.0f+32,v*64.0f+32,8,256,1.65f,{0.2f,1,0.1f},0});
    auto merged=mergeSurfaceLights(pool);
    check(merged.size()==1&&merged[0].radius>256&&merged[0].strength>1.65f,"Tiles of one cell must merge into one wider light");
    pool.push_back({{0,0,1,0,0,0},32,32,120,256,1.65f,{0.2f,1,0.1f},1});
    check(mergeSurfaceLights(pool).size()==2,"Floor and ceiling lights must stay separate");
    puts("Surface lighting range, movement, budget transitions and resets passed.");
}
