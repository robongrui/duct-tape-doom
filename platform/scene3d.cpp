/* Platform-neutral 3D scene built from the loaded map. See scene3d.h. */
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <queue>
#include <unordered_map>
#include <vector>
#include <ctime>
#include <functional>
#include <random>
#include <string>
#include <thread>
extern "C" {
#include "doomdef.h"
#include "doomstat.h"
#include "r_state.h"
#include "r_sky.h"
#include "doomdata.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"
#include "m_swap.h"
#include "m_argv.h"
#include "i_system.h"
#include "r_main.h"
extern int viewwindowx, viewwindowy;
extern boolean menuactive, automapactive, paused;
extern int numflats;
int P_PicAnimationNext(boolean istexture,int pic,int *next);
}
#include <SDL3/SDL.h>
#include "i_render3d.h"
#include "scene3d.h"
#include "emissive.h"
#include "screen_glass.h"
#include "detail_texture.h"
#include "surface_lighting.h"
#include "baked_lighting.h"

namespace doom3d {
Settings settings;
SDL_Window *gameWindow;
FlashSet flashes;
FogLights fogLights;
std::vector<LightBlocker> lightBlockers;
std::unordered_map<int,Image> images;
std::map<int,std::vector<Vertex>> batches,shadowBatches,decalBatches;
std::vector<Vertex> mistVertices,particleVertices,heatVertices,skyVertices,shaftVertices;
GpuTextureRef seamTexture,contactTexture,cloudTexture,wallBakeTexture,flatLightTexture,causticTexture,causticPattern,shoreTexture;
GpuTextureRef wallDirectionTexture,flatDirectionTexture;
float reflectionPlane=0;
bool reflectionActive=false;
bool worldPending,screenshotPending,flashlightOn=false;
float pitch;

namespace {
struct FlashPulse { float x,y,z,radius,strength; int tic; const mobj_t *source; std::array<float,3> color; };
std::array<const mobj_t*,maxLights> lightSources={};
// Lights kept only for the fog glow (bake-only static lights): no surface,
// sprite or shadow uses them.
std::array<bool,maxLights> fogOnly={};
std::vector<FlashPulse> flashPulses;
unsigned flashLevelSerial;
std::unordered_map<unsigned,std::array<float,3>> glowColors;
std::vector<std::vector<Point>> floors;
std::vector<std::vector<int>> sectorFloors;
std::vector<std::array<float,4>> floorBounds;
// hot: lava, or a damaging floor whose flat is mostly red or orange (heat haze).
struct SectorMist { bool pit=false,liquid=false,toxic=false,outdoor=false,reflective=false,hot=false; std::array<float,3> color={0.24f,0.29f,0.32f}; };
std::vector<SectorMist> sectorMist;
float fogReferenceHeight=0;
unsigned levelSerial;
std::array<float,3> flashlightTint={},flashlightTargetTint={};
uint64_t flashlightSampleTime;
uint64_t flashlightTime;
unsigned flashlightLevelSerial;
float oldEye[3], oldYaw;
// Sector heights at the previous tic; doors and lifts blend like moving things.
std::vector<std::array<float,2>> oldHeights;
float renderFraction=1;
// Map-space surface data, built once per level from the linedefs. Each cell
// stores its sector, the nearest open neighbor sector with a blend weight,
// the sector its light flows from (bakeFlow), and floor/ceiling distances to
// walls for contact shading.
constexpr uint16_t noSector=0xFFFF;
constexpr float seamBand=16,contactRange=48;
struct SeamCell { uint16_t own=noSector,neighbor=noSector,weight=0,flow=noSector; };
std::vector<SeamCell> seamCells;
// RGBA8 per cell: floor and ceiling contact distance, baked sun visibility,
// light flow weight.
std::vector<uint8_t> contactCells;
bool sunBaked=false;
float sunLevel=0; // Typical sky-sector light; indoor sun patches rise toward it.
std::array<float,3> sunTint={1,1,1};
float sunAzimuth[2]={1,0}; // Horizontal direction toward the baked sun (40 degrees high).
float sunDirection[3]={0,0,1};
// The sun drawn in the sky: yaw of the baked direction, and its strength
// from how bright the sky texture is around it (a dark sky shows only a hint).
float sunYaw=0,sunGlow=0;
// Sky sectors joined by sky-to-sky lines form one opening (a hole may be
// several sectors, nested or side by side). Its outer edge decides how
// enclosed it is: indoor lines lead into non-sky sectors.
struct SkyOpening { float edge=0; std::vector<std::pair<const sector_t*,float>> indoor; };
std::vector<int> skyOpeningOf;std::vector<SkyOpening> skyOpenings;
// Sunbeams (bakeShafts): from a sunlit indoor floor point (x,y,z) back toward
// the sun for length units, until the ray leaves the building.
struct SunShaft { float x,y,z,length,strength; int sector; };
std::vector<SunShaft> sunShafts;
// Walls: one strip per linedef side (index line*2+side) in an RGBA8 atlas
// (rgb baked light at half scale, a sun), columns along the side from its
// first drawn vertex at a, rows up from low to the highest ceiling its
// sectors and their neighbors reach. The atlas's right half holds bounce
// light at the same coordinates.
struct BakeStrip { int x=-1,y=0,width=0,height=0,sector=0; float low=0,high=0,scale=0,ax=0,ay=0,dx=0,dy=0,length=0; };
std::vector<BakeStrip> bakeStrips;
std::vector<int> bakeOrder; // Strips that have texels, tallest first.
constexpr int atlasWidth=4096,atlasStride=2*atlasWidth;
int atlasHeight=0;
std::vector<uint8_t> wallBakeCells;
// Four map-sized layers stacked in rows: floor light, ceiling light, floor
// bounce, ceiling bounce; rgb at half scale.
enum FlatLayer { floorLight, ceilingLight, floorBounce, ceilingBounce };
std::vector<uint8_t> flatLightCells;
BakeMap bakeMap;
// Static lights were baked from these inputs (see lightBakeInputs); -1
// before the level's first light bake. The serial counts light bakes.
int lightBakeKey=-1;
unsigned lightBakeSerial=0;
bool bakedLightsActive=false; // This frame's lights rely on the bake.
bool bakeOnlyActive=false; // ...and static lights come only from it.
// The baked lights, kept for sprites, and a coarse grid listing the lights
// that can reach each of its cells.
std::vector<BakeLight> bakeSources;
constexpr float bakeGridCell=128;
int bakeGridWidth=0,bakeGridHeight=0;
std::vector<std::vector<int>> bakeGrid;
// Bake-only lights: liquid pools as area lights, listed in bakeGrid after the
// point lights; each point light's flicker group (0: steady); and per wall
// atlas texel and floor/ceiling cell where the static light comes from.
std::vector<BakeArea> bakeAreas;
std::vector<uint8_t> sourceGroups;
std::vector<uint8_t> wallDirectionCells,flatDirectionCells;
constexpr int flickerGroups=16;
// Static light at a point as things take it, without facing: the color
// (unflickered), where it comes from (unit directions weighted by each
// light's luminance, and their total), the strongest flicker group's share,
// and the strongest point light not far below that can cast a thing's shadow.
struct StaticSample {
    std::array<float,3> color={},towards={};float amount=0;
    int group=0;float groupShare=0;
    int shadow=-1;float shadowAmount=0;
};
// Grid lighting cells (bakeStaticGrid).
constexpr float staticCell=32;
int staticWidth=0,staticHeight=0;
unsigned staticSerial=0;
struct StaticCell { bool used=false; float low=0,high=0; StaticSample at[2]; };
std::vector<StaticCell> staticGrid;
// Bounce light was baked from these inputs (see bounceInputs); -1: not yet.
int bounceKey=-1;
constexpr float bounceStrength=1.0f;
// Decorations in the bakes (settings.thingShadows) as last synced; -1 before
// the level's first sync. Sky/occlusion and flow were baked with this key.
int bakeThingsKey=-1,ambientKey=-1,flowKey=-1;
float skyLevel=0; // Typical sky-sector light; indoor sky light rises toward it.
std::array<float,3> skyTint={1,1,1};
float mapOrigin[2],mapCell=4;int mapWidth=0,mapHeight=0;
uint8_t *flatCell(FlatLayer layer,size_t cell) {return &flatLightCells[4*((size_t)layer*mapWidth*mapHeight+cell)];}
uint64_t tickTime, titleTime;
int frameCount;
float fps;
const char *graphicsConfig="graphics.cfg";
std::unordered_map<const mobj_t*, std::array<float,3>> oldThings;
std::vector<SurfaceLight> surfaceLights;
float surfaceEye[3];
SurfaceLightSelection surfaceSelection;
uint64_t surfaceLightTime;
struct FlatLightSample { Point point; int u,v; };
std::map<std::pair<int,int>,std::vector<FlatLightSample>> flatLightSamples;
} // namespace
// Waits for the background re-bake around moving sectors (relightMovingSectors)
// and, with apply, uploads what it baked. Every full bake calls it first.
void finishRelight(bool apply=true);
extern std::vector<int> relightPending;

std::vector<unsigned> paletteLUTColors(const unsigned *palette) {
    std::vector<unsigned> colors(32*32*32);
    for(int b=0;b<32;++b)for(int g=0;g<32;++g)for(int r=0;r<32;++r) {
        int red=(r*255+15)/31,green=(g*255+15)/31,blue=(b*255+15)/31,best=0,bestDistance=INT_MAX;
        for(int i=0;i<256;++i) {
            int dr=red-int((palette[i]>>16)&255),dg=green-int((palette[i]>>8)&255),db=blue-int(palette[i]&255);
            int distance=dr*dr+dg*dg+db*db;
            if(distance<bestDistance) {bestDistance=distance;best=i;}
        }
        colors[(b*32+g)*32+r]=palette[best];
    }
    return colors;
}
void buildCloudTexture() {
    // Periodic, deterministic low-frequency cloud texture; no downloaded assets.
    std::array<byte,64*64> pixels;
    for(int y=0;y<64;++y)for(int x=0;x<64;++x) {
        float u=x*doomPi/32,v=y*doomPi/32;
        float cloud=0.5f+0.19f*sin(u+sin(v))+0.16f*cos(v*2-u)+0.10f*sin(u*3+v*2);
        pixels[y*64+x]=(byte)(std::clamp(cloud,0.0f,1.0f)*255);
    }
    cloudTexture=gpuCreateTexture(GpuFormat::R8,64,64,pixels.data(),64);
    if(!cloudTexture)I_Error((char*)"Could not allocate cloud texture");
}
// Puffy sprite normals: a chamfer distance field over the alpha mask gives a
// rounded height profile; its gradient tilts the normal toward the silhouette.
// Packed as unsigned x/y (texture down) for the sprite's B/A channels.
std::vector<byte> spriteNormals(int w,int h,const byte *pixels) {
    std::vector<float> distance((size_t)w*h);
    for(size_t i=0;i<distance.size();++i)distance[i]=pixels[2*i+1]?1e6f:0;
    auto at=[&](int x,int y){return x<0||y<0||x>=w||y>=h?0.0f:distance[(size_t)y*w+x];};
    for(int pass=0;pass<2;++pass) {
        int step=pass?-1:1;
        for(int y=pass?h-1:0;y>=0&&y<h;y+=step)for(int x=pass?w-1:0;x>=0&&x<w;x+=step) {
            float &d=distance[(size_t)y*w+x];if(d==0)continue;
            d=std::min({d,at(x-step,y)+1,at(x,y-step)+1,at(x-step,y-step)+1.4142f,at(x+step,y-step)+1.4142f});
        }
    }
    float radius=std::clamp(std::min(w,h)*0.25f,3.0f,12.0f);
    auto height=[&](int x,int y){float t=std::min(1.0f,at(x,y)/radius);return std::sqrt(std::max(0.0f,1-(1-t)*(1-t)));};
    std::vector<byte> normals((size_t)w*h*2,128);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        if(!pixels[((size_t)y*w+x)*2+1])continue;
        float gx=(height(x+1,y)-height(x-1,y))*0.5f,gy=(height(x,y+1)-height(x,y-1))*0.5f;
        float nx=-gx*radius,ny=-gy*radius,length=std::sqrt(nx*nx+ny*ny+1);
        normals[((size_t)y*w+x)*2]=(byte)std::lround((nx/length*0.5f+0.5f)*255);
        normals[((size_t)y*w+x)*2+1]=(byte)std::lround((ny/length*0.5f+0.5f)*255);
    }
    return normals;
}
// Palette mip levels for walls and flats, as software Quake built them: each
// level texel averages the colors it covers (weighted by coverage) and snaps
// back to the nearest PLAYPAL entry, so the shader keeps applying the live
// palette (damage and pickup flashes). Coverage and the emissive mask
// average; monitor glass takes the middle texel. Returns every level after
// the first, packed at the texture's channel count, and counts all levels.
std::vector<byte> paletteMips(int w,int h,const byte *base,int channels,int &levels) {
    const byte *palette=(const byte*)W_CacheLumpName((char*)"PLAYPAL",PU_CACHE);
    // Nearest entries by 5-bit color cell, filled as cells are first used.
    static std::array<byte,768> cachedPalette={};
    static std::vector<short> nearest;
    if(nearest.empty()||memcmp(cachedPalette.data(),palette,768)) {
        memcpy(cachedPalette.data(),palette,768);nearest.assign(32*32*32,-1);
    }
    auto snap=[&](float r,float g,float b) {
        int cell=(std::min(31,(int)(r/8))<<10)|(std::min(31,(int)(g/8))<<5)|std::min(31,(int)(b/8));
        if(nearest[cell]<0) {
            float cr=(cell>>10)*8+4.0f,cg=((cell>>5)&31)*8+4.0f,cb=(cell&31)*8+4.0f,best=1e30f;
            for(int i=0;i<256;++i) {
                float dr=palette[i*3]-cr,dg=palette[i*3+1]-cg,db=palette[i*3+2]-cb;
                float d=2*dr*dr+4*dg*dg+3*db*db;
                if(d<best) {best=d;nearest[cell]=(short)i;}
            }
        }
        return (byte)nearest[cell];
    };
    std::vector<byte> out;levels=1;
    for(int lw=w,lh=h;lw>1||lh>1;++levels) {
        lw=std::max(1,lw/2);lh=std::max(1,lh/2);
        for(int y=0;y<lh;++y) for(int x=0;x<lw;++x) {
            int x0=x*w/lw,x1=std::max(x0+1,(x+1)*w/lw),y0=y*h/lh,y1=std::max(y0+1,(y+1)*h/lh);
            double rgb[3]={},coverage=0,mask=0;int count=0;
            for(int sy=y0;sy<y1;++sy) for(int sx=x0;sx<x1;++sx) {
                const byte *p=base+((size_t)sy*w+sx)*channels;
                for(int c=0;c<3;++c)rgb[c]+=palette[p[0]*3+c]*(double)p[1];
                coverage+=p[1];if(channels==4)mask+=p[2];++count;
            }
            const byte *middle=base+((size_t)((y0+y1)/2)*w+(x0+x1)/2)*channels;
            out.push_back(coverage>0?snap(float(rgb[0]/coverage),float(rgb[1]/coverage),float(rgb[2]/coverage)):middle[0]);
            out.push_back((byte)std::lround(coverage/count));
            if(channels==4) {out.push_back((byte)std::lround(mask/count));out.push_back(middle[3]);}
        }
    }
    return out;
}
// Channels: palette index, coverage, then the emissive mask (or sprite
// normal x) and, on walls and flats, monitor glass height (or sprite normal y).
// mipmaps: walls and flats also get palette mip levels.
Image upload(int w,int h,const byte *pixels,int left=0,int top=0,const byte *mask=nullptr,const byte *normals=nullptr,const byte *glass=nullptr,bool mipmaps=false) {
    Image image; image.pixels.assign(pixels,pixels+(size_t)w*h*2); image.width=w; image.height=h; image.left=left; image.top=top;
    for(size_t i=0;i<(size_t)w*h;++i)if(pixels[2*i+1]!=255) {image.opaque=false;break;}
    std::vector<byte> packed;
    bool wide=mask||normals||glass;
    if(wide) {
        packed.resize((size_t)w*h*4);
        for(size_t i=0;i<(size_t)w*h;++i) {
            packed[4*i]=pixels[2*i];packed[4*i+1]=pixels[2*i+1];
            packed[4*i+2]=normals?normals[2*i]:mask?mask[i]:0;
            packed[4*i+3]=normals?normals[2*i+1]:glass?glass[i]:DOOM_GLASS_NONE;
        }
    }
    if(mask) {
        int lump=W_GetNumForName((char*)"PLAYPAL");
        if(W_LumpLength(lump)<768) I_Error((char*)"Invalid emissive palette");
        const byte *palette=(const byte*)W_CacheLumpNum(lump,PU_CACHE);
        double weight=0;
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
            size_t i=(size_t)y*w+x;float e=mask[i]/255.0f;
            weight+=e;image.emissionU+=(x+0.5f)*e;image.emissionV+=(y+0.5f)*e;
            for(int c=0;c<3;++c) image.emissionColor[c]+=palette[pixels[2*i]*3+c]/255.0f*e;
        }
        if(weight>0) {
            image.emissionU/=weight;image.emissionV/=weight;
            float peak=*std::max_element(image.emissionColor.begin(),image.emissionColor.end());
            for(float &c:image.emissionColor)c=std::max(0.04f,c/std::max(peak,0.001f));
            image.emissionCoverage=(float)weight/(w*h);
            image.emissionWeight=std::clamp(image.emissionCoverage*8.0f,0.25f,1.0f);
        }
    }
    int channels=wide?4:2,levels=1;
    const byte *levelData=wide?packed.data():pixels;
    std::vector<byte> chain;
    if(mipmaps) {
        std::vector<byte> mips=paletteMips(w,h,levelData,channels,levels);
        chain.assign(levelData,levelData+(size_t)w*h*channels);chain.insert(chain.end(),mips.begin(),mips.end());
        levelData=chain.data();
    }
    image.texture=gpuCreateTexture(wide?GpuFormat::RGBA8:GpuFormat::RG8,w,h,levelData,w*channels,levels);
    if(!image.texture) I_Error((char*)"Could not allocate a GPU texture");
    return image;
}
// Screen glass heights, or empty when the texture has no screens; frame
// receives where each glass pixel sits on its screen (doom_screen_glass).
std::vector<byte> screenGlass(const char *name,bool flat,int w,int h,const byte *pixels,std::vector<byte> &frame) {
    frame.clear();
    if(!doom_screen_texture(name,flat))return {};
    const byte *palette=(const byte*)W_CacheLumpName((char*)"PLAYPAL",PU_CACHE);
    std::vector<byte> glass((size_t)w*h);frame.assign((size_t)w*h*4,0);
    if(!doom_screen_glass(w,h,pixels,palette,glass.data(),frame.data())) {frame.clear();return {};}
    return glass;
}
// Uploads the screen frame beside the image the shader refracts through it.
Image &withGlassFrame(Image &image,const std::vector<byte> &frame) {
    if(frame.empty())return image;
    image.glassFrame=gpuCreateTexture(GpuFormat::RGBA8,image.width,image.height,frame.data(),image.width*4);
    if(!image.glassFrame)I_Error((char*)"Could not allocate a GPU texture");
    return image;
}
// Masks live beside graphics.cfg, or in the directory selected by -emissive.
std::vector<byte> emissionMask(const char *name,bool flat,int w,int h,const byte *pixels) {
    std::vector<byte> mask((size_t)w*h,0);
    int paletteLump=W_GetNumForName((char*)"PLAYPAL");
    if(W_LumpLength(paletteLump)<768) I_Error((char*)"Invalid emissive palette");
    const byte *palette=(const byte*)W_CacheLumpNum(paletteLump,PU_CACHE);
    int kind=doom_emissive_kind(name,flat);
    for(size_t i=0;i<mask.size();++i)
        mask[i]=doom_emissive_pixel(kind,palette+pixels[2*i]*3,pixels[2*i+1]);
    std::string root=graphicsConfig;
    size_t slash=root.find_last_of("/\\");
    root=slash==std::string::npos?"Emissive":root.substr(0,slash+1)+"Emissive";
    int option=M_CheckParm((char*)"-emissive");
    if(option && option+1<myargc) root=myargv[option+1];
    while(root.size()>1&&(root.back()=='/'||root.back()=='\\'))root.pop_back();
    std::string file=root+(flat?"/flats/":"/walls/")+name+".png";
    if(FILE *probe=fopen(file.c_str(),"rb")) {
        fclose(probe);
        std::vector<float> values;
        if(!platformReadMask(file.c_str(),w,h,values)) {
            fprintf(stderr,"Emissive mask must be %dx%d: %s\n",w,h,file.c_str());
        } else {
            for(size_t i=0;i<mask.size();++i)
                mask[i]=pixels[i*2+1]?(byte)std::clamp(values[i]*255.0f,0.0f,255.0f):0;
            fprintf(stderr,"Loaded emissive mask: %s\n",file.c_str());
        }
    }
    return mask;
}
/* Texture keys: wall index >=0, WAD lump = -1-lump. */
Image &wallImage(int index) {
    auto found=images.find(index); if(found!=images.end()) return found->second;
    int w,h; R_TextureDimensions(index,&w,&h);
    if(w<1||h<1||w>8192||h>8192) I_Error((char*)"Unsupported texture dimensions");
    std::vector<byte> pixels((size_t)w*h*2); R_CopyIndexedTexture(index,pixels.data());
    char name[9]; R_TextureName(index,name);
    auto mask=emissionMask(name,false,w,h,pixels.data());
    bool glowing=std::any_of(mask.begin(),mask.end(),[](byte p){return p!=0;});
    std::vector<byte> frame;auto glass=screenGlass(name,false,w,h,pixels.data(),frame);
    return withGlassFrame(images.emplace(index,upload(w,h,pixels.data(),0,0,glowing?mask.data():nullptr,nullptr,
        glass.empty()?nullptr:glass.data(),true)).first->second,frame);
}
Image &lumpImage(int lump,bool flat) {
    int key=-1-lump; auto found=images.find(key); if(found!=images.end()) return found->second;
    int size=W_LumpLength(lump);
    const byte *data=(const byte*)W_CacheLumpNum(lump,PU_CACHE);
    if(flat) {
        if(size<4096) I_Error((char*)"Invalid floor texture");
        std::vector<byte> pixels(64*64*2);
        for(int i=0;i<4096;++i) {pixels[2*i]=data[i];pixels[2*i+1]=255;}
        char name[9];memcpy(name,lumpinfo[lump].name,8);name[8]=0;
        auto mask=emissionMask(name,true,64,64,pixels.data());
        bool glowing=std::any_of(mask.begin(),mask.end(),[](byte p){return p!=0;});
        std::vector<byte> frame;auto glass=screenGlass(name,true,64,64,pixels.data(),frame);
        return withGlassFrame(images.emplace(key,upload(64,64,pixels.data(),0,0,glowing?mask.data():nullptr,nullptr,
            glass.empty()?nullptr:glass.data(),true)).first->second,frame);
    }
    if(size<8) I_Error((char*)"Invalid sprite texture");
    const patch_t *patch=(const patch_t*)data;
    int w=SHORT(patch->width),h=SHORT(patch->height);
    if(w<1||h<1||w>8192||h>8192||8+(size_t)w*4>(size_t)size)
        I_Error((char*)"Invalid sprite dimensions");
    std::vector<byte> pixels((size_t)w*h*2);
    for(int x=0;x<w;++x) {
        size_t pos=(uint32_t)LONG(patch->columnofs[x]); int previous=-1;
        while(pos<(size_t)size && data[pos]!=255) {
            if(pos+4>(size_t)size||pos+4+data[pos+1]>(size_t)size) I_Error((char*)"Invalid sprite column");
            int top=data[pos]; if(top<=previous) top+=previous; previous=top;
            for(int y=0;y<data[pos+1];++y) if(top+y>=0&&top+y<h) {
                size_t pixel=((size_t)(top+y)*w+x)*2;
                pixels[pixel]=data[pos+3+y]; pixels[pixel+1]=255;
            }
            pos+=data[pos+1]+4;
        }
    }
    auto normals=spriteNormals(w,h,pixels.data());
    Image image=upload(w,h,pixels.data(),SHORT(patch->leftoffset),SHORT(patch->topoffset),nullptr,normals.data());
    int paletteLump=W_GetNumForName((char*)"PLAYPAL");
    if(W_LumpLength(paletteLump)<768) I_Error((char*)"Invalid lighting palette");
    const byte *palette=(const byte*)W_CacheLumpNum(paletteLump,PU_CACHE);
    image.glowWeight=doom_sprite_glow(pixels.data(),(size_t)w*h,palette,image.glow.data());
    return images.emplace(key,std::move(image)).first->second;
}
std::vector<Point> clip(const std::vector<Point> &poly,Point origin,Point direction,bool right) {
    std::vector<Point> out; if(poly.empty()) return out;
    auto distance=[&](Point p) {double d=(double)direction.x*(p.y-origin.y)-(double)direction.y*(p.x-origin.x);return right?-d:d;};
    Point a=poly.back(); double da=distance(a);
    for(Point b:poly) {
        double db=distance(b);
        if((da>=-0.01)!=(db>=-0.01)) {
            double t=da/(da-db);
            out.push_back({(float)(a.x+(b.x-a.x)*t),(float)(a.y+(b.y-a.y)*t)});
        }
        if(db>=-0.01) out.push_back(b);
        a=b; da=db;
    }
    return out;
}
void partition(unsigned node,const std::vector<Point> &poly,int depth=0) {
    if(poly.size()<3) return;
    if(depth>numnodes+1) I_Error((char*)"Invalid BSP tree");
    if(node&NF_SUBSECTOR) {
        unsigned leaf=node&~NF_SUBSECTOR; if(leaf>=(unsigned)numsubsectors) I_Error((char*)"Invalid BSP leaf");
        auto shape=poly; const subsector_t &sub=subsectors[leaf];
        for(int i=0;i<sub.numlines;++i) {
            const seg_t &seg=segs[sub.firstline+i];
            Point a={units(seg.v1->x),units(seg.v1->y)},b={units(seg.v2->x),units(seg.v2->y)};
            shape=clip(shape,a,{b.x-a.x,b.y-a.y},true);
        }
        floors[leaf]=std::move(shape); return;
    }
    if(node>=(unsigned)numnodes) I_Error((char*)"Invalid BSP node");
    const node_t &n=nodes[node]; Point origin={units(n.x),units(n.y)},direction={units(n.dx),units(n.dy)};
    partition(n.children[0],clip(poly,origin,direction,true),depth+1);
    partition(n.children[1],clip(poly,origin,direction,false),depth+1);
}
// Liquids are the liquid-named flats the engine animates: Doom 2's SLIME13-16
// share the name but are still metal floors (MAP04's courtyard).
bool liquidFlat(int pic) {
    const char *name=lumpinfo[firstflat+pic].name;int next;
    return (!strncmp(name,"NUKAGE",6)||!strncmp(name,"FWATER",6)||!strncmp(name,"SLIME",5)||
            !strncmp(name,"BLOOD",5)||!strncmp(name,"LAVA",4))&&P_PicAnimationNext(false,pic,&next)>0;
}
void classifySectorMist() {
    sectorMist.assign(numsectors,{});
    std::vector<float> heights;
    const byte *palette=(const byte*)W_CacheLumpName((char*)"PLAYPAL",PU_CACHE);
    for(int i=0;i<numsectors;++i) {
        const sector_t &sector=sectors[i];auto &mist=sectorMist[i];
        heights.push_back(units(sector.floorheight));
        mist.outdoor=sector.ceilingpic==skyflatnum;
        float lowestNeighbor=1e9;bool hasNeighbor=false;
        for(int j=0;j<sector.linecount;++j) {
            const line_t &line=*sector.lines[j];
            const sector_t *neighbor=line.frontsector==&sector?line.backsector:line.frontsector;
            if(!neighbor||neighbor==&sector)continue;
            hasNeighbor=true;lowestNeighbor=std::min(lowestNeighbor,units(neighbor->floorheight));
        }
        mist.pit=hasNeighbor&&lowestNeighbor-units(sector.floorheight)>=24;
        int lump=firstflat+sector.floorpic;
        mist.liquid=liquidFlat(sector.floorpic);
        mist.reflective=mist.liquid&&strncmp(lumpinfo[lump].name,"LAVA",4)!=0;
        mist.toxic=sector.special==4||sector.special==5||sector.special==7||sector.special==11||sector.special==16;
        if(mist.liquid&&W_LumpLength(lump)>=4096) {
            const byte *flat=(const byte*)W_CacheLumpNum(lump,PU_CACHE);
            mist.color={};
            for(int pixel=0;pixel<4096;++pixel)for(int c=0;c<3;++c)
                mist.color[c]+=palette[flat[pixel]*3+c]/(4096.0f*255.0f);
        } else if(mist.toxic) mist.color={0.18f,0.30f,0.12f};
        bool lava=!strncmp(lumpinfo[lump].name,"LAVA",4);
        if((lava||mist.toxic)&&W_LumpLength(lump)>=4096) {
            const byte *flat=(const byte*)W_CacheLumpNum(lump,PU_CACHE);
            float rgb[3]={};
            for(int pixel=0;pixel<4096;++pixel)for(int c=0;c<3;++c)rgb[c]+=palette[flat[pixel]*3+c];
            mist.hot=lava||(rgb[0]>rgb[1]*1.25f&&rgb[0]>rgb[2]*1.4f);
        }
    }
    if(!heights.empty()) {
        std::nth_element(heights.begin(),heights.begin()+heights.size()/2,heights.end());
        fogReferenceHeight=heights[heights.size()/2];
    }
    size_t count=std::count_if(sectorMist.begin(),sectorMist.end(),[](const auto &m){return m.pit||m.liquid||m.toxic;});
    fprintf(stderr,"3D mist: %zu candidate sectors; derived from floor heights, flats and specials.\n",count);
}
void buildSurfaceMaps(float minx,float miny,float maxx,float maxy) {
    minx-=64;miny-=64;maxx+=64;maxy+=64;
    mapCell=std::max(4.0f,std::ceil(std::max(maxx-minx,maxy-miny)/1024.0f));
    mapOrigin[0]=minx;mapOrigin[1]=miny;
    mapWidth=(int)std::ceil((maxx-minx)/mapCell);mapHeight=(int)std::ceil((maxy-miny)/mapCell);
    size_t count=(size_t)mapWidth*mapHeight;
    seamCells.assign(count,{});
    std::vector<float> floorDistance(count,contactRange),ceilingDistance(count,contactRange),seamDistance(count,seamBand);
    auto center=[&](int x,int y){return Point{mapOrigin[0]+(x+0.5f)*mapCell,mapOrigin[1]+(y+0.5f)*mapCell};};
    // Cell sectors come from the convex BSP floor polygons; the rest is void.
    for(int leaf=0;leaf<numsubsectors;++leaf) {
        const auto &poly=floors[leaf];if(poly.size()<3)continue;
        const auto &b=floorBounds[leaf];
        int x0=std::max(0,(int)std::floor((b[0]-minx)/mapCell)),x1=std::min(mapWidth-1,(int)std::floor((b[2]-minx)/mapCell));
        int y0=std::max(0,(int)std::floor((b[1]-miny)/mapCell)),y1=std::min(mapHeight-1,(int)std::floor((b[3]-miny)/mapCell));
        uint16_t sector=(uint16_t)(subsectors[leaf].sector-sectors);
        for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x) {
            Point p=center(x,y);bool positive=false,negative=false;
            for(size_t n=0;n<poly.size();++n) {
                Point a=poly[n],c=poly[(n+1)%poly.size()];
                float cross=(c.x-a.x)*(p.y-a.y)-(c.y-a.y)*(p.x-a.x);
                positive|=cross>0.001f;negative|=cross<-0.001f;
            }
            if(!(positive&&negative))seamCells[(size_t)y*mapWidth+x].own=sector;
        }
    }
    for(int i=0;i<numlines;++i) {
        const line_t &line=lines[i];
        float ax=units(line.v1->x),ay=units(line.v1->y),bx=units(line.v2->x),by=units(line.v2->y);
        float ex=bx-ax,ey=by-ay,length2=ex*ex+ey*ey;if(length2<0.01f)continue;
        const sector_t *front=line.frontsector,*back=line.backsector;
        int f=front?(int)(front-sectors):-1,b=back?(int)(back-sectors):-1;
        if(f==b)continue;
        float ff=front?units(front->floorheight):0,fc=front?units(front->ceilingheight):0;
        float bf=back?units(back->floorheight):0,bc=back?units(back->ceilingheight):0;
        // Lines only blend light between walkable neighbors; tall steps stay crisp.
        bool seam=front&&back&&std::abs(ff-bf)<=24;
        int x0=std::max(0,(int)std::floor((std::min(ax,bx)-contactRange-minx)/mapCell));
        int x1=std::min(mapWidth-1,(int)std::floor((std::max(ax,bx)+contactRange-minx)/mapCell));
        int y0=std::max(0,(int)std::floor((std::min(ay,by)-contactRange-miny)/mapCell));
        int y1=std::min(mapHeight-1,(int)std::floor((std::max(ay,by)+contactRange-miny)/mapCell));
        for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x) {
            size_t index=(size_t)y*mapWidth+x;auto &cell=seamCells[index];
            if(cell.own==noSector)continue;
            Point p=center(x,y);
            float t=std::clamp(((p.x-ax)*ex+(p.y-ay)*ey)/length2,0.0f,1.0f);
            float d=std::hypot(p.x-ax-ex*t,p.y-ay-ey*t);
            if(d>=contactRange)continue;
            int s=cell.own;bool isFront=s==f,isBack=s==b;
            bool floorWall=!back||(isFront&&bf>=ff+24)||(isBack&&ff>=bf+24);
            bool ceilingWall=!back||(isFront&&bc<=fc-24)||(isBack&&fc<=bc-24);
            if(floorWall)floorDistance[index]=std::min(floorDistance[index],d);
            if(ceilingWall)ceilingDistance[index]=std::min(ceilingDistance[index],d);
            if(seam&&(isFront||isBack)&&d<seamDistance[index]) {
                seamDistance[index]=d;cell.neighbor=(uint16_t)(isFront?b:f);
                cell.weight=(uint16_t)std::lround(0.5f*(1-d/seamBand)*65535);
            }
        }
    }
    contactCells.assign(count*4,0);
    for(size_t i=0;i<count;++i) {
        bool solid=seamCells[i].own==noSector;
        contactCells[4*i]=solid?0:(uint8_t)std::lround(floorDistance[i]/contactRange*255);
        contactCells[4*i+1]=solid?0:(uint8_t)std::lround(ceilingDistance[i]/contactRange*255);
    }
    seamTexture=gpuCreateTexture(GpuFormat::RGBA16Uint,mapWidth,mapHeight,seamCells.data(),mapWidth*8);
    contactTexture=gpuCreateTexture(GpuFormat::RGBA8,mapWidth,mapHeight,contactCells.data(),mapWidth*4);
    if(!seamTexture||!contactTexture)I_Error((char*)"Could not allocate surface maps");
    fprintf(stderr,"3D surface maps: %dx%d cells of %.0f units.\n",mapWidth,mapHeight,mapCell);
}
// Caustics on walls near liquids: map cells hold the liquid's sector (r, and
// g's low 7 bits), whether it glows by itself (g's top bit: nukage, slime,
// lava), its tint at full brightness in 3-3-2 bits (b) and a proximity (a),
// so the shader can follow the light on the water as it changes. It spreads through open cells whose
// floors sit near the liquid surface, so ledges and closed doors stop it;
// void cells take their strongest neighbor for the walls on their boundary. The pattern
// is every animation frame of the level's most common liquid flat, stacked,
// keeping how far each texel (softened over its 3x3 neighbors, so lone bright
// texels drop out) rises above the flat's mean brightness, so the ripples are
// the WAD's own wave streaks and animate in step with the floor.
// Each frame has causticLayers rows of 64x64: 0 that plain pattern; 1-4 real
// caustics at causticHeights above the water, from reading the softened
// brightness as wave height and bouncing overhead light off it (8x8 rays per
// texel, splatted where they land; denser landing is brighter); 5-6 the wave
// slope in x and y around 128. The wave strength comes from the flat's own
// curvature (its 70th percentile) so the lines sharpen causticFocus units up;
// stored is how far the landing light exceeds the even spread, full at 4x.
constexpr float causticRange=96,causticFocus=40;
constexpr int causticLayers=7;
constexpr float causticHeights[4]={8,24,48,96};
std::vector<int> causticFrames;int causticSpeed=0;
void buildCaustics() {
    causticTexture=nullptr;causticPattern=nullptr;causticFrames.clear();causticSpeed=0;
    uint64_t start=SDL_GetTicksNS();
    size_t count=(size_t)mapWidth*mapHeight;
    std::vector<float> distance(count,causticRange),height(count,0);
    std::vector<int> source(count,-1);
    using Entry=std::pair<float,size_t>;
    std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
    for(size_t i=0;i<count;++i) {
        int s=seamCells[i].own;
        if(s==noSector||!sectorMist[s].liquid||sectors[s].floorpic==skyflatnum)continue;
        distance[i]=0;height[i]=units(sectors[s].floorheight);source[i]=s;queue.push({0.0f,i});
    }
    if(queue.empty())return;
    size_t liquidCells=queue.size();
    while(!queue.empty()) {
        auto [d,i]=queue.top();queue.pop();
        if(d>distance[i])continue;
        int x=int(i%mapWidth),y=int(i/mapWidth);
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx) {
            int nx=x+dx,ny=y+dy;
            if((!dx&&!dy)||nx<0||ny<0||nx>=mapWidth||ny>=mapHeight)continue;
            size_t n=(size_t)ny*mapWidth+nx;int s=seamCells[n].own;
            if(s==noSector)continue;
            float floor=units(sectors[s].floorheight);
            if(floor>height[i]+32||floor<height[i]-64||units(sectors[s].ceilingheight)-floor<8)continue;
            float next=d+mapCell*(dx&&dy?1.4142f:1.0f);
            if(next>=distance[n])continue;
            distance[n]=next;height[n]=height[i];source[n]=source[i];queue.push({next,n});
        }
    }
    std::vector<uint8_t> cells(count*4,0);
    for(size_t i=0;i<count;++i) {
        if(source[i]<0||source[i]>=32768)continue;
        const auto &mist=sectorMist[source[i]];const auto &color=mist.color;
        float peak=std::max({color[0],color[1],color[2],0.01f}),proximity=1-distance[i]/causticRange;
        auto level=[&](int c,int steps){return (int)std::lround(std::clamp(color[c]/peak,0.0f,1.0f)*steps);};
        cells[4*i]=(uint8_t)(source[i]&255);
        cells[4*i+1]=(uint8_t)((source[i]>>8)|(mist.toxic||mist.hot?128:0));
        cells[4*i+2]=(uint8_t)(level(0,7)<<5|level(1,7)<<2|level(2,3));
        cells[4*i+3]=(uint8_t)std::lround(proximity*proximity*255);
    }
    std::vector<uint8_t> dilated=cells;
    for(int y=0;y<mapHeight;++y)for(int x=0;x<mapWidth;++x) {
        size_t i=(size_t)y*mapWidth+x;if(seamCells[i].own!=noSector)continue;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx) {
            int nx=x+dx,ny=y+dy;if(nx<0||ny<0||nx>=mapWidth||ny>=mapHeight)continue;
            size_t n=(size_t)ny*mapWidth+nx;
            if(seamCells[n].own!=noSector&&cells[4*n+3]>dilated[4*i+3])std::copy_n(&cells[4*n],4,&dilated[4*i]);
        }
    }
    std::map<int,int> flatUse;
    for(int i=0;i<numsectors;++i)if(sectorMist[i].liquid)++flatUse[sectors[i].floorpic];
    int pic=std::max_element(flatUse.begin(),flatUse.end(),[](const auto &a,const auto &b){return a.second<b.second;})->first,lump=firstflat+pic;
    causticFrames={pic};int next=pic;causticSpeed=P_PicAnimationNext(false,pic,&next);
    while(causticSpeed>0&&next!=pic&&causticFrames.size()<32) {causticFrames.push_back(next);P_PicAnimationNext(false,next,&next);}
    size_t frames=causticFrames.size();
    std::vector<byte> pattern;
    std::vector<float> luma(4096*frames,0);float mean=0,peak=0;
    const byte *palette=(const byte*)W_CacheLumpName((char*)"PLAYPAL",PU_CACHE);
    for(size_t f=0;f<frames;++f) {
        int frameLump=firstflat+causticFrames[f];
        if(W_LumpLength(frameLump)<4096)continue;
        const byte *pixels=(const byte*)W_CacheLumpNum(frameLump,PU_CACHE);
        for(int i=0;i<4096;++i) {
            const byte *c=palette+pixels[i]*3;
            luma[f*4096+i]=0.299f*c[0]+0.587f*c[1]+0.114f*c[2];
        }
        std::array<float,4096> soft;
        for(int y=0;y<64;++y)for(int x=0;x<64;++x) {
            float sum=0;
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)sum+=luma[f*4096+((y+dy)&63)*64+((x+dx)&63)]*(dx||dy?1.0f:2.0f);
            soft[y*64+x]=sum/10;mean+=sum/10;peak=std::max(peak,sum/10);
        }
        std::copy(soft.begin(),soft.end(),luma.begin()+f*4096);
    }
    mean/=4096.0f*frames;
    std::vector<float> wave(luma.size()),slope(luma.size()*2),curvature;
    for(size_t i=0;i<luma.size();++i)wave[i]=(luma[i]-mean)/std::max(peak-mean,0.001f);
    float steepest=0.001f;
    for(size_t f=0;f<frames;++f) {
        auto h=[&](int x,int y){return wave[f*4096+(y&63)*64+(x&63)];};
        for(int y=0;y<64;++y)for(int x=0;x<64;++x) {
            size_t i=f*4096+y*64+x;
            slope[2*i]=(h(x+1,y)-h(x-1,y))/2;slope[2*i+1]=(h(x,y+1)-h(x,y-1))/2;
            steepest=std::max({steepest,std::abs(slope[2*i]),std::abs(slope[2*i+1])});
            float xx=h(x+1,y)-2*h(x,y)+h(x-1,y),yy=h(x,y+1)-2*h(x,y)+h(x,y-1);
            float xy=(h(x+1,y+1)-h(x-1,y+1)-h(x+1,y-1)+h(x-1,y-1))/4;
            curvature.push_back(std::abs((xx+yy)/2)+std::sqrt((xx-yy)*(xx-yy)/4+xy*xy));
        }
    }
    std::nth_element(curvature.begin(),curvature.begin()+curvature.size()*70/100,curvature.end());
    float bend=1/(causticFocus*std::max(curvature[curvature.size()*70/100],0.001f));
    pattern.assign(4096*frames*causticLayers,0);
    auto row=[&](int layer,size_t f){return pattern.data()+(layer*frames+f)*4096;};
    for(size_t f=0;f<frames;++f) {
        byte *plain=row(0,f);
        for(int i=0;i<4096;++i) {
            plain[i]=(byte)std::lround(std::clamp(wave[f*4096+i],0.0f,1.0f)*255);
            for(int axis=0;axis<2;++axis)
                row(5+axis,f)[i]=(byte)std::lround(128+127*std::clamp(slope[2*(f*4096+i)+axis]/steepest,-1.0f,1.0f));
        }
        // Bilinear slope between texel centers, so landing spots move smoothly.
        auto slopeAt=[&](float x,float y,int axis) {
            x-=0.5f;y-=0.5f;int x0=(int)std::floor(x),y0=(int)std::floor(y);float fx=x-x0,fy=y-y0;
            auto at=[&](int xx,int yy){return slope[2*(f*4096+(yy&63)*64+(xx&63))+axis];};
            return (at(x0,y0)*(1-fx)+at(x0+1,y0)*fx)*(1-fy)+(at(x0,y0+1)*(1-fx)+at(x0+1,y0+1)*fx)*fy;
        };
        for(int layer=0;layer<4;++layer) {
            float reach=bend*causticHeights[layer];
            std::array<float,4096> density={};
            for(int y=0;y<64;++y)for(int x=0;x<64;++x)for(int sy=0;sy<8;++sy)for(int sx=0;sx<8;++sx) {
                float px=x+(sx+0.5f)/8,py=y+(sy+0.5f)/8;
                float qx=px+reach*slopeAt(px,py,0)-0.5f,qy=py+reach*slopeAt(px,py,1)-0.5f;
                int x0=(int)std::floor(qx),y0=(int)std::floor(qy);float fx=qx-x0,fy=qy-y0;
                density[(y0&63)*64+(x0&63)]+=(1-fx)*(1-fy)/64;density[(y0&63)*64+((x0+1)&63)]+=fx*(1-fy)/64;
                density[((y0+1)&63)*64+(x0&63)]+=(1-fx)*fy/64;density[((y0+1)&63)*64+((x0+1)&63)]+=fx*fy/64;
            }
            byte *out=row(1+layer,f);
            for(int i=0;i<4096;++i)out[i]=(byte)std::lround(std::clamp((density[i]-1)/3,0.0f,1.0f)*255);
        }
    }
    causticTexture=gpuCreateTexture(GpuFormat::RGBA8,mapWidth,mapHeight,dilated.data(),mapWidth*4);
    causticPattern=gpuCreateTexture(GpuFormat::R8,64,64*(int)(frames*causticLayers),pattern.data(),64);
    if(!causticTexture||!causticPattern)I_Error((char*)"Could not allocate caustic maps");
    fprintf(stderr,"3D caustics: %zu liquid cells; pattern from %.8s, %zu frames, computed in %.0f ms.\n",liquidCells,lumpinfo[lump].name,frames,(SDL_GetTicksNS()-start)/1e6);
}
// Shorelines: where a liquid meets a wall, a closed door, a higher floor or
// dry ground at its own height (not another liquid, not a drop). Map cells
// hold the signed distance to the nearest such edge, positive on the
// liquid's side, as (d+shoreRange)/(2*shoreRange) in r; the shader samples it
// linearly, which is exact along straight edges, so damp banks keep their
// width however coarse the cells are. g and b's low 7 bits hold that edge's
// liquid sector, b's top bit marks cells with an edge in range, a its tint in
// 3-3-2 bits like causticMap. Lava and other hot liquids leave no damp.
constexpr float shoreRange=16;
void buildShore() {
    shoreTexture=nullptr;
    size_t count=(size_t)mapWidth*mapHeight;
    std::vector<float> best(count,1e9f);
    std::vector<int> liquidOf(count,-1);
    size_t edges=0;
    float reach=shoreRange+2*mapCell;
    for(int i=0;i<numlines;++i) {
        const line_t &line=lines[i];
        float ax=units(line.v1->x),ay=units(line.v1->y),bx=units(line.v2->x),by=units(line.v2->y);
        float ex=bx-ax,ey=by-ay,length2=ex*ex+ey*ey;if(length2<0.01f)continue;
        for(int side=0;side<2;++side) {
            const sector_t *wet=side?line.backsector:line.frontsector,*dry=side?line.frontsector:line.backsector;
            if(!wet||wet==dry)continue;
            int s=(int)(wet-sectors);const auto &mist=sectorMist[s];
            if(!mist.liquid||mist.hot||wet->floorpic==skyflatnum||s>=32768)continue;
            float water=units(wet->floorheight);
            if(dry) {
                float floor=units(dry->floorheight),ceiling=units(dry->ceilingheight);
                bool closed=ceiling-floor<8||ceiling<=water+8;
                bool flush=std::abs(floor-water)<=0.5f;
                if(!closed&&(floor<water-0.5f||(flush&&sectorMist[dry-sectors].liquid)))continue;
            }
            ++edges;
            // Doom's front side is on the line's right: negative cross product.
            float wetSign=side?1.0f:-1.0f;
            int x0=std::max(0,(int)std::floor((std::min(ax,bx)-reach-mapOrigin[0])/mapCell));
            int x1=std::min(mapWidth-1,(int)std::floor((std::max(ax,bx)+reach-mapOrigin[0])/mapCell));
            int y0=std::max(0,(int)std::floor((std::min(ay,by)-reach-mapOrigin[1])/mapCell));
            int y1=std::min(mapHeight-1,(int)std::floor((std::max(ay,by)+reach-mapOrigin[1])/mapCell));
            for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x) {
                float px=mapOrigin[0]+(x+0.5f)*mapCell,py=mapOrigin[1]+(y+0.5f)*mapCell;
                float t=std::clamp(((px-ax)*ex+(py-ay)*ey)/length2,0.0f,1.0f);
                float d=std::hypot(px-ax-ex*t,py-ay-ey*t);
                size_t index=(size_t)y*mapWidth+x;
                if(d>=reach||d>=std::abs(best[index]))continue;
                float cross=ex*(py-ay)-ey*(px-ax);
                best[index]=cross*wetSign>=0?d:-d;liquidOf[index]=s;
            }
        }
    }
    if(!edges)return;
    std::vector<uint8_t> cells(count*4,0);
    for(size_t i=0;i<count;++i) {
        cells[4*i]=255;
        int s=liquidOf[i];if(s<0)continue;
        const auto &color=sectorMist[s].color;
        float peak=std::max({color[0],color[1],color[2],0.01f});
        auto level=[&](int c,int steps){return (int)std::lround(std::clamp(color[c]/peak,0.0f,1.0f)*steps);};
        cells[4*i]=(uint8_t)std::lround(std::clamp((best[i]+shoreRange)/(2*shoreRange),0.0f,1.0f)*255);
        cells[4*i+1]=(uint8_t)(s&255);
        cells[4*i+2]=(uint8_t)((s>>8)|128);
        cells[4*i+3]=(uint8_t)(level(0,7)<<5|level(1,7)<<2|level(2,3));
    }
    shoreTexture=gpuCreateTexture(GpuFormat::RGBA8,mapWidth,mapHeight,cells.data(),mapWidth*4);
    if(!shoreTexture)I_Error((char*)"Could not allocate the shore map");
    fprintf(stderr,"3D shorelines: %zu liquid edges.\n",edges);
}
void layoutBake();
void findDoors();
void buildMap() {
    finishRelight(false);
    batches.clear();shadowBatches.clear();
    surfaceSelection.clear();flatLightSamples.clear();surfaceLightTime=0;
    floors.assign(numsubsectors,{});
    float minx=1e9,miny=1e9,maxx=-1e9,maxy=-1e9;
    for(int i=0;i<numvertexes;++i) {
        float x=units(vertexes[i].x),y=units(vertexes[i].y);
        minx=std::min(minx,x);miny=std::min(miny,y);maxx=std::max(maxx,x);maxy=std::max(maxy,y);
    }
    std::vector<Point> bounds={{minx-128,miny-128},{maxx+128,miny-128},{maxx+128,maxy+128},{minx-128,maxy+128}};
    partition(numnodes?unsigned(numnodes-1):unsigned(NF_SUBSECTOR),bounds);
    sectorFloors.assign(numsectors,{});
    for(int i=0;i<numsubsectors;++i)
        sectorFloors[subsectors[i].sector-sectors].push_back(i);
    floorBounds.assign(numsubsectors,{1e9f,1e9f,-1e9f,-1e9f});
    for(int i=0;i<numsubsectors;++i) for(Point p:floors[i]) {
        auto &b=floorBounds[i];b[0]=std::min(b[0],p.x);b[1]=std::min(b[1],p.y);
        b[2]=std::max(b[2],p.x);b[3]=std::max(b[3],p.y);
    }
    classifySectorMist();
    buildSurfaceMaps(minx,miny,maxx,maxy);
    buildCaustics();
    buildShore();
    sunBaked=false;lightBakeKey=-1;bounceKey=-1;ambientKey=-1;flowKey=-1;bakeThingsKey=-1;wallBakeTexture=nullptr;flatLightTexture=nullptr;
    skyOpeningOf.clear();skyOpenings.clear();sunShafts.clear();
    bakeSources.clear();bakeGrid.clear();bakeAreas.clear();sourceGroups.clear();staticGrid.clear();
    wallDirectionCells.clear();flatDirectionCells.clear();wallDirectionTexture=nullptr;flatDirectionTexture=nullptr;
    layoutBake();relightPending.clear();
    findDoors();
    oldThings.clear(); oldHeights.clear(); tickTime=0;
    levelSerial=r_levelserial;
    size_t triangles=0; for(auto &p:floors) if(p.size()>2) triangles+=p.size()-2;
    fprintf(stderr,"3D map: %d subsectors, %zu floor triangles, %d lines.\n",numsubsectors,triangles,numlines);
}
float sectorHeight(const sector_t *sector,bool ceiling) {
    float now=units(ceiling?sector->ceilingheight:sector->floorheight);
    size_t index=sector-sectors;
    if(index>=oldHeights.size())return now;
    float old=oldHeights[index][ceiling];
    return old+(now-old)*renderFraction;
}
float floorZ(const sector_t *sector) {return sectorHeight(sector,false);}
float ceilZ(const sector_t *sector) {return sectorHeight(sector,true);}
float lighting(const sector_t *sector,bool bright=false) {
    if(bright) return 1;
    return std::clamp(sector->lightlevel/255.0f,0.12f,1.0f);
}
// Shared by the sun and light bakes, built once per level: the sector/line
// map the rays walk, and a texel for every wall at the floor cell size.
// Sector heights start as the level's; relightMovingSectors keeps them
// following doors and lifts.
void layoutBake() {
    bakeMap={};bakeMap.sectors.resize(numsectors);
    for(int i=0;i<numsectors;++i)
        bakeMap.sectors[i]={units(sectors[i].floorheight),units(sectors[i].ceilingheight),sectors[i].ceilingpic==skyflatnum,{}};
    for(int i=0;i<numlines;++i) {
        const line_t &line=lines[i];
        int front=line.frontsector?(int)(line.frontsector-sectors):-1,back=line.backsector?(int)(line.backsector-sectors):-1;
        bakeMap.lines.push_back({units(line.v1->x),units(line.v1->y),units(line.v2->x),units(line.v2->y),front,back});
        if(front>=0)bakeMap.sectors[front].lines.push_back(i);
        if(back>=0&&back!=front)bakeMap.sectors[back].lines.push_back(i);
    }
    // Strip heights cover what doors, lifts and floors can reach: each
    // sector's own and its neighbors' floors and ceilings.
    std::vector<float> reachLow(numsectors),reachHigh(numsectors);
    for(int i=0;i<numsectors;++i) {reachLow[i]=bakeMap.sectors[i].floor;reachHigh[i]=bakeMap.sectors[i].ceiling;}
    for(const auto &line:bakeMap.lines) if(line.front>=0&&line.back>=0) for(int k=0;k<2;++k) {
        int a=k?line.back:line.front,b=k?line.front:line.back;
        reachLow[a]=std::min(reachLow[a],bakeMap.sectors[b].floor);reachHigh[a]=std::max(reachHigh[a],bakeMap.sectors[b].ceiling);
    }
    bakeStrips.assign((size_t)numlines*2,{});bakeOrder.clear();
    for(int i=0;i<numlines;++i) for(int side=0;side<2;++side) {
        const line_t &line=lines[i];if(line.sidenum[side]<0)continue;
        BakeStrip &strip=bakeStrips[(size_t)i*2+side];
        strip.sector=(int)(sides[line.sidenum[side]].sector-sectors);
        const vertex_t *from=side?line.v2:line.v1,*to=side?line.v1:line.v2;
        strip.ax=units(from->x);strip.ay=units(from->y);
        float dx=units(to->x)-strip.ax,dy=units(to->y)-strip.ay;
        strip.length=std::hypot(dx,dy);if(strip.length<1)continue;
        strip.dx=dx/strip.length;strip.dy=dy/strip.length;
        const sector_t *other=side?line.frontsector:line.backsector;
        strip.low=reachLow[strip.sector];strip.high=reachHigh[strip.sector];
        if(other) {int o=(int)(other-sectors);strip.low=std::min(strip.low,reachLow[o]);strip.high=std::max(strip.high,reachHigh[o]);}
        if(strip.high>strip.low)bakeOrder.push_back(i*2+side);
    }
    std::sort(bakeOrder.begin(),bakeOrder.end(),[](int a,int b) {
        return bakeStrips[a].high-bakeStrips[a].low>bakeStrips[b].high-bakeStrips[b].low;
    });
    // Shelf packing at the floor cell size, coarser if the atlas would not fit.
    constexpr int atlasLimit=8192;
    for(float cell=mapCell;;cell*=2) {
        int x=0,y=0,shelf=0;bool fits=true;
        for(int index:bakeOrder) {
            BakeStrip &strip=bakeStrips[index];
            strip.scale=std::min(1/cell,(atlasWidth-2)/strip.length);
            strip.width=(int)std::ceil(strip.length*strip.scale)+1;
            strip.height=(int)std::ceil((strip.high-strip.low)*strip.scale)+1;
            if(x+strip.width>atlasWidth) {x=0;y+=shelf;shelf=0;}
            strip.x=x;strip.y=y;x+=strip.width;shelf=std::max(shelf,strip.height);
            if(y+shelf>atlasLimit) {fits=false;break;}
        }
        if(fits) {atlasHeight=std::max(1,y+shelf);break;}
    }
    wallBakeCells.assign((size_t)atlasStride*atlasHeight*4,0);
    flatLightCells.assign((size_t)mapWidth*mapHeight*4*4,0);
    // Middle textures on two-sided lines block rays where they are opaque,
    // placed as geometry() draws them in the level's initial state.
    for(int i=0;i<numlines;++i) {
        const line_t &line=lines[i];
        if(!line.frontsector||!line.backsector)continue;
        for(int side=0;side<2;++side) {
            if(line.sidenum[side]<0||!sides[line.sidenum[side]].midtexture)continue;
            const side_t &s=sides[line.sidenum[side]];
            const Image &image=wallImage(texturetranslation[s.midtexture]);
            float low=std::max(units(line.frontsector->floorheight),units(line.backsector->floorheight));
            float high=std::min(units(line.frontsector->ceilingheight),units(line.backsector->ceilingheight));
            float anchor=((line.flags&ML_DONTPEGBOTTOM)?low+image.height:high)+units(s.rowoffset);
            float length=std::hypot(bakeMap.lines[i].bx-bakeMap.lines[i].ax,bakeMap.lines[i].by-bakeMap.lines[i].ay);
            // Side 1 runs from v2, so its texture u decreases from v1.
            float u=units(s.textureoffset)+(side?length:0);
            bakeMap.lines[i].mask={image.pixels.data(),image.width,image.height,u,side?-1.0f:1.0f,anchor,
                                   std::max(low,anchor-image.height),std::min(high,anchor)};
            break;
        }
    }
}
// Runs work(n) for n in [0,count) on all cores (at most threads); work must
// touch only its own texels.
void parallelFor(int count,const std::function<void(int)> &work,unsigned threads=16) {
    std::atomic<int> next{0};
    auto run=[&] {for(int n;(n=next++)<count;)work(n);};
    std::vector<std::thread> workers;
    for(unsigned n=1;n<std::clamp(std::thread::hardware_concurrency(),1u,std::max(1u,threads));++n)workers.emplace_back(run);
    run();
    for(auto &worker:workers)worker.join();
}
// Calls texel(cell index, x, y, z) for each texel of a wall strip, at a
// point just in front of the side, in its own sector.
template<class Texel> void forStripTexels(const BakeStrip &strip,Texel texel) {
    for(int column=0;column<strip.width;++column)for(int row=0;row<strip.height;++row) {
        float along=std::clamp((column+0.5f)/strip.scale,0.5f,strip.length-0.5f);
        float z=std::min(strip.low+(row+0.5f)/strip.scale,strip.high);
        texel((size_t)(strip.y+row)*atlasStride+strip.x+column,
              strip.ax+strip.dx*along+strip.dy*0.5f,strip.ay+strip.dy*along-strip.dx*0.5f,z);
    }
}
void uploadWallBake() {
    wallBakeTexture=gpuCreateTexture(GpuFormat::RGBA8,atlasStride,atlasHeight,wallBakeCells.data(),atlasStride*4);
    if(!wallBakeTexture)I_Error((char*)"Could not allocate surface maps");
}
void uploadFlatLight() {
    flatLightTexture=gpuCreateTexture(GpuFormat::RGBA8,mapWidth,mapHeight*4,flatLightCells.data(),mapWidth*4);
    if(!flatLightTexture)I_Error((char*)"Could not allocate surface maps");
}
// Sun shadows, baked once per level into the contact map's blue channel and
// the wall atlas alpha. The sky texture's brightest columns face the sun
// (sky.frag maps column x to yaw -x*2pi/(4*width)) and also give its tint;
// the elevation is fixed. Rays see the sector heights in bakeMap.
float enclosure(const sector_t *sector,float *light=nullptr);
float hashUnit(int x,int y,uint32_t seed);
void bakeShafts();
// A floor cell's sun visibility (0-255), or -1 for void and sky floors.
int sunCell(const BakeMap &map,size_t i,const float direction[3]) {
    int sector=seamCells[i].own;
    if(sector==noSector||sectors[sector].floorpic==skyflatnum)return -1;
    float px=mapOrigin[0]+(i%mapWidth+0.5f)*mapCell,py=mapOrigin[1]+(i/mapWidth+0.5f)*mapCell;
    return (int)std::lround(sunVisibility(map,sector,px,py,map.sectors[sector].floor+1,direction)*255);
}
// A wall strip's sun visibility into the atlas alpha. Walls fade with the
// angle to the sun so grazing walls fall into shade.
void sunStrip(const BakeMap &map,const BakeStrip &strip,const float direction[3]) {
    float length=std::hypot(direction[0],direction[1]),sunX=direction[0]/length,sunY=direction[1]/length;
    float facing=std::clamp((strip.dy*sunX-strip.dx*sunY)*1.5f,0.0f,1.0f);
    forStripTexels(strip,[&](size_t i,float x,float y,float z) {
        float visible=facing>0?facing*sunVisibility(map,strip.sector,x,y,z,direction):0;
        wallBakeCells[4*i+3]=(uint8_t)std::lround(visible*255);
    });
}
void bakeSun() {
    finishRelight();
    sunBaked=true;sunLevel=0;
    if(contactCells.empty())return;
    std::vector<float> skyLights;
    for(int i=0;i<numsectors;++i)if(sectors[i].ceilingpic==skyflatnum)skyLights.push_back(lighting(&sectors[i]));
    if(skyLights.empty()) {fprintf(stderr,"3D sun: no sky sectors.\n");return;}
    std::nth_element(skyLights.begin(),skyLights.begin()+skyLights.size()/2,skyLights.end());
    float level=skyLights[skyLights.size()/2];
    uint64_t start=SDL_GetTicksNS();
    const Image &sky=wallImage(skytexture);
    const byte *palette=(const byte*)W_CacheLumpName((char*)"PLAYPAL",PU_CACHE);
    // Fourth-power luminance favors highlights over large mid-tone areas.
    std::vector<double> columns(sky.width);
    auto weight=[&](int x,int y,const byte *&rgb) {
        size_t i=(size_t)y*sky.width+x;rgb=palette+sky.pixels[2*i]*3;
        if(!sky.pixels[2*i+1])return 0.0;
        double luma=(0.299*rgb[0]+0.587*rgb[1]+0.114*rgb[2])/255.0;
        return luma*luma*luma*luma;
    };
    for(int x=0;x<sky.width;++x)for(int y=0;y<sky.height;++y) {const byte *rgb;columns[x]+=weight(x,y,rgb);}
    int best=0;double bestScore=-1;
    for(int x=0;x<sky.width;++x) {
        double score=0;
        for(int d=-4;d<=4;++d)score+=columns[(x+d+sky.width)%sky.width];
        if(score>bestScore) {bestScore=score;best=x;}
    }
    double tint[3]={},total=0;
    for(int d=-4;d<=4;++d)for(int y=0;y<sky.height;++y) {
        const byte *rgb;double w=weight((best+d+sky.width)%sky.width,y,rgb);
        for(int c=0;c<3;++c)tint[c]+=rgb[c]/255.0*w;
        total+=w;
    }
    double peak=std::max({tint[0],tint[1],tint[2],1e-9});
    for(int c=0;c<3;++c)sunTint[c]=total>0?(float)(0.5+0.5*tint[c]/peak):1.0f;
    float yaw=-(best+0.5f)*2*doomPi/(4.0f*sky.width),elevation=40*doomPi/180;
    float bright=(float)std::pow(total/(9.0*sky.height),0.25),glow=std::clamp((bright-0.2f)/0.4f,0.0f,1.0f);
    sunYaw=yaw;sunGlow=glow*glow*(3-2*glow);
    const float direction[3]={std::cos(yaw)*std::cos(elevation),std::sin(yaw)*std::cos(elevation),std::sin(elevation)};
    float sunLength=std::hypot(direction[0],direction[1]),sunX=direction[0]/sunLength,sunY=direction[1]/sunLength;
    sunAzimuth[0]=sunX;sunAzimuth[1]=sunY;
    std::atomic<size_t> lit{0};
    parallelFor(mapHeight,[&](int y) {
        for(int x=0;x<mapWidth;++x) {
            size_t i=(size_t)y*mapWidth+x;
            int visible=sunCell(bakeMap,i,direction);
            if(visible<0)continue;
            contactCells[4*i+2]=(uint8_t)visible;
            if(visible>0)++lit;
        }
    });
    parallelFor((int)bakeOrder.size(),[&](int n) {sunStrip(bakeMap,bakeStrips[bakeOrder[n]],direction);});
    contactTexture=gpuCreateTexture(GpuFormat::RGBA8,mapWidth,mapHeight,contactCells.data(),mapWidth*4);
    if(!contactTexture)I_Error((char*)"Could not allocate surface maps");
    uploadWallBake();
    for(int c=0;c<3;++c)sunDirection[c]=direction[c];
    bakeShafts();
    sunLevel=level;
    fprintf(stderr,"3D sun: yaw %.0f degrees, %zu sunlit cells, %zu sunbeams, %zu wall strips in %dx%d, baked in %.0f ms.\n",
        yaw*180/doomPi,lit.load(),sunShafts.size(),bakeOrder.size(),atlasWidth,atlasHeight,(SDL_GetTicksNS()-start)/1e6);
}
// Sunbeams: about one per 24 units of sunlit indoor floor (sky holes count as
// indoors when enclosed), jittered within its block so the beams never line
// up in rows. Each runs from the floor back toward the sun, sampled on the
// map grid, while it stays in indoor air; it ends a little past the point
// where it leaves through a sky ceiling or a window into open sky.
void bakeShafts() {
    sunShafts.clear();
    std::vector<char> indoor(numsectors);
    for(int i=0;i<numsectors;++i)indoor[i]=sectors[i].ceilingpic!=skyflatnum||enclosure(&sectors[i])>0.5f;
    int block=std::max(1,(int)std::lround(24/mapCell));
    for(int by=0;by<mapHeight;by+=block)for(int bx=0;bx<mapWidth;bx+=block) {
        int x=std::min(mapWidth-1,bx+(int)(hashUnit(bx,by,0x5a17u)*block)),y=std::min(mapHeight-1,by+(int)(hashUnit(bx,by,0x5a18u)*block));
        size_t i=(size_t)y*mapWidth+x;int sector=seamCells[i].own;
        if(sector==noSector||!indoor[sector]||sectors[sector].floorpic==skyflatnum)continue;
        float visible=contactCells[4*i+2]/255.0f;
        if(visible<0.5f)continue;
        float px=mapOrigin[0]+(x+0.5f)*mapCell,py=mapOrigin[1]+(y+0.5f)*mapCell,pz=bakeMap.sectors[sector].floor;
        float length=0;bool left=false;
        for(float t=4;t<1024;t+=4) {
            float qx=px+sunDirection[0]*t,qy=py+sunDirection[1]*t,qz=pz+sunDirection[2]*t;
            int cx=(int)std::floor((qx-mapOrigin[0])/mapCell),cy=(int)std::floor((qy-mapOrigin[1])/mapCell);
            if(cx<0||cy<0||cx>=mapWidth||cy>=mapHeight)break;
            int at=seamCells[(size_t)cy*mapWidth+cx].own;
            if(at==noSector||qz<bakeMap.sectors[at].floor)break;
            if(!indoor[at]||qz>bakeMap.sectors[at].ceiling) {left=bakeMap.sectors[at].sky;break;}
            length=t;
        }
        if(left&&length>=24)sunShafts.push_back({px,py,pz,length+24,visible,sector});
    }
}
// Wall vertex coordinates in the bake atlas; -1 leaves the wall without one.
// Walls also carry their strip's origin and scale in the tint for the
// texel-aligned read (blood decals use the tint for themselves).
void bakeWallCoordinates(const line_t &line,int side,float along,Vertex &vertex,bool strip=false) {
    if(bakeStrips.empty())return;
    const BakeStrip &s=bakeStrips[(size_t)(&line-lines)*2+side];
    if(s.x<0)return;
    vertex.sunU=s.x+along*s.scale;
    vertex.sunV=s.y+(std::clamp(vertex.z,s.low,s.high)-s.low)*s.scale;
    if(strip) {vertex.red=(float)s.x;vertex.green=(float)s.y;vertex.blue=s.scale;}
}
void findSkyOpenings() {
    skyOpeningOf.assign(numsectors,-1);skyOpenings.clear();
    for(int first=0;first<numsectors;++first) {
        if(sectors[first].ceilingpic!=skyflatnum||skyOpeningOf[first]>=0)continue;
        int id=(int)skyOpenings.size();skyOpenings.emplace_back();
        std::vector<int> stack={first};skyOpeningOf[first]=id;
        while(!stack.empty()) {
            const sector_t *sector=&sectors[stack.back()];stack.pop_back();
            for(int i=0;i<sector->linecount;++i) {
                const line_t *line=sector->lines[i];
                const sector_t *other=line->frontsector==sector?line->backsector:line->frontsector;
                if(other==sector)continue;
                if(other&&other->ceilingpic==skyflatnum) {
                    int n=(int)(other-sectors);
                    if(skyOpeningOf[n]<0) {skyOpeningOf[n]=id;stack.push_back(n);}
                    continue;
                }
                float length=std::hypot(units(line->dx),units(line->dy));
                skyOpenings[id].edge+=length;
                // Only a real gap counts (closed doors and ledges stay walls), and
                // only under a short rim: a ceiling hole sits just above the room's
                // ceiling, while cliffs around a canyon rise far above theirs.
                if(!other||std::min(ceilZ(sector),ceilZ(other))-std::max(floorZ(sector),floorZ(other))<24)continue;
                float rim=std::clamp((128-(ceilZ(sector)-ceilZ(other)))/64,0.0f,1.0f);
                if(rim>0)skyOpenings[id].indoor.push_back({other,length*rim});
            }
        }
    }
}
// How enclosed a sky sector's opening is: 0 for open air up to 1 where most
// of its edge opens into indoor sectors, as a hole in a ceiling does. light
// receives those sectors' current light, weighted by edge length.
float enclosure(const sector_t *sector,float *light) {
    if(skyOpeningOf.size()!=(size_t)numsectors)findSkyOpenings();
    if(light)*light=0;
    int id=skyOpeningOf[sector-sectors];if(id<0)return 0;
    const SkyOpening &opening=skyOpenings[id];
    float open=0,sum=0;
    for(const auto &[other,length]:opening.indoor) {open+=length;sum+=length*lighting(other);}
    if(open<=0||opening.edge<=0)return 0;
    if(light)*light=sum/open;
    float t=std::clamp((open/opening.edge-0.3f)/0.3f,0.0f,1.0f);
    return t*t*(3-2*t);
}
// Sky sectors pass their enclosure and outdoor floor to the shader packed in
// sectorInfo w: 1 + round(enclosure*255) + round(floor*255)/256 (0 indoors).
// The floor is the lowest light sun shadow and sky occlusion may take the
// sector to: the light of the indoor sectors its opening leads into, by its
// enclosure. A hole in a bright room's ceiling then never shades below the
// room, while a courtyard with a door or two keeps its shadows.
float packOutdoor(const sector_t *sector,float *enclosed=nullptr,float *floor=nullptr) {
    float light,e=enclosure(sector,&light);
    if(enclosed)*enclosed=e;
    if(floor)*floor=light*e;
    return 1+std::round(e*255)+std::round(light*e*255)/256;
}
// CPU twin of the shader's sunLight on floors, for sprites and the weapon.
// Under a sky, shadow drops light steps outdoors while an enclosed opening
// lights like indoors, blended by its enclosure.
float sunLightAt(float x,float y,const sector_t *sector,float light) {
    if(!settings.sun||sunLevel<=0||contactCells.empty())return light;
    int cx=std::clamp((int)std::floor((x-mapOrigin[0])/mapCell),0,mapWidth-1);
    int cy=std::clamp((int)std::floor((y-mapOrigin[1])/mapCell),0,mapHeight-1);
    float visible=contactCells[4*((size_t)cy*mapWidth+cx)+2]/255.0f;
    float lifted=light+std::round(std::max(0.0f,sunLevel-light)*visible*16)/16;
    if(sector->ceilingpic!=skyflatnum)return lifted;
    float enclosed,floor;packOutdoor(sector,&enclosed,&floor);
    float shaded=light-std::round(light*0.3f*(1-visible)*16)/16;
    return std::max(std::round((shaded+(lifted-shaded)*enclosed)*16)/16,std::min(light,floor));
}
// CPU twin of seamLight at the nearest cell, for sprites and the weapon.
float seamLightAt(float x,float y,const sector_t *sector,float fallback) {
    if(!settings.softLight||seamCells.empty())return fallback;
    int cx=(int)std::floor((x-mapOrigin[0])/mapCell),cy=(int)std::floor((y-mapOrigin[1])/mapCell);
    if(cx<0||cy<0||cx>=mapWidth||cy>=mapHeight)return fallback;
    const auto &cell=seamCells[(size_t)cy*mapWidth+cx];
    uint16_t own=(uint16_t)(sector-sectors);
    if(cell.own!=own||cell.neighbor==noSector)return fallback;
    return fallback+(lighting(&sectors[cell.neighbor])-fallback)*cell.weight/65535.0f;
}
std::array<float,3> frameGlow(int sprite,int frame) {
    frame&=FF_FRAMEMASK;
    if(sprite<0||sprite>=numsprites||frame>=sprites[sprite].numframes) return {1,1,1};
    unsigned key=(unsigned(sprite)<<16)|unsigned(frame);
    auto cached=glowColors.find(key);if(cached!=glowColors.end()) return cached->second;
    const auto &sf=sprites[sprite].spriteframes[frame];
    std::array<float,3> color={1,1,1};float weight=-1;
    for(int n=0;n<(sf.rotate?8:1);++n) {
        const Image &image=lumpImage(firstspritelump+sf.lump[n],false);
        if(image.glowWeight>weight) {weight=image.glowWeight;color=image.glow;}
    }
    glowColors.emplace(key,color);return color;
}
void interpolatedPosition(const mobj_t &thing,float fraction,float &x,float &y,float &z) {
    x=units(thing.x);y=units(thing.y);z=units(thing.z);
    auto old=oldThings.find(&thing);
    if(old!=oldThings.end()&&std::hypot(x-old->second[0],y-old->second[1])<128) {
        x=old->second[0]+(x-old->second[0])*fraction;
        y=old->second[1]+(y-old->second[1])*fraction;
        z=old->second[2]+(z-old->second[2])*fraction;
    }
}
void muzzleFlash(mobj_t *source,int weapon,int projectile) {
    if(!source||gamestate!=GS_LEVEL) return;
    if(flashLevelSerial!=r_levelserial) {flashPulses.clear();flashLevelSerial=r_levelserial;}
    float radius=384,strength=1.9f;
    if(weapon==wp_shotgun||weapon==wp_supershotgun) {radius=448;strength=2.4f;}
    else if(weapon==wp_missile||weapon==wp_bfg) {radius=480;strength=2.6f;}
    else if(weapon==wp_plasma) {radius=320;strength=1.6f;}
    float z=source->player?units(source->player->viewz)-8:units(source->z)+units(source->height)*0.65f;
    int sprite=source->sprite,frame=source->frame;
    if(projectile>=0&&projectile<NUMMOBJTYPES) {
        const auto &state=states[mobjinfo[projectile].spawnstate];sprite=state.sprite;frame=state.frame;
    } else if(source->player&&weapon>=0&&weapon<NUMWEAPONS) {
        const auto &state=states[weaponinfo[weapon].flashstate];sprite=state.sprite;frame=state.frame;
    }
    FlashPulse pulse={units(source->x),units(source->y),z,radius,strength,leveltime,source,frameGlow(sprite,frame)};
    for(auto &old:flashPulses) if(old.source==source) {old=pulse;return;}
    if(flashPulses.size()==8) flashPulses.erase(flashPulses.begin());
    flashPulses.push_back(pulse);
}
// Directionality: 0 lights every surface alike, 1 is full Lambert facing.
// Unoccluded lights skip wall blockers, so shading them costs no ray tests.
// baked: the part of this light already in the bake maps, which world surfaces
// subtract so they only gain its flicker and detail (0 for dynamic-only lights).
// spot: a spotlight's unit axis and cone cosine (direction); without a cone,
// x is a glowing panel's half-height, which softens its wall shadows.
void appendLight(float x,float y,float z,float radius,float strength,const std::array<float,3> &color,const mobj_t *source,float directionality,bool occluded=true,float baked=0,
                 const std::array<float,4> &spot={}) {
    if(flashes.count>=maxLights) return;
    lightSources[flashes.count]=source;fogOnly[flashes.count]=false;
    Flash &f=flashes.lights[flashes.count++];
    f={{x,y,z,radius},strength,(unsigned)lightBlockers.size(),0,baked,{color[0],color[1],color[2],directionality},{spot[0],spot[1],spot[2],spot[3]}};
    if(!occluded) return;
    for(int i=0;i<numlines;++i) {
        const line_t &line=lines[i];
        float ax=units(line.v1->x),ay=units(line.v1->y),bx=units(line.v2->x),by=units(line.v2->y);
        if(std::max(ax,bx)<x-radius||std::min(ax,bx)>x+radius||
           std::max(ay,by)<y-radius||std::min(ay,by)>y+radius) continue;
        float bottom=1,top=-1;
        if(line.frontsector&&line.backsector) {
            bottom=std::max(floorZ(line.frontsector),floorZ(line.backsector));
            top=std::min(ceilZ(line.frontsector),ceilZ(line.backsector));
            if(line.frontsector->ceilingpic==skyflatnum&&line.backsector->ceilingpic==skyflatnum) top=32768;
            if(line.frontsector==line.backsector) continue;
        }
        LightBlocker blocker={{ax,ay,bx,by},{bottom,top,0,0}};
        if(!doom_flash_blocker_relevant(&f,&blocker,!line.backsector)) continue;
        lightBlockers.push_back(blocker);
    }
    f.count=doom_flash_cull_hidden(&f,lightBlockers.data()+f.first,(uint32_t)lightBlockers.size()-f.first);
    lightBlockers.resize(f.first+f.count);
}
void barrelBlast(const mobj_t &thing,float x,float y,float z);
void doorSpill(const Uniforms &camera);
// Exploding barrels: the light builds through the fireball and peaks on the
// blast tic (A_Explode), then fades; the blast itself also leaves sparks and
// a floor scorch, once per barrel.
std::unordered_map<const mobj_t*,bool> burstBarrels;
float barrelLight(const mobj_t &thing) {
    if(thing.type!=MT_BARREL||!thing.state)return 0;
    int state=int(thing.state-states);
    if(state<S_BEXP||state>S_BEXP5)return 0;
    static const float curve[]={0.7f,1.2f,1.8f,2.8f,1.5f};
    float strength=curve[state-S_BEXP];
    if(state==S_BEXP5)strength*=std::clamp(thing.tics/10.0f,0.0f,1.0f);
    return strength;
}
// Decoration lights: things whose spawn frame is fullbright by design
// (torches, candles, lamps, burning barrels), recognized from thing data
// rather than names. Monsters, shots, pickups and short-lived fogs and puffs
// are excluded. Animated flames flicker like DOOM's fire-flicker sectors:
// a stepped drop every 4 tics, from a hash rather than the game's RNG.
bool decorationLight(const mobj_t &thing) {
    if(thing.player||(thing.flags&(MF_MISSILE|MF_COUNTKILL|MF_SPECIAL|MF_NOBLOCKMAP)))return false;
    const state_t &spawn=states[thing.info->spawnstate];
    return (spawn.frame&FF_FULLBRIGHT)&&(thing.frame&FF_FULLBRIGHT);
}
float decorationFlicker(const mobj_t &thing) {
    const state_t &spawn=states[thing.info->spawnstate];
    if(spawn.tics<0||spawn.nextstate==thing.info->spawnstate)return 1;
    uint32_t hash=(uint32_t)(uintptr_t)&thing*2654435761u^(uint32_t)(leveltime/4)*40503u;
    hash^=hash>>13;hash*=0x5bd1e995u;hash^=hash>>15;
    return 1-0.12f*(hash&3);
}
// Bake-only lights flicker by group: each animated decoration falls in one
// of 15 groups that step like decorationFlicker; group 0 stays steady.
int flickerGroup(const mobj_t &thing) {
    const state_t &spawn=states[thing.info->spawnstate];
    if(spawn.tics<0||spawn.nextstate==thing.info->spawnstate)return 0;
    uint32_t hash=(uint32_t)(uintptr_t)&thing*2654435761u;
    return 1+(int)((hash>>16)%(flickerGroups-1));
}
float groupFlicker(int group) {
    if(group<=0)return 1;
    uint32_t hash=(uint32_t)group*2654435761u^(uint32_t)(leveltime/4)*40503u;
    hash^=hash>>13;hash*=0x5bd1e995u;hash^=hash>>15;
    return 1-0.12f*(hash&3);
}
// A decoration's unflickered light, shared by the dynamic set and the bake.
// Size follows the artwork: a candle glows small, a tall torch wide.
BakeLight decorationSource(const mobj_t &thing) {
    const auto &frame=sprites[thing.sprite].spriteframes[thing.frame&FF_FRAMEMASK];
    float height=(float)lumpImage(firstspritelump+frame.lump[0],false).height;
    auto color=frameGlow(thing.sprite,thing.frame);
    return {units(thing.x),units(thing.y),units(thing.z)+height*0.85f,std::clamp(height*4.0f,96.0f,320.0f),
            std::clamp(height/60.0f,0.6f,1.2f),0.75f,{color[0],color[1],color[2]}};
}
const std::vector<int> &bakeGridLights(float x,float y) {
    int gx=std::clamp((int)std::floor((x-mapOrigin[0])/bakeGridCell),0,bakeGridWidth-1);
    int gy=std::clamp((int)std::floor((y-mapOrigin[1])/bakeGridCell),0,bakeGridHeight-1);
    return bakeGrid[(size_t)gy*bakeGridWidth+gx];
}
// A light that casts things' floor shadows (addEnemyShadow).
struct ShadowLight { float x,y,z,strength; };
float luminance(const float color[3]) {return 0.299f*color[0]+0.587f*color[1]+0.114f*color[2];}
StaticSample traceStatic(float x,float y,float z,int sector,const BakeMap &map=bakeMap) {
    StaticSample sample;
    if(bakeGrid.empty())return sample;
    float groups[flickerGroups]={};
    int points=(int)bakeSources.size();
    for(int n:bakeGridLights(x,y)) {
        float towards[3];
        const float *color=n<points?bakeSources[n].color:bakeAreas[n-points].color;
        float amount=n<points?addBakedLight(map,bakeSources[n],sector,x,y,z,nullptr,sample.color.data(),towards)
                             :addBakedArea(map,bakeAreas[n-points],sector,x,y,z,nullptr,sample.color.data(),towards);
        if(amount<=0)continue;
        float weight=amount*luminance(color);
        for(int c=0;c<3;++c)sample.towards[c]+=towards[c]*weight;
        sample.amount+=weight;
        if(n>=points)continue;
        groups[sourceGroups[n]]+=weight;
        if(bakeSources[n].z>z-24&&amount>sample.shadowAmount) {sample.shadowAmount=amount;sample.shadow=n;}
    }
    for(int g=1;g<flickerGroups;++g)if(groups[g]>0&&groups[g]>(sample.group?groups[sample.group]:0))sample.group=g;
    if(sample.group)sample.groupShare=groups[sample.group]/sample.amount;
    return sample;
}
// Grid lighting (settings.gridSpriteLight): a StaticSample every 32 units,
// 32 units above the floor of the sector at the cell's center and, in rooms
// taller than 112, also 96 above it. Things blend the nearest cells instead
// of tracing rays every frame. Baked after each light bake (staticSerial);
// cells in void or in closed sectors (doors) hold none.
bool gridLightActive() {
    return settings.gridSpriteLight&&bakedLightsActive&&staticSerial==lightBakeSerial&&!staticGrid.empty();
}
StaticCell staticCellAt(const BakeMap &map,int gx,int gy) {
    StaticCell cell;
    float x=mapOrigin[0]+(gx+0.5f)*staticCell,y=mapOrigin[1]+(gy+0.5f)*staticCell;
    int cx=std::clamp((int)std::floor((x-mapOrigin[0])/mapCell),0,mapWidth-1);
    int cy=std::clamp((int)std::floor((y-mapOrigin[1])/mapCell),0,mapHeight-1);
    int sector=seamCells[(size_t)cy*mapWidth+cx].own;
    if(sector==noSector)return cell;
    const BakeSector &s=map.sectors[sector];
    if(s.ceiling-s.floor<16)return cell;
    cell.used=true;
    cell.low=s.floor+std::min(32.0f,(s.ceiling-s.floor)*0.5f);
    cell.at[0]=traceStatic(x,y,cell.low,sector,map);
    cell.high=cell.low;cell.at[1]=cell.at[0];
    if(s.ceiling-s.floor>112) {cell.high=s.floor+96;cell.at[1]=traceStatic(x,y,cell.high,sector,map);}
    return cell;
}
void bakeStaticGrid() {
    finishRelight();
    staticSerial=lightBakeSerial;staticGrid.clear();
    if(bakeGrid.empty()||seamCells.empty())return;
    uint64_t start=SDL_GetTicksNS();
    staticWidth=(int)std::ceil(mapWidth*mapCell/staticCell);staticHeight=(int)std::ceil(mapHeight*mapCell/staticCell);
    staticGrid.assign((size_t)staticWidth*staticHeight,{});
    std::atomic<size_t> used{0};
    parallelFor(staticHeight,[&](int gy) {
        for(int gx=0;gx<staticWidth;++gx) {
            StaticCell &cell=staticGrid[(size_t)gy*staticWidth+gx];
            cell=staticCellAt(bakeMap,gx,gy);
            if(cell.used)++used;
        }
    });
    fprintf(stderr,"3D light grid: %zu cells of %dx%d, baked in %.0f ms.\n",used.load(),staticWidth,staticHeight,(SDL_GetTicksNS()-start)/1e6);
}
// The grid's blend at (x,y,z): bilinear over used cells, between each
// cell's two heights. The flicker group is the heaviest cell's; the shadow
// light is the one with the most blended amount.
StaticSample gridStatic(float x,float y,float z) {
    StaticSample result;
    float gx=(x-mapOrigin[0])/staticCell-0.5f,gy=(y-mapOrigin[1])/staticCell-0.5f;
    int x0=(int)std::floor(gx),y0=(int)std::floor(gy);float fx=gx-x0,fy=gy-y0;
    float total=0,heaviest=0;
    StaticSample parts[4];float weights[4]={};
    for(int k=0;k<4;++k) {
        int cx=x0+(k&1),cy=y0+(k>>1);
        if(cx<0||cy<0||cx>=staticWidth||cy>=staticHeight)continue;
        const StaticCell &cell=staticGrid[(size_t)cy*staticWidth+cx];
        if(!cell.used)continue;
        float w=((k&1)?fx:1-fx)*((k>>1)?fy:1-fy)+1e-4f;
        float t=cell.high>cell.low?std::clamp((z-cell.low)/(cell.high-cell.low),0.0f,1.0f):0;
        const StaticSample &a=cell.at[0],&b=cell.at[1];
        StaticSample &p=parts[k];
        for(int c=0;c<3;++c) {p.color[c]=a.color[c]+(b.color[c]-a.color[c])*t;p.towards[c]=a.towards[c]+(b.towards[c]-a.towards[c])*t;}
        p.amount=a.amount+(b.amount-a.amount)*t;
        const StaticSample &nearer=t<0.5f?a:b;
        p.group=nearer.group;p.groupShare=nearer.groupShare;p.shadow=nearer.shadow;p.shadowAmount=nearer.shadowAmount;
        weights[k]=w;total+=w;
        if(w*p.amount>heaviest) {heaviest=w*p.amount;result.group=p.group;}
    }
    if(total<=0)return result;
    float share=0,groupWeight=0;
    for(int k=0;k<4;++k) {
        if(weights[k]<=0)continue;
        float w=weights[k]/total;const StaticSample &p=parts[k];
        for(int c=0;c<3;++c) {result.color[c]+=p.color[c]*w;result.towards[c]+=p.towards[c]*w;}
        result.amount+=p.amount*w;
        if(result.group&&p.group==result.group) {share+=p.groupShare*w;groupWeight+=w;}
        if(p.shadow<0)continue;
        float blended=0;
        for(int j=0;j<4;++j)if(weights[j]>0&&parts[j].shadow==p.shadow)blended+=parts[j].shadowAmount*weights[j]/total;
        if(blended>result.shadowAmount) {result.shadowAmount=blended;result.shadow=p.shadow;}
    }
    if(groupWeight>0)result.groupShare=share/groupWeight;
    return result;
}
// Baked light at a thing's center or the eye: the static lights (from the
// grid when it is on), flickered with bake-only lights, plus the bounce
// between the floor and ceiling below and above it. sample receives the
// static light's details.
std::array<float,3> bakedLightAt(float x,float y,float z,const sector_t *sector,StaticSample *sample=nullptr) {
    StaticSample statics;
    if(bakedLightsActive)statics=gridLightActive()?gridStatic(x,y,z):traceStatic(x,y,z,(int)(sector-sectors));
    std::array<float,3> result=statics.color;
    if(bakeOnlyActive&&statics.group) {
        float flicker=1-statics.groupShare*(1-groupFlicker(statics.group));
        for(float &c:result)c*=flicker;
    }
    if(settings.bounce&&bounceKey>=0&&!flatLightCells.empty()) {
        int cx=std::clamp((int)std::floor((x-mapOrigin[0])/mapCell),0,mapWidth-1);
        int cy=std::clamp((int)std::floor((y-mapOrigin[1])/mapCell),0,mapHeight-1);
        size_t cell=(size_t)cy*mapWidth+cx;
        for(int c=0;c<3;++c)
            result[c]+=(flatCell(floorBounce,cell)[c]+flatCell(ceilingBounce,cell)[c])/255.0f*bounceStrength;
    }
    if(sample)*sample=statics;
    return result;
}
// The static light that casts a thing's shadow, when bake-only lights or the
// grid stand in for the dynamic copies: above the thing's feet at z.
bool staticShadow(const StaticSample &sample,float z,ShadowLight &light) {
    if(!(bakeOnlyActive||gridLightActive())||sample.shadow<0)return false;
    const BakeLight &source=bakeSources[sample.shadow];
    if(source.z<=z+8)return false;
    float amount=sample.shadowAmount*(bakeOnlyActive?groupFlicker(sourceGroups[sample.shadow]):1);
    light={source.x,source.y,source.z,amount};
    return true;
}
// Bake-only static lights near the eye, for the fog glow (collectSurfaceLights).
struct StaticFog { float x,y,z,radius,strength;std::array<float,3> color; };
std::vector<StaticFog> staticFog;
void collectFlashes(float fraction,const Uniforms &camera) {
    flashes={};lightBlockers.clear();fogOnly={};
    // The flashlight is held low and right of the eye, like a lamp beside the
    // weapon, aimed to meet the view 256 units ahead. A light at the eye
    // would hide every shadow it casts behind its caster; from here walls,
    // door frames and things throw visible shadows (see flashlightSilhouette).
    if(flashlightOn) {
        float lamp[3],aim[3],length=0;
        for(int c=0;c<3;++c) {
            lamp[c]=camera.eye[c]+camera.right[c]*14-camera.up[c]*16;
            aim[c]=camera.eye[c]+camera.forward[c]*256-lamp[c];length+=aim[c]*aim[c];
        }
        length=std::sqrt(length);
        appendLight(lamp[0],lamp[1],lamp[2],900,1.9f,{1.0f,0.96f,0.86f},nullptr,0.85f,true,0,{aim[0]/length,aim[1]/length,aim[2]/length,0.90f});
    }
    if(flashLevelSerial!=r_levelserial) {flashPulses.clear();flashLevelSerial=r_levelserial;}
    flashPulses.erase(std::remove_if(flashPulses.begin(),flashPulses.end(),[](const FlashPulse &p) {
        return leveltime<p.tic||leveltime-p.tic>=6;
    }),flashPulses.end());
    for(const auto &p:flashPulses) {
        float strength=p.strength*doom_flash_fade(leveltime-p.tic,fraction);
        if(strength>0) appendLight(p.x,p.y,p.z,p.radius,strength,p.color,p.source,0.75f);
    }
    // Live WAD missile objects supply continuous light, including their bright
    // impact frames. Keep the nearest eight so rapid fire has a bounded cost.
    struct Candidate {mobj_t *thing;float distance;};
    std::vector<Candidate> projectiles,barrels,decorations;
    for(auto &entry:burstBarrels)entry.second=false;
    for(int i=0;i<numsectors;++i) for(mobj_t *thing=sectors[i].thinglist;thing;thing=thing->snext) {
        if(barrelLight(*thing)>0) {
            float x,y,z;interpolatedPosition(*thing,fraction,x,y,z);
            float dx=x-camera.eye[0],dy=y-camera.eye[1],dz=z-camera.eye[2];
            barrels.push_back({thing,dx*dx+dy*dy+dz*dz});
            auto seen=burstBarrels.find(thing);
            if(seen!=burstBarrels.end())seen->second=true;
            else if(thing->state-states>=S_BEXP4) {burstBarrels[thing]=true;barrelBlast(*thing,x,y,z);}
            continue;
        }
        if(decorationLight(*thing)) {
            float dx=units(thing->x)-camera.eye[0],dy=units(thing->y)-camera.eye[1];
            float distance=std::hypot(dx,dy);
            if(distance<1024)decorations.push_back({thing,distance});
            continue;
        }
        if(!(thing->info->flags&MF_MISSILE)||!(thing->frame&FF_FULLBRIGHT)) continue;
        float x,y,z;interpolatedPosition(*thing,fraction,x,y,z);
        float dx=x-camera.eye[0],dy=y-camera.eye[1],dz=z-camera.eye[2];
        projectiles.push_back({thing,dx*dx+dy*dy+dz*dz});
    }
    for(auto it=burstBarrels.begin();it!=burstBarrels.end();) it=it->second?std::next(it):burstBarrels.erase(it);
    std::sort(barrels.begin(),barrels.end(),[](const Candidate &a,const Candidate &b) {return a.distance<b.distance;});
    for(size_t n=0;n<std::min(size_t(4),barrels.size());++n) {
        const mobj_t &thing=*barrels[n].thing;
        float x,y,z;interpolatedPosition(thing,fraction,x,y,z);
        appendLight(x,y,z+24,448,barrelLight(thing),frameGlow(thing.sprite,thing.frame),&thing,0.75f);
    }
    std::sort(decorations.begin(),decorations.end(),[](const Candidate &a,const Candidate &b) {return a.distance<b.distance;});
    staticFog.clear();
    for(size_t n=0;n<std::min(size_t(10),decorations.size());++n) {
        const mobj_t &thing=*decorations[n].thing;
        BakeLight light=decorationSource(thing);
        // Bake-only: the bake lights everything; only the fog glow remains.
        if(bakeOnlyActive) {
            float strength=light.strength*groupFlicker(flickerGroup(thing));
            staticFog.push_back({light.x,light.y,light.z,light.radius,strength,{light.color[0],light.color[1],light.color[2]}});
            continue;
        }
        float flicker=decorationFlicker(thing);
        float strength=light.strength*flicker*std::clamp((1024-decorations[n].distance)/256,0.0f,1.0f);
        appendLight(light.x,light.y,light.z,light.radius,strength,{light.color[0],light.color[1],light.color[2]},&thing,0.75f,
                    true,bakedLightsActive?1/flicker:0);
    }
    std::sort(projectiles.begin(),projectiles.end(),[](const Candidate &a,const Candidate &b) {return a.distance<b.distance;});
    for(size_t n=0;n<std::min(size_t(8),projectiles.size());++n) {
        const mobj_t &thing=*projectiles[n].thing;
        float x,y,z;interpolatedPosition(thing,fraction,x,y,z);
        float radius=256,strength=1.7f;
        if(thing.type==MT_BFG) {radius=448;strength=2.4f;}
        else if(thing.type==MT_ROCKET) {radius=288;strength=1.5f;}
        appendLight(x,y,z+units(thing.height)*0.5f,radius,strength,frameGlow(thing.sprite,thing.frame),&thing,0.75f);
    }
    doorSpill(camera);
}
float flashAt(float x,float y,float z,const Flash &f) {
    return doom_flash_at(x,y,z,&f,lightBlockers.data());
}

// Vertex mode bits 12+ carry sector+1 for map-space lighting; 0 means none.
// Bits 256 (blood decal), 512 (hard-edged silhouette), 1024 (soft effect
// sprite) and 2048 (shiny airborne blood) sit below them.
unsigned sectorMode(const sector_t *sector) {return unsigned(sector-sectors+1)<<12;}
void quad(std::vector<Vertex> &out,Vertex a,Vertex b,Vertex c,Vertex d) {
    out.insert(out.end(),{a,b,c,a,c,d});
}
void surfaceLight(const SurfaceLightKey &key,float x,float y,float z,const Image &image,int facing,float radius=224,float extent=0) {
    if(!settings.emissive || image.emissionWeight==0) return;
    surfaceLights.push_back({key,x,y,z,radius,1.65f*image.emissionWeight,image.emissionColor,facing,extent});
}
const std::vector<FlatLightSample> &flatSamples(int sectorIndex,int lump,const Image &image) {
    auto key=std::make_pair(sectorIndex,lump);
    auto cached=flatLightSamples.find(key);
    if(cached!=flatLightSamples.end()) return cached->second;
    std::map<std::pair<int,int>,Point> samples;
    // Sample the texture's emission center on its world-aligned repeat grid.
    // BSP polygons only test membership; their centers never place lights.
    for(int leaf:sectorFloors[sectorIndex]) {
        const auto &poly=floors[leaf];if(poly.size()<3) continue;
        const auto &bounds=floorBounds[leaf];
        int firstU=(int)std::ceil((bounds[0]-image.emissionU)/image.width);
        int lastU=(int)std::floor((bounds[2]-image.emissionU)/image.width);
        int firstV=(int)std::ceil((-bounds[3]-image.emissionV)/image.height);
        int lastV=(int)std::floor((-bounds[1]-image.emissionV)/image.height);
        for(int u=firstU;u<=lastU;++u) for(int v=firstV;v<=lastV;++v) {
            Point point={image.emissionU+u*image.width,-image.emissionV-v*image.height};
            bool positive=false,negative=false;
            for(size_t n=0;n<poly.size();++n) {
                Point a=poly[n],b=poly[(n+1)%poly.size()];
                float cross=(b.x-a.x)*(point.y-a.y)-(b.y-a.y)*(point.x-a.x);
                positive|=cross>0.01f;negative|=cross<-0.01f;
            }
            if(!(positive&&negative)) samples[{u,v}]=point;
        }
    }
    std::vector<FlatLightSample> result;
    for(const auto &sample:samples) result.push_back({sample.second,sample.first.first,sample.first.second});
    return flatLightSamples.emplace(key,std::move(result)).first->second;
}
void collectFlatLights() {
    if(!settings.emissive) return;
    for(int i=0;i<numsectors;++i) for(int ceiling=0;ceiling<2;++ceiling) {
        const sector_t &sector=sectors[i];int flat=ceiling?sector.ceilingpic:sector.floorpic;
        if(flat==skyflatnum)continue;
        int lump=firstflat+flattranslation[flat];const Image &image=lumpImage(lump,true);
        if(image.emissionWeight==0)continue;
        float z=sectorHeight(&sector,ceiling)+(ceiling?-8:8);
        for(const auto &sample:flatSamples(i,lump,image))
            surfaceLight({0,i,ceiling,sample.u,sample.v},sample.point.x,sample.point.y,z,image,ceiling,256);
    }
}
void collectSurfaceLights() {
    uint64_t now=SDL_GetTicksNS();
    float seconds=surfaceLightTime?(now-surfaceLightTime)/1e9f:1.0f/60;
    surfaceLightTime=now;
    if(bakeOnlyActive) {
        // The bake holds these lights; the nearest static lights stay as
        // fog-only lights so the glow around torches and pools remains.
        surfaceSelection.clear();
        if(!settings.fog)return;
        if(settings.emissive)for(const auto &light:mergeSurfaceLights(surfaceLights))
            staticFog.push_back({light.x,light.y,light.z,light.radius,light.strength,light.color});
        std::vector<std::pair<float,size_t>> scored;
        for(size_t n=0;n<staticFog.size();++n) {
            const StaticFog &f=staticFog[n];
            float distance=std::hypot(f.x-surfaceEye[0],f.y-surfaceEye[1]);
            if(distance<=f.radius+512)scored.emplace_back(f.strength/(1+distance/128),n);
        }
        std::sort(scored.begin(),scored.end(),[](const auto &a,const auto &b){return a.first>b.first;});
        for(size_t n=0;n<std::min(size_t(4),scored.size())&&flashes.count<maxLights;++n) {
            const StaticFog &f=staticFog[scored[n].second];
            appendLight(f.x,f.y,f.z,f.radius,f.strength,f.color,nullptr,0.75f,false);
            fogOnly[flashes.count-1]=true;
        }
        return;
    }
    if(!settings.emissive) {surfaceSelection.clear();return;}
    surfaceSelection.update(mergeSurfaceLights(surfaceLights),surfaceEye,seconds);
    // Surface lights reach a short way, but walls still stop them: the bake
    // subtracts only their flat share, so their gloss would otherwise show on
    // shiny surfaces through walls. The Performance switch skips the tests.
    for(const auto &slot:surfaceSelection.slots) {
        const auto &p=slot.light;
        appendLight(p.x,p.y,p.z,p.radius,p.strength*slot.gain,p.color,nullptr,0.35f,!settings.unoccludedSurfaceLights,bakedLightsActive?1.0f:0,
                    {std::min(p.extent,p.radius),0,0,0});
    }
}
void wallLights(const line_t &line,int side,Point a,Point b,float bottom,float top,int tex,float anchor,int span) {
    if(!settings.emissive||tex<=0||top<=bottom)return;
    const Image &image=wallImage(texturetranslation[tex]);
    float u=units(sides[line.sidenum[side]].textureoffset),len=std::hypot(b.x-a.x,b.y-a.y);
    if(image.emissionWeight<=0||len<=0)return;
    float nx=(b.y-a.y)/len,ny=-(b.x-a.x)/len;
    int facing=2+((int)std::lround(std::atan2(ny,nx)/(float)(M_PI/4))+8)%8;
    int firstU=(int)std::ceil((u-image.emissionU)/image.width);
    int lastU=(int)std::floor((u+len-image.emissionU)/image.width);
    int firstV=(int)std::ceil((anchor-top-image.emissionV)/image.height);
    int lastV=(int)std::floor((anchor-bottom-image.emissionV)/image.height);
    int stepV=std::max(1,64/image.height);
    for(int tileU=firstU;tileU<=lastU;tileU+=std::max(1,64/image.width)) {
        float along=image.emissionU+tileU*image.width-u,t=along/len;
        for(int tileV=firstV;tileV<=lastV;tileV+=stepV) {
            // Each sample stands for the rows of tiles it skips, within the wall piece.
            float z=anchor-image.emissionV-tileV*image.height;
            float extent=std::max(0.0f,std::min({image.height*stepV*0.5f,top-z,z-bottom}));
            surfaceLight({1,(int)(&line-lines),side,tileU,tileV,span},
                a.x+(b.x-a.x)*t+nx*4,a.y+(b.y-a.y)*t+ny*4,z,image,facing,224,extent);
        }
    }
}
void addWall(const line_t &line,int side,Point a,Point b,float bottom,float top,int tex,float anchor,float light,bool facing,int span) {
    wallLights(line,side,a,b,bottom,top,tex,anchor,span);
    if(!facing)return;
    if(tex<=0||top<=bottom) return;
    tex=texturetranslation[tex]; wallImage(tex);
    float u=units(sides[line.sidenum[side]].textureoffset),len=std::hypot(b.x-a.x,b.y-a.y);
    auto &out=batches[tex];unsigned mode=sectorMode(sides[line.sidenum[side]].sector);
    Vertex corners[4]={{a.x,a.y,top,u,anchor-top,light,mode},{b.x,b.y,top,u+len,anchor-top,light,mode},
                       {b.x,b.y,bottom,u+len,anchor-bottom,light,mode},{a.x,a.y,bottom,u,anchor-bottom,light,mode}};
    for(int n=0;n<4;++n)bakeWallCoordinates(line,side,n==1||n==2?len:0,corners[n],true);
    quad(out,corners[0],corners[1],corners[2],corners[3]);
}
// The strongest visible shot or projectile supplies a world-space light here.
// Contact shadows stay separate and remain after the transient shadow fades.
float decalLift(float x,float y,const Uniforms &camera);
void addEnemyShadow(const mobj_t &thing,const Image &image,int lump,bool flip,
                    float x,float y,float z,const Uniforms &camera,const ShadowLight *light=nullptr,bool anyThing=false) {
    if(!anyThing&&!(thing.info->flags&MF_COUNTKILL)&&thing.type!=MT_SKULL) return;
    const sector_t *sector=thing.subsector->sector;
    if(sector->floorpic==skyflatnum) return;
    float floor=floorZ(sector),height=std::max(0.0f,z-floor);
    float opacity=0.28f*lighting(sector)/(1.0f+height/48.0f);
    if(opacity<0.015f) return;
    float width=std::clamp(units(thing.radius)*2.0f,20.0f,80.0f)*std::min(1.7f,1.0f+height/160.0f);
    float length=width*0.65f,start=-length*0.35f;
    Point across={camera.right[0],camera.right[1]},away={-across.y,across.x};
    if(light) {
        float dx=x-light->x,dy=y-light->y,distance=std::hypot(dx,dy);
        if(distance>1.0f&&light->z>z+8.0f) {
            away={dx/distance,dy/distance};across={away.y,-away.x};
            length=std::clamp(distance*units(thing.height)/(light->z-z),width*0.65f,192.0f);
            start=0;opacity=std::clamp(light->strength*0.55f,0.0f,0.65f);
        }
    }
    auto point=[&](float u,float v) {return Point{x+across.x*u+away.x*v,y+across.y*u+away.y*v};};
    std::vector<Point> footprint={point(-width/2,start),point(width/2,start),
                                  point(width/2,start+length),point(-width/2,start+length)};
    std::array<float,4> bounds={1e9f,1e9f,-1e9f,-1e9f};
    for(Point p:footprint) {
        bounds[0]=std::min(bounds[0],p.x);bounds[1]=std::min(bounds[1],p.y);
        bounds[2]=std::max(bounds[2],p.x);bounds[3]=std::max(bounds[3],p.y);
    }
    auto &out=shadowBatches[lump];
    for(int leaf:sectorFloors[sector-sectors]) {
        const auto &b=floorBounds[leaf];
        if(b[2]<bounds[0]||b[0]>bounds[2]||b[3]<bounds[1]||b[1]>bounds[3]) continue;
        const auto &receiver=floors[leaf];if(receiver.size()<3) continue;
        auto polygon=receiver;
        for(size_t n=0;n<footprint.size()&&!polygon.empty();++n) {
            Point a=footprint[n],b=footprint[(n+1)%footprint.size()];
            polygon=clip(polygon,a,{b.x-a.x,b.y-a.y},false);
        }
        auto vertex=[&](Point p) {
            float dx=p.x-x,dy=p.y-y;
            float u=(dx*across.x+dy*across.y)/width+0.5f;
            float v=1.0f-((dx*away.x+dy*away.y)-start)/length;
            // Twice the blood's lift, so shadows fall on top of blood decals.
            return Vertex{p.x,p.y,floor+2*decalLift(p.x,p.y,camera),(flip?1-u:u)*image.width,v*image.height,opacity,0};
        };
        for(size_t n=1;n+1<polygon.size();++n)
            for(Point p:{polygon[0],polygon[n],polygon[n+1]}) out.push_back(vertex(p));
    }
}
// Dust scatters sunlight mostly onward: a Henyey-Greenstein lobe (g 0.35,
// averaging 1 over all directions) brightens beams seen toward the sun, about
// 1.7x, and dims them from behind, about 0.55x. x, y, z: eye to the point.
float sunScatter(float x,float y,float z) {
    if(!settings.sunScatter)return 1;
    constexpr float g=0.35f;
    float distance=std::sqrt(x*x+y*y+z*z);
    if(distance<1e-3f)return 1;
    float toward=(x*sunDirection[0]+y*sunDirection[1]+z*sunDirection[2])/distance;
    return (1-g*g)/std::pow(1+g*g-2*g*toward,1.5f);
}
// Sunbeam ribbons: each baked shaft as a quad turned about its axis to face
// the eye, drawn additively after the world without writing depth; mist.frag
// softens its edges, ends and contact with geometry. Shafts over a floor that
// has since moved are skipped.
void buildShafts(const Uniforms &camera) {
    shaftVertices.clear();
    if(!settings.sunShafts||!settings.sun||sunLevel<=0||camera.effects[2]!=0)return;
    const float *d=sunDirection,*eye=camera.eye;
    for(const SunShaft &s:sunShafts) {
        if(std::fabs(floorZ(&sectors[s.sector])-s.z)>1)continue;
        float half=s.length*0.5f,mid[3]={s.x+d[0]*half-eye[0],s.y+d[1]*half-eye[1],s.z+d[2]*half-eye[2]};
        float distance=std::sqrt(mid[0]*mid[0]+mid[1]*mid[1]+mid[2]*mid[2]);
        if(distance>1600||mid[0]*camera.forward[0]+mid[1]*camera.forward[1]+mid[2]*camera.forward[2]<-half)continue;
        float side[3]={d[1]*mid[2]-d[2]*mid[1],d[2]*mid[0]-d[0]*mid[2],d[0]*mid[1]-d[1]*mid[0]};
        float across=std::sqrt(side[0]*side[0]+side[1]*side[1]+side[2]*side[2]);
        if(across<1e-3f)continue;
        // Seen down the axis a ribbon turns edge-on, so it fades out.
        float facing=std::min(1.0f,across/distance*2.5f),far=std::clamp((1600-distance)/500,0.0f,1.0f);
        float alpha=s.strength*facing*far;
        if(alpha<0.02f)continue;
        for(float &c:side)c*=14/across;
        auto vertex=[&](float u,float v) {
            float t=v*s.length,x=s.x+d[0]*t+side[0]*u,y=s.y+d[1]*t+side[1]*u,z=s.z+d[2]*t+side[2]*u;
            Vertex out={x,y,z,u,v,alpha*sunScatter(x-eye[0],y-eye[1],z-eye[2]),2};
            out.red=sunTint[0];out.green=sunTint[1];out.blue=sunTint[2];
            return out;
        };
        quad(shaftVertices,vertex(-1,0),vertex(1,0),vertex(1,1),vertex(-1,1));
    }
}
void buildMist(const Uniforms &camera) {
    mistVertices.clear();
    if(!settings.fog||camera.effects[2]!=0)return;
    struct Triangle { float depth; Vertex v[3]; };
    std::vector<Triangle> triangles;
    for(int i=0;i<numsubsectors;++i) {
        const auto &poly=floors[i];if(poly.size()<3)continue;
        const sector_t &sector=*subsectors[i].sector;const auto &mist=sectorMist[&sector-sectors];
        if(!(mist.pit||mist.liquid||mist.toxic)||sector.floorpic==skyflatnum)continue;
        const auto &bounds=floorBounds[i];
        if(camera.eye[0]<bounds[0]-2000||camera.eye[0]>bounds[2]+2000||
           camera.eye[1]<bounds[1]-2000||camera.eye[1]>bounds[3]+2000)continue;
        float bottom=floorZ(&sector),top=ceilZ(&sector);
        float extent=std::min(mist.pit?24.0f:16.0f,std::max(0.0f,top-bottom-2));
        int layers=mist.pit?4:3;
        for(int layer=0;layer<layers&&extent>=4;++layer) {
            float z=bottom+extent*(layer+1)/(layers+1);
            float alpha=(mist.toxic?0.09f:mist.liquid?0.075f:0.06f)*(mist.outdoor?0.65f:1.0f);
            alpha*=1.0f-layer*0.15f;
            for(size_t n=1;n+1<poly.size();++n) {
                Triangle triangle={};int v=0;
                for(Point p:{poly[0],poly[n],poly[n+1]}) {
                    triangle.v[v++]={p.x,p.y,z,layer*0.31f,layer*0.17f,alpha,0,mist.color[0],mist.color[1],mist.color[2]};
                    triangle.depth+=(p.x-camera.eye[0])*camera.forward[0]+(p.y-camera.eye[1])*camera.forward[1]+(z-camera.eye[2])*camera.forward[2];
                }
                triangles.push_back(triangle);
            }
        }
    }
    std::stable_sort(triangles.begin(),triangles.end(),[](const auto &a,const auto &b){return a.depth>b.depth;});
    for(const auto &triangle:triangles)mistVertices.insert(mistVertices.end(),triangle.v,triangle.v+3);
}
// Hitscan wall impacts leave fading bullet marks and a short burst of debris
// tinted by the wall texture's average color. Render-only: no game state,
// own random numbers, cleared on level changes and loads.
struct WallDecal { int line,side,section; float along,dz,born,size; bool flip; };
struct Debris { float x,y,z,vx,vy,vz,born,life,size; std::array<float,3> color; };
struct Scorch { float x,y,born,size;int sector; };
std::vector<WallDecal> wallDecals;std::vector<Debris> debris;std::vector<Scorch> scorches;
unsigned decalLevelSerial;std::minstd_rand impactRandom(1993);
float gameSeconds() {return (leveltime+renderFraction)/(float)TICRATE;}
float randomRange(float low,float high) {return std::uniform_real_distribution<float>(low,high)(impactRandom);}
const std::array<float,3> &averageColor(Image &image) {
    if(image.averaged)return image.average;
    const byte *palette=(const byte*)W_CacheLumpName((char*)"PLAYPAL",PU_CACHE);
    double sum[3]={},count=0;
    for(size_t i=0;i+1<image.pixels.size();i+=2) if(image.pixels[i+1]) {
        for(int c=0;c<3;++c)sum[c]+=palette[image.pixels[i]*3+c]/255.0;
        ++count;
    }
    for(int c=0;c<3;++c)image.average[c]=count?(float)(sum[c]/count):0.5f;
    image.averaged=true;return image.average;
}
struct WallSpan { Point a,b;float bottom,top,anchor;int texture; };
// Section 0 is a one-sided wall (anchored to its floor), 1 an upper wall
// (anchored to the back ceiling, e.g. a door face), 2 a lower wall.
bool wallSpan(const line_t &line,int side,int section,WallSpan &span) {
    if(line.sidenum[side]<0)return false;
    const side_t &s=sides[line.sidenum[side]];
    const sector_t *front=s.sector,*back=side?line.frontsector:line.backsector;
    span.a={units((side?line.v2:line.v1)->x),units((side?line.v2:line.v1)->y)};
    span.b={units((side?line.v1:line.v2)->x),units((side?line.v1:line.v2)->y)};
    if(section==0) {if(back)return false;span.bottom=floorZ(front);span.top=ceilZ(front);span.anchor=span.bottom;span.texture=s.midtexture;}
    else if(!back) return false;
    else if(section==1) {span.bottom=ceilZ(back);span.top=ceilZ(front);span.anchor=span.bottom;span.texture=s.toptexture;}
    else {span.bottom=floorZ(front);span.top=floorZ(back);span.anchor=span.top;span.texture=s.bottomtexture;}
    return span.top>span.bottom;
}
void wallImpact(line_t *line,int side,fixed_t fx,fixed_t fy,fixed_t fz) {
    if(!line||gamestate!=GS_LEVEL)return;
    if(decalLevelSerial!=r_levelserial){wallDecals.clear();debris.clear();scorches.clear();decalLevelSerial=r_levelserial;}
    float x=units(fx),y=units(fy),z=units(fz);
    const sector_t *back=side?line->frontsector:line->backsector;
    int section=!back?0:z>=ceilZ(back)?1:z<=floorZ(back)?2:-1;
    WallSpan span;if(section<0||!wallSpan(*line,side,section,span))return;
    float ex=span.b.x-span.a.x,ey=span.b.y-span.a.y,len=std::hypot(ex,ey);if(len<1)return;
    float along=std::clamp(((x-span.a.x)*ex+(y-span.a.y)*ey)/len,0.0f,len);
    float now=gameSeconds();
    if(wallDecals.size()>=192)wallDecals.erase(wallDecals.begin());
    wallDecals.push_back({int(line-lines),side,section,along,z-span.anchor,now,randomRange(5.0f,7.5f),randomRange(0,1)<0.5f});
    if(span.texture<=0)return;
    Image &image=wallImage(texturetranslation[span.texture]);
    float light=lighting(sides[line->sidenum[side]].sector);
    auto color=averageColor(image);for(float &c:color)c*=light;
    float nx=ey/len,ny=-ex/len,tx=ex/len,ty=ey/len;
    float wx=span.a.x+tx*along+nx*1.5f,wy=span.a.y+ty*along+ny*1.5f;
    if(debris.size()>512)debris.erase(debris.begin(),debris.begin()+(debris.size()-512));
    for(int i=0;i<8;++i) {
        float out=randomRange(50,140),slide=randomRange(-55,55),shade=randomRange(0.7f,1.15f);
        debris.push_back({wx,wy,z,nx*out+tx*slide,ny*out+ty*slide,randomRange(20,120),now,randomRange(0.45f,0.8f),randomRange(0.9f,1.7f),
            {std::min(1.0f,color[0]*shade),std::min(1.0f,color[1]*shade),std::min(1.0f,color[2]*shade)}});
    }
}
void barrelBlast(const mobj_t &thing,float x,float y,float z) {
    if(decalLevelSerial!=r_levelserial){wallDecals.clear();debris.clear();scorches.clear();decalLevelSerial=r_levelserial;}
    float now=gameSeconds();
    if(scorches.size()>=48)scorches.erase(scorches.begin());
    scorches.push_back({x,y,now,randomRange(60,76),int(thing.subsector->sector-sectors)});
    if(debris.size()>480)debris.erase(debris.begin(),debris.begin()+(debris.size()-480));
    for(int i=0;i<32;++i) {
        float angle=randomRange(0,2*doomPi),speed=randomRange(60,220),heat=randomRange(0,1);
        debris.push_back({x,y,z+randomRange(8,32),std::cos(angle)*speed,std::sin(angle)*speed,randomRange(80,260),now,
            randomRange(0.6f,1.3f),randomRange(1.0f,2.2f),{1.0f,0.45f+0.5f*heat,0.08f+0.3f*heat*heat}});
    }
}
void buildImpacts(const Uniforms &camera) {
    for(auto &batch:decalBatches)batch.second.clear();
    particleVertices.clear();
    if(decalLevelSerial!=r_levelserial){wallDecals.clear();debris.clear();scorches.clear();decalLevelSerial=r_levelserial;}
    float now=gameSeconds();
    scorches.erase(std::remove_if(scorches.begin(),scorches.end(),[&](const Scorch &d){return now-d.born>60||now<d.born;}),scorches.end());
    const auto &blast=sprites[SPR_BEXP];
    if(blast.numframes>2&&!scorches.empty()) {
        // Scorches reuse a fireball frame's coverage, clipped to the sector floor.
        int lump=firstspritelump+blast.spriteframes[2].lump[0];Image &image=lumpImage(lump,false);
        for(const auto &d:scorches) {
            const sector_t &sector=sectors[d.sector];if(sector.floorpic==skyflatnum)continue;
            float h=d.size*0.5f,fade=std::clamp((60-(now-d.born))/8,0.0f,1.0f)*0.7f,z=floorZ(&sector)+0.08f;
            std::vector<Point> square={{d.x-h,d.y-h},{d.x+h,d.y-h},{d.x+h,d.y+h},{d.x-h,d.y+h}};
            for(int leaf:sectorFloors[d.sector]) {
                const auto &b=floorBounds[leaf];
                if(b[2]<d.x-h||b[0]>d.x+h||b[3]<d.y-h||b[1]>d.y+h)continue;
                auto polygon=floors[leaf];
                for(size_t n=0;n<square.size()&&!polygon.empty();++n) {
                    Point a=square[n],c=square[(n+1)%square.size()];
                    polygon=clip(polygon,a,{c.x-a.x,c.y-a.y},false);
                }
                auto vertex=[&](Point p){return Vertex{p.x,p.y,z,(p.x-d.x+h)/(2*h)*image.width,(d.y+h-p.y)/(2*h)*image.height,fade,0};};
                for(size_t n=1;n+1<polygon.size();++n)
                    for(Point p:{polygon[0],polygon[n],polygon[n+1]})decalBatches[lump].push_back(vertex(p));
            }
        }
    }
    wallDecals.erase(std::remove_if(wallDecals.begin(),wallDecals.end(),[&](const WallDecal &d){return now-d.born>40||now<d.born;}),wallDecals.end());
    const auto &puff=sprites[SPR_PUFF];
    if(puff.numframes>2) {
        int lump=firstspritelump+puff.spriteframes[2].lump[0];Image &image=lumpImage(lump,false);
        for(const auto &d:wallDecals) {
            WallSpan span;if(!wallSpan(lines[d.line],d.side,d.section,span))continue;
            float ex=span.b.x-span.a.x,ey=span.b.y-span.a.y,len=std::hypot(ex,ey);
            if(ex*(camera.eye[1]-span.a.y)-ey*(camera.eye[0]-span.a.x)>0)continue; // Back of the wall.
            float zc=span.anchor+d.dz,h=d.size*0.5f;
            float u0=std::max(0.0f,d.along-h),u1=std::min(len,d.along+h),z0=std::max(span.bottom,zc-h),z1=std::min(span.top,zc+h);
            if(u1<=u0||z1<=z0)continue;
            float fade=std::clamp((40-(now-d.born))/4,0.0f,1.0f)*0.6f;
            float tx=ex/len,ty=ey/len,nx=ty*0.5f,ny=-tx*0.5f;
            auto texU=[&](float u){float t=(u-(d.along-h))/(2*h);return (d.flip?1-t:t)*image.width;};
            auto texV=[&](float z){return ((zc+h)-z)/(2*h)*image.height;};
            auto vertex=[&](float u,float z){return Vertex{span.a.x+tx*u+nx,span.a.y+ty*u+ny,z,texU(u),texV(z),fade,0};};
            quad(decalBatches[lump],vertex(u0,z1),vertex(u1,z1),vertex(u1,z0),vertex(u0,z0));
        }
    }
    debris.erase(std::remove_if(debris.begin(),debris.end(),[&](const Debris &p){return now-p.born>p.life||now<p.born;}),debris.end());
    for(const auto &p:debris) {
        float t=now-p.born,x=p.x+p.vx*t,y=p.y+p.vy*t,z=p.z+p.vz*t-210*t*t;
        const sector_t *sector=R_PointInSubsector((fixed_t)(x*FRACUNIT),(fixed_t)(y*FRACUNIT))->sector;
        z=std::clamp(z,floorZ(sector)+p.size*0.5f,ceilZ(sector)-p.size*0.5f);
        float alpha=std::clamp((p.life-t)/(p.life*0.4f),0.0f,1.0f),h=p.size*0.5f;
        auto vertex=[&](float sx,float sz){
            Vertex v={x+camera.right[0]*sx+camera.up[0]*sz,y+camera.right[1]*sx+camera.up[1]*sz,z+camera.right[2]*sx+camera.up[2]*sz,0,0,alpha,0};
            v.red=p.color[0];v.green=p.color[1];v.blue=p.color[2];return v;
        };
        quad(particleVertices,vertex(-h,h),vertex(h,h),vertex(h,-h),vertex(-h,-h));
    }
}
// The first wall a ray from (x,y,z) along the unit horizontal (dx,dy) meets
// within length, climbing dz per unit: one-sided lines, and two-sided ones
// where the ray passes above the far ceiling or below the far floor, at the
// current heights. side faces the ray's origin; section is as in wallSpan.
struct WallHit { int line=-1,side=0,section=0; float t=0; };
bool firstSolidWall(float x,float y,float z,float dx,float dy,float dz,float length,WallHit &hit) {
    std::vector<std::pair<float,int>> crossings;
    for(int i=0;i<numlines;++i) {
        const line_t &line=lines[i];
        float ax=units(line.v1->x),ay=units(line.v1->y),ex=units(line.v2->x)-ax,ey=units(line.v2->y)-ay;
        float det=dx*ey-dy*ex;if(std::abs(det)<1e-6f)continue;
        float ox=ax-x,oy=ay-y,t=(ox*ey-oy*ex)/det,u=(ox*dy-oy*dx)/det;
        if(t>0.01f&&t<length&&u>=0&&u<=1)crossings.push_back({t,i});
    }
    std::sort(crossings.begin(),crossings.end());
    for(auto [t,i]:crossings) {
        const line_t &line=lines[i];
        float ax=units(line.v1->x),ay=units(line.v1->y),ex=units(line.v2->x)-ax,ey=units(line.v2->y)-ay;
        int side=ex*(y-ay)-ey*(x-ax)>=0?1:0;
        const sector_t *front=side?line.backsector:line.frontsector,*back=side?line.frontsector:line.backsector;
        if(!front)continue;
        float at=z+dz*t;int section=-1;
        if(!back)section=0;
        else if(at>=ceilZ(back)&&!(front->ceilingpic==skyflatnum&&back->ceilingpic==skyflatnum))section=1;
        else if(at<=floorZ(back))section=2;
        if(section<0)continue;
        hit={i,side,section,t};return true;
    }
    return false;
}
uint32_t hash32(uint32_t x) {x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x;}
float hashUnit(int x,int y,uint32_t seed) {return (hash32((uint32_t)x*73856093u^(uint32_t)y*19349663u^seed)&0xffffff)/16777216.0f;}
float valueNoise(float x,float y,float cell,uint32_t seed) {
    x/=cell;y/=cell;int ix=(int)std::floor(x),iy=(int)std::floor(y);
    float fx=x-ix,fy=y-iy;fx=fx*fx*(3-2*fx);fy=fy*fy*(3-2*fy);
    float a=hashUnit(ix,iy,seed),b=hashUnit(ix+1,iy,seed),c=hashUnit(ix,iy+1,seed),d=hashUnit(ix+1,iy+1,seed);
    float top=a+(b-a)*fx,bottom=c+(d-c)*fx;
    return top+(bottom-top)*fy;
}

// Blood: decals drawn in the world pass and lit like the surface under them.
// Their artwork is generated per WAD from the BLUD sprite's own palette
// indices, ordered dark to bright by how often each is used: one pool shape
// and four splat shapes, one texel per unit like the flats.
constexpr int bloodKeyBase=-0x40000000; // Image keys clear of walls and lumps.
constexpr int bloodPool=0,bloodSplats=4;
std::vector<byte> bloodShades;bool bloodShadesLoaded=false;
const std::vector<byte> &bloodPalette() {
    if(bloodShadesLoaded)return bloodShades;
    bloodShadesLoaded=true;
    if(SPR_BLUD>=numsprites)return bloodShades;
    const byte *palette=(const byte*)W_CacheLumpName((char*)"PLAYPAL",PU_CACHE);
    std::vector<std::pair<float,byte>> pixels;
    const auto &blood=sprites[SPR_BLUD];
    for(int frame=0;frame<blood.numframes;++frame) {
        const Image &image=lumpImage(firstspritelump+blood.spriteframes[frame].lump[0],false);
        for(size_t i=0;i+1<image.pixels.size();i+=2) if(image.pixels[i+1]) {
            const byte *rgb=palette+image.pixels[i]*3;
            pixels.push_back({0.299f*rgb[0]+0.587f*rgb[1]+0.114f*rgb[2],image.pixels[i]});
        }
    }
    std::sort(pixels.begin(),pixels.end());
    for(const auto &pixel:pixels)bloodShades.push_back(pixel.second);
    return bloodShades;
}
Image &bloodImage(int variant) {
    int key=bloodKeyBase+variant;
    auto found=images.find(key);if(found!=images.end())return found->second;
    const auto &shades=bloodPalette();
    bool pool=variant==bloodPool;
    int size=pool?64:32;float center=size*0.5f;
    std::vector<byte> pixels((size_t)size*size*2,0);
    uint32_t seed=hash32(variant*7919u+17u);
    float phase[3];for(int i=0;i<3;++i)phase[i]=hashUnit(i,variant,seed)*2*doomPi;
    auto put=[&](int x,int y,float shade) {
        if(x<0||y<0||x>=size||y>=size)return;
        size_t i=((size_t)y*size+x)*2;
        pixels[i]=shades[std::clamp((int)std::lround(std::clamp(shade,0.0f,1.0f)*(shades.size()-1)),0,(int)shades.size()-1)];
        pixels[i+1]=255;
    };
    for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
        float dx=x+0.5f-center,dy=y+0.5f-center,d=std::hypot(dx,dy),a=std::atan2(dy,dx);
        float edge=pool?size*0.40f*(0.82f+0.10f*std::sin(2*a+phase[0])+0.06f*std::sin(5*a+phase[1])+0.04f*std::sin(9*a+phase[2]))
                       :size*0.30f*(0.70f+0.22f*std::sin(3*a+phase[0])+0.12f*std::sin(7*a+phase[1]));
        edge+=(valueNoise((float)x,(float)y,4,seed)-0.5f)*size*0.08f;
        if(d>=edge)continue;
        float rim=1-d/edge;
        put(x,y,pool?0.12f+0.30f*valueNoise((float)x,(float)y,6,seed^1u)+0.12f*(1-rim)
                    :0.25f+0.45f*std::pow(rim,0.6f)+0.2f*(valueNoise((float)x,(float)y,3,seed^2u)-0.5f));
    }
    // Droplets thrown past the edge.
    int drops=pool?3:6+int(hash32(seed)%5);
    for(int n=0;n<drops;++n) {
        float a=hashUnit(n,1,seed)*2*doomPi,reach=size*(pool?0.42f:0.30f+0.17f*hashUnit(n,2,seed));
        float radius=pool?1.5f+hashUnit(n,3,seed)*1.5f:0.8f+hashUnit(n,3,seed)*1.4f,shade=0.35f+0.25f*hashUnit(n,4,seed);
        float px=center+std::cos(a)*reach,py=center+std::sin(a)*reach;
        for(int y=(int)(py-radius);y<=(int)(py+radius)+1;++y)for(int x=(int)(px-radius);x<=(int)(px+radius)+1;++x)
            if(std::hypot(x+0.5f-px,y+0.5f-py)<radius)put(x,y,pool?shade*0.6f:shade);
    }
    return images.emplace(key,upload(size,size,pixels.data())).first->second;
}
struct BloodFloor { float x,y,born,size,angle;int sector,variant;bool flip; };
struct BloodWall { int line,side,section;float along,dz,born,size;int variant;bool flip; };
struct BloodPool { float x,y,born,size,angle;int sector; };
std::vector<BloodFloor> bloodFloors;std::vector<BloodWall> bloodWalls;std::vector<BloodPool> bloodPools;
std::unordered_map<const mobj_t*,bool> pooledCorpses;
unsigned bloodLevelSerial;
void checkBloodLevel() {
    if(bloodLevelSerial==r_levelserial)return;
    bloodFloors.clear();bloodWalls.clear();bloodPools.clear();pooledCorpses.clear();bloodLevelSerial=r_levelserial;
}
// Hitscan blood: splats land on the floor a little past the hit after their
// fall, and with harder hits the spray reaches a wall close behind.
void bloodHit(mobj_t *target,fixed_t fx,fixed_t fy,fixed_t fz,fixed_t fdx,fixed_t fdy,int damage) {
    (void)target;
    if(!settings.blood||gamestate!=GS_LEVEL||floors.empty()||bloodPalette().empty())return;
    checkBloodLevel();
    float x=units(fx),y=units(fy),z=units(fz),dx=units(fdx),dy=units(fdy),length=std::hypot(dx,dy);
    if(length<0.001f)return;
    dx/=length;dy/=length;
    float now=gameSeconds();
    int count=1+(randomRange(0,30)<damage);
    for(int i=0;i<count;++i) {
        float reach=randomRange(4,36),side=randomRange(-10,10);
        float px=x+dx*reach-dy*side,py=y+dy*reach+dx*side,sx=px-x,sy=py-y,span=std::hypot(sx,sy);
        WallHit wall;
        if(span>0.5f&&firstSolidWall(x,y,z,sx/span,sy/span,0,span,wall))continue;
        const sector_t *sector=R_PointInSubsector((fixed_t)(px*FRACUNIT),(fixed_t)(py*FRACUNIT))->sector;
        float drop=z-floorZ(sector);
        if(sector->floorpic==skyflatnum||drop<0||drop>160)continue;
        float fall=(70+std::sqrt(4900+2450*drop))/1225; // Doom gravity after the 2 unit/tic hop.
        if(bloodFloors.size()>=160)bloodFloors.erase(bloodFloors.begin());
        bloodFloors.push_back({px,py,now+fall,randomRange(14,24)*(damage>=15?1.25f:1.0f),randomRange(0,2*doomPi),
                               int(sector-sectors),1+int(randomRange(0,bloodSplats))%bloodSplats,randomRange(0,1)<0.5f});
    }
    if(randomRange(0,1)>=std::min(0.75f,damage/16.0f))return;
    float sz=z+randomRange(-10,6);
    WallHit hit;WallSpan span;
    if(!firstSolidWall(x,y,sz,dx,dy,0,96,hit)||!wallSpan(lines[hit.line],hit.side,hit.section,span))return;
    float ex=span.b.x-span.a.x,ey=span.b.y-span.a.y,len=std::hypot(ex,ey);if(len<1)return;
    float along=std::clamp(((x+dx*hit.t-span.a.x)*ex+(y+dy*hit.t-span.a.y)*ey)/len,0.0f,len);
    if(bloodWalls.size()>=96)bloodWalls.erase(bloodWalls.begin());
    bloodWalls.push_back({hit.line,hit.side,hit.section,along,sz-span.anchor,now+hit.t/400,randomRange(16,26),
                          1+int(randomRange(0,bloodSplats))%bloodSplats,randomRange(0,1)<0.5f});
}
// Decal depth offset: grows with distance so depth precision never lets the
// surface below show through.
float decalLift(float x,float y,const Uniforms &camera) {return 0.15f+std::hypot(x-camera.eye[0],y-camera.eye[1])*0.0004f;}
// A rotated square of blood clipped to its sector's floor polygons.
void floorBlood(int variant,int sectorIndex,float cx,float cy,float size,float angle,bool flip,float wet,float dark,const Uniforms &camera) {
    const sector_t &sector=sectors[sectorIndex];
    if(sector.floorpic==skyflatnum)return;
    const Image &image=bloodImage(variant);
    float h=size*0.5f,c=std::cos(angle),s=std::sin(angle),reach=h*1.4143f;
    Point across={c,s},up={-s,c};
    auto corner=[&](float u,float v){return Point{cx+across.x*u+up.x*v,cy+across.y*u+up.y*v};};
    std::vector<Point> square={corner(-h,-h),corner(h,-h),corner(h,h),corner(-h,h)};
    float z=floorZ(&sector)+decalLift(cx,cy,camera),light=lighting(&sector);
    unsigned mode=256|sectorMode(&sector);
    auto &out=batches[bloodKeyBase+variant];
    for(int leaf:sectorFloors[sectorIndex]) {
        const auto &b=floorBounds[leaf];
        if(b[2]<cx-reach||b[0]>cx+reach||b[3]<cy-reach||b[1]>cy+reach)continue;
        auto polygon=floors[leaf];
        for(size_t n=0;n<square.size()&&!polygon.empty();++n) {
            Point a=square[n],d=square[(n+1)%square.size()];
            polygon=clip(polygon,a,{d.x-a.x,d.y-a.y},false);
        }
        auto vertex=[&](Point p) {
            float u=((p.x-cx)*across.x+(p.y-cy)*across.y+h)/size,v=(h-((p.x-cx)*up.x+(p.y-cy)*up.y))/size;
            return Vertex{p.x,p.y,z,(flip?1-u:u)*image.width,v*image.height,light,mode,wet,dark,0};
        };
        for(size_t n=1;n+1<polygon.size();++n)
            for(Point p:{polygon[0],polygon[n],polygon[n+1]})out.push_back(vertex(p));
    }
}
// Wet blood steps from glossy to matte, darkening as it dries; values move
// in quarters and eighths at tic-rate ages, never smoothly.
void bloodAge(float age,float wetFor,float dryOver,float &wet,float &dark) {
    float fresh=std::clamp(1-(age-wetFor)/dryOver,0.0f,1.0f);
    wet=settings.bloodShine?std::ceil(fresh*4)/4:0;
    dark=1-std::round((1-fresh)*0.28f*8)/8;
}
void buildBlood(const Uniforms &camera) {
    if(!settings.blood||bloodPalette().empty())return;
    checkBloodLevel();
    float now=gameSeconds();
    // Pools: corpses that came to rest, once each, sized to the body.
    for(auto &entry:pooledCorpses)entry.second=false;
    for(int i=0;i<numsectors;++i)for(mobj_t *thing=sectors[i].thinglist;thing;thing=thing->snext) {
        if(!(thing->flags&MF_CORPSE)||(thing->flags&MF_NOBLOOD))continue;
        auto seen=pooledCorpses.find(thing);
        if(seen!=pooledCorpses.end()) {seen->second=true;continue;}
        if(thing->z>thing->floorz||thing->momx||thing->momy||sectors[i].floorpic==skyflatnum)continue;
        pooledCorpses[thing]=true;
        if(bloodPools.size()>=64)bloodPools.erase(bloodPools.begin());
        bloodPools.push_back({units(thing->x),units(thing->y),now,std::clamp(units(thing->radius)*2.4f,28.0f,64.0f),randomRange(0,2*doomPi),i});
    }
    for(auto it=pooledCorpses.begin();it!=pooledCorpses.end();) it=it->second?std::next(it):pooledCorpses.erase(it);
    for(const auto &pool:bloodPools) {
        float age=now-pool.born;
        // The pool spreads over four seconds in eight steps.
        float grow=0.3f+0.7f*std::min(1.0f,std::floor(age*2)/8);
        float wet,dark;bloodAge(age,12,18,wet,dark);
        floorBlood(bloodPool,pool.sector,pool.x,pool.y,pool.size*grow,pool.angle,false,wet,dark,camera);
    }
    bloodFloors.erase(std::remove_if(bloodFloors.begin(),bloodFloors.end(),[&](const BloodFloor &d){return now-d.born>180;}),bloodFloors.end());
    for(const auto &d:bloodFloors) {
        if(now<d.born)continue;
        float wet,dark;bloodAge(now-d.born,0,8,wet,dark);
        floorBlood(d.variant,d.sector,d.x,d.y,d.size,d.angle,d.flip,wet,dark,camera);
    }
    // Wall splats stay matte; thin spray doesn't glint believably.
    bloodWalls.erase(std::remove_if(bloodWalls.begin(),bloodWalls.end(),[&](const BloodWall &d){return now-d.born>180;}),bloodWalls.end());
    for(const auto &d:bloodWalls) {
        if(now<d.born)continue;
        WallSpan span;if(!wallSpan(lines[d.line],d.side,d.section,span))continue;
        float ex=span.b.x-span.a.x,ey=span.b.y-span.a.y,len=std::hypot(ex,ey);
        if(len<1||ex*(camera.eye[1]-span.a.y)-ey*(camera.eye[0]-span.a.x)>0)continue; // Back of the wall.
        float zc=span.anchor+d.dz,h=d.size*0.5f;
        float u0=std::max(0.0f,d.along-h),u1=std::min(len,d.along+h),z0=std::max(span.bottom,zc-h),z1=std::min(span.top,zc+h);
        if(u1<=u0||z1<=z0)continue;
        float wet,dark;bloodAge(now-d.born,0,8,wet,dark);
        const Image &image=bloodImage(d.variant);
        const line_t &line=lines[d.line];const sector_t *front=sides[line.sidenum[d.side]].sector;
        float light=lighting(front);
        if(std::abs(ey)<0.01f)light=std::max(0.1f,light-0.04f);
        if(std::abs(ex)<0.01f)light=std::min(1.0f,light+0.04f);
        float tx=ex/len,ty=ey/len,lift=decalLift(span.a.x+tx*d.along,span.a.y+ty*d.along,camera)+0.25f,nx=ty*lift,ny=-tx*lift;
        unsigned mode=256|sectorMode(front);
        auto vertex=[&](float u,float z) {
            float t=(u-(d.along-h))/(2*h);
            Vertex v={span.a.x+tx*u+nx,span.a.y+ty*u+ny,z,(d.flip?1-t:t)*image.width,((zc+h)-z)/(2*h)*image.height,light,mode,0,dark,0};
            bakeWallCoordinates(line,d.side,u,v);
            return v;
        };
        quad(batches[bloodKeyBase+d.variant],vertex(u0,z1),vertex(u1,z1),vertex(u1,z0),vertex(u0,z0));
    }
}

// Flashlight silhouettes: the beam's light sits low and right of the eye
// (collectFlashes), so a thing it catches throws its sprite's outline onto
// the wall behind it, enlarged with distance. The
// outline is projected through a grid of cells clipped to the wall's span
// and drawn hard-edged.
struct WallPoint { float s,z,u,v; };
std::vector<WallPoint> clipWallPolygon(const std::vector<WallPoint> &polygon,bool height,float limit,bool above) {
    std::vector<WallPoint> out;if(polygon.empty())return out;
    auto inside=[&](const WallPoint &p){float value=height?p.z:p.s;return above?value>=limit:value<=limit;};
    WallPoint a=polygon.back();
    for(const WallPoint &b:polygon) {
        if(inside(a)!=inside(b)) {
            float va=height?a.z:a.s,vb=height?b.z:b.s,t=(limit-va)/(vb-va);
            out.push_back({a.s+(b.s-a.s)*t,a.z+(b.z-a.z)*t,a.u+(b.u-a.u)*t,a.v+(b.v-a.v)*t});
        }
        if(inside(b))out.push_back(b);
        a=b;
    }
    return out;
}
void flashlightSilhouette(const mobj_t &thing,const Image &image,int lump,bool flip,float x,float y,float z,const Uniforms &camera,const Flash &beam) {
    const float *lamp=beam.position;
    float cz=z+units(thing.height)*0.5f,dx=x-lamp[0],dy=y-lamp[1],horizontal=std::hypot(dx,dy);
    if(horizontal<16)return;
    float ex=x-camera.eye[0],ey=y-camera.eye[1],ez=cz-camera.eye[2],distance=std::sqrt(ex*ex+ey*ey+ez*ez);
    if(distance>700||(ex*camera.forward[0]+ey*camera.forward[1]+ez*camera.forward[2])/distance<beam.direction[3])return;
    float ux=dx/horizontal,uy=dy/horizontal,slope=(cz-lamp[2])/horizontal;
    WallHit hit;WallSpan span;
    if(!firstSolidWall(x,y,cz,ux,uy,slope,384,hit)||!wallSpan(lines[hit.line],hit.side,hit.section,span))return;
    float reach=flashAt(x+ux*(hit.t-2),y+uy*(hit.t-2),cz+slope*(hit.t-2),beam)/beam.strength;
    float opacity=std::min(0.6f,reach*1.4f)*std::clamp(1.25f-lighting(sides[lines[hit.line].sidenum[hit.side]].sector),0.3f,1.0f);
    if(opacity<0.03f)return;
    float sx=span.b.x-span.a.x,sy=span.b.y-span.a.y,len=std::hypot(sx,sy);if(len<1)return;
    float tx=sx/len,ty=sy/len,nx=ty,ny=-tx;
    float planeDistance=(span.a.x-lamp[0])*nx+(span.a.y-lamp[1])*ny;
    float rx=camera.right[0],ry=camera.right[1];
    float left=-(float)image.left,right=left+image.width,top=z+image.top,bottom=top-image.height;
    constexpr int columns=6,rows=8;
    std::array<WallPoint,(columns+1)*(rows+1)> grid;std::array<bool,(columns+1)*(rows+1)> valid;
    for(int j=0;j<=rows;++j)for(int i=0;i<=columns;++i) {
        float across=left+(right-left)*i/columns,height=top-(top-bottom)*j/rows;
        float px=x+rx*across-lamp[0],py=y+ry*across-lamp[1],pz=height-lamp[2];
        float toward=px*nx+py*ny,scale=toward<-1e-3f?planeDistance/toward:-1;
        int k=j*(columns+1)+i;valid[k]=scale>=1;
        float qx=lamp[0]+px*scale,qy=lamp[1]+py*scale;
        grid[k]={(qx-span.a.x)*tx+(qy-span.a.y)*ty,lamp[2]+pz*scale,(flip?1-(float)i/columns:(float)i/columns)*image.width,(float)j/rows*image.height};
    }
    float lift=2*decalLift(span.a.x+tx*len*0.5f,span.a.y+ty*len*0.5f,camera)+0.6f; // Above wall blood.
    auto &out=decalBatches[lump];
    for(int j=0;j<rows;++j)for(int i=0;i<columns;++i) {
        int k[4]={j*(columns+1)+i,j*(columns+1)+i+1,(j+1)*(columns+1)+i+1,(j+1)*(columns+1)+i};
        if(!valid[k[0]]||!valid[k[1]]||!valid[k[2]]||!valid[k[3]])continue;
        std::vector<WallPoint> cell={grid[k[0]],grid[k[1]],grid[k[2]],grid[k[3]]};
        cell=clipWallPolygon(cell,false,0,true);cell=clipWallPolygon(cell,false,len,false);
        cell=clipWallPolygon(cell,true,span.bottom,true);cell=clipWallPolygon(cell,true,span.top,false);
        auto vertex=[&](const WallPoint &p){return Vertex{span.a.x+tx*p.s+nx*lift,span.a.y+ty*p.s+ny*lift,p.z,p.u,p.v,opacity,512};};
        for(size_t n=1;n+1<cell.size();++n)for(size_t m:{size_t(0),n,n+1})out.push_back(vertex(cell[m]));
    }
}

// Heat haze: hot-air volumes over lava and hot damaging floors (40 units
// high, side walls along the sector's edges to cooler neighbors) and small
// boxes over warm flames. Heat per vertex: full at the source, a third at
// the top; present.frag turns it into a raster shimmer.
void buildHeat(const Uniforms &camera) {
    heatVertices.clear();
    if(!settings.heatHaze||camera.effects[2]!=0||sectorMist.size()!=(size_t)numsectors)return;
    auto vertex=[&](float x,float y,float z,float heat) {
        float fade=std::clamp((2400-std::hypot(x-camera.eye[0],y-camera.eye[1]))/600,0.0f,1.0f);
        return Vertex{x,y,z,0,0,heat*fade,0};
    };
    constexpr float rise=40;
    for(int i=0;i<numsubsectors;++i) {
        const auto &poly=floors[i];if(poly.size()<3)continue;
        const sector_t &sector=*subsectors[i].sector;
        if(!sectorMist[&sector-sectors].hot||sector.floorpic==skyflatnum)continue;
        const auto &b=floorBounds[i];
        if(camera.eye[0]<b[0]-2400||camera.eye[0]>b[2]+2400||camera.eye[1]<b[1]-2400||camera.eye[1]>b[3]+2400)continue;
        float top=std::min(ceilZ(&sector),floorZ(&sector)+rise);
        if(top<=floorZ(&sector)+2)continue;
        for(size_t n=1;n+1<poly.size();++n)for(Point p:{poly[0],poly[n],poly[n+1]})heatVertices.push_back(vertex(p.x,p.y,top,0.35f));
    }
    for(int i=0;i<numlines;++i) {
        const line_t &line=lines[i];
        for(int side=0;side<2;++side) {
            const sector_t *own=side?line.backsector:line.frontsector,*other=side?line.frontsector:line.backsector;
            if(!own||!sectorMist[own-sectors].hot||own->floorpic==skyflatnum||(other&&sectorMist[other-sectors].hot))continue;
            float ax=units(line.v1->x),ay=units(line.v1->y),bx=units(line.v2->x),by=units(line.v2->y);
            if(std::hypot((ax+bx)/2-camera.eye[0],(ay+by)/2-camera.eye[1])>2400)continue;
            float floor=floorZ(own),top=std::min(ceilZ(own),floor+rise);
            if(top<=floor+2)continue;
            quad(heatVertices,vertex(ax,ay,floor,1),vertex(bx,by,floor,1),vertex(bx,by,top,0.35f),vertex(ax,ay,top,0.35f));
        }
    }
    for(int i=0;i<numsectors;++i)for(mobj_t *thing=sectors[i].thinglist;thing;thing=thing->snext) {
        if(!decorationLight(*thing))continue;
        BakeLight flame=decorationSource(*thing);
        if(std::hypot(flame.x-camera.eye[0],flame.y-camera.eye[1])>1200)continue;
        if(!(flame.color[0]>flame.color[1]*1.15f&&flame.color[0]>flame.color[2]*1.5f))continue;
        float h=10,z0=flame.z-12,z1=flame.z+36;
        Point c[4]={{flame.x-h,flame.y-h},{flame.x+h,flame.y-h},{flame.x+h,flame.y+h},{flame.x-h,flame.y+h}};
        for(int n=0;n<4;++n) {
            Point a=c[n],b=c[(n+1)%4];
            quad(heatVertices,vertex(a.x,a.y,z0,0.8f),vertex(b.x,b.y,z0,0.8f),vertex(b.x,b.y,z1,0),vertex(a.x,a.y,z1,0));
        }
        quad(heatVertices,vertex(c[0].x,c[0].y,z1,0),vertex(c[1].x,c[1].y,z1,0),vertex(c[2].x,c[2].y,z1,0),vertex(c[3].x,c[3].y,z1,0));
    }
}

// Eye adaptation: what the eye has settled to follows the light around and
// ahead of the player, quickly toward brighter light and slowly toward
// darker. Exposure is the square root of target over settled light, in
// sixteenths like Doom's light levels, updated once per tic. Each sector's
// light is held at its recent peak so flickering and strobing sectors don't
// pump the exposure.
float adaptedLight=-1,exposure=1;int adaptTic=-1;unsigned adaptLevelSerial;
std::vector<std::pair<float,int>> steadyLights;
float steadyLight(const sector_t *sector) {
    auto &held=steadyLights[sector-sectors];
    float now=lighting(sector),value=held.second<0?now:std::max(now,held.first-(leveltime-held.second)*0.01f);
    held={value,leveltime};
    return value;
}
float viewLight(const Uniforms &camera) {
    auto lightAt=[&](float x,float y,const sector_t *sector) {return sunLightAt(x,y,sector,seamLightAt(x,y,sector,steadyLight(sector)));};
    float own=lightAt(camera.eye[0],camera.eye[1],players[displayplayer].mo->subsector->sector);
    float fx=camera.forward[0],fy=camera.forward[1],flat=std::hypot(fx,fy);
    if(flat<0.2f)return own;
    fx/=flat;fy/=flat;
    WallHit hit;
    float reach=firstSolidWall(camera.eye[0],camera.eye[1],camera.eye[2],fx,fy,camera.forward[2]/flat,512,hit)?hit.t:512;
    float sum=0;int count=0;
    for(float d:{64.0f,160.0f,320.0f}) {
        float t=std::min(d,reach-8);if(t<8)break;
        float px=camera.eye[0]+fx*t,py=camera.eye[1]+fy*t;
        sum+=lightAt(px,py,R_PointInSubsector((fixed_t)(px*FRACUNIT),(fixed_t)(py*FRACUNIT))->sector);++count;
    }
    return count?own*0.4f+sum/count*0.6f:own;
}
void updateExposure(Uniforms &camera) {
    if(!settings.eyeAdaptation||camera.effects[2]!=0||!players[displayplayer].mo) {adaptedLight=-1;exposure=1;return;}
    if(adaptLevelSerial!=r_levelserial||steadyLights.size()!=(size_t)numsectors) {
        adaptedLight=-1;adaptLevelSerial=r_levelserial;steadyLights.assign(numsectors,{0.0f,-1});
    }
    if(leveltime!=adaptTic) {
        float target=viewLight(camera);
        if(adaptedLight<0)adaptedLight=target;
        else {
            float dt=std::clamp((leveltime-adaptTic)/(float)TICRATE,0.0f,0.5f),tau=target>adaptedLight?0.5f:1.6f;
            adaptedLight+=(target-adaptedLight)*(1-std::exp(-dt/tau));
        }
        adaptTic=leveltime;
        float ratio=std::sqrt(target/std::max(adaptedLight,0.05f));
        exposure=1+std::round((std::clamp(ratio,0.6f,1.5f)-1)*16)/16;
    }
    camera.fx[0]=exposure;
}

// Splashes: per tic, things in liquid sectors are watched for landing,
// projectiles exploding at the surface, and wading. Each event throws
// droplets in the liquid's average flat color and an expanding ring that
// also pushes the mirror image aside.
struct SplashWatch { float z;bool missile,seen;int ripple; };
struct Ring { float x,y,born,speed,life,strength;int sector; };
std::unordered_map<const mobj_t*,SplashWatch> splashWatch;
std::vector<Ring> rings;
unsigned splashLevelSerial;
void splash(float x,float y,int sector,float size,int drops) {
    float now=leveltime/(float)TICRATE;
    if(rings.size()>=24)rings.erase(rings.begin());
    rings.push_back({x,y,now,30+size*0.6f,0.8f+size*0.01f,std::min(1.0f,size/40),sector});
    if(decalLevelSerial!=r_levelserial){wallDecals.clear();debris.clear();scorches.clear();decalLevelSerial=r_levelserial;}
    const auto &color=sectorMist[sector].color;float light=lighting(&sectors[sector]);
    float floor=units(sectors[sector].floorheight),lift=std::clamp(size/40,0.6f,1.4f);
    if(debris.size()>480)debris.erase(debris.begin(),debris.begin()+(debris.size()-480));
    for(int i=0;i<drops;++i) {
        float angle=randomRange(0,2*doomPi),out=randomRange(20,80),shade=randomRange(0.8f,1.3f);
        debris.push_back({x,y,floor+1,std::cos(angle)*out,std::sin(angle)*out,randomRange(90,220)*lift,now,randomRange(0.35f,0.6f),randomRange(1.0f,1.8f),
            {std::min(1.0f,color[0]*shade*light*1.3f),std::min(1.0f,color[1]*shade*light*1.3f),std::min(1.0f,color[2]*shade*light*1.3f)}});
    }
}
void watchSplashes() {
    if(!settings.splashes||levelSerial!=r_levelserial||sectorMist.size()!=(size_t)numsectors)return;
    if(splashLevelSerial!=r_levelserial) {splashWatch.clear();rings.clear();splashLevelSerial=r_levelserial;}
    for(auto &entry:splashWatch)entry.second.seen=false;
    for(int i=0;i<numsectors;++i) {
        if(!sectorMist[i].liquid||sectors[i].floorpic==skyflatnum)continue;
        float floor=units(sectors[i].floorheight);
        for(mobj_t *thing=sectors[i].thinglist;thing;thing=thing->snext) {
            bool missile=thing->flags&MF_MISSILE;
            if((thing->flags&MF_NOBLOCKMAP)&&!missile)continue;
            float x=units(thing->x),y=units(thing->y),z=units(thing->z);
            bool surface=z<=floor+1;
            auto found=splashWatch.find(thing);
            if(found==splashWatch.end()) {splashWatch[thing]={z,missile,true,leveltime};continue;}
            SplashWatch &watch=found->second;watch.seen=true;
            float size=units(thing->radius)*2;
            if(watch.missile&&!missile&&z<=floor+8)splash(x,y,i,48,12);
            else if(surface&&watch.z>floor+3) {
                splash(x,y,i,std::clamp(size*0.5f+(watch.z-z)*0.8f,12.0f,64.0f),std::clamp(int(4+(watch.z-z)*0.25f),4,16));
                watch.ripple=leveltime;
            } else if(surface&&std::hypot(units(thing->momx),units(thing->momy))>1&&leveltime-watch.ripple>=12) {
                splash(x,y,i,size*0.4f,2);watch.ripple=leveltime;
            }
            watch.z=z;watch.missile=missile;
        }
    }
    for(auto it=splashWatch.begin();it!=splashWatch.end();) it=it->second.seen?std::next(it):splashWatch.erase(it);
}
// Rings: twelve-sided, growing at the tic rate, in the liquid's color lifted
// toward white; the four youngest near the eye also reach the reflection.
void buildRings(Uniforms &camera) {
    if(!settings.splashes||splashLevelSerial!=r_levelserial) {rings.clear();return;}
    float now=gameSeconds();
    rings.erase(std::remove_if(rings.begin(),rings.end(),[&](const Ring &r){return now-r.born>r.life||now<r.born-0.1f;}),rings.end());
    int slot=0;
    for(auto it=rings.rbegin();it!=rings.rend();++it) {
        const Ring &ring=*it;
        float age=std::max(0.0f,std::floor((now-ring.born)*TICRATE)/TICRATE),radius=4+ring.speed*age,fade=1-age/ring.life;
        const sector_t &sector=sectors[ring.sector];
        float z=floorZ(&sector)+decalLift(ring.x,ring.y,camera)+0.1f,light=lighting(&sector),alpha=0.55f*ring.strength*fade;
        const auto &color=sectorMist[ring.sector].color;
        std::array<float,3> tint;for(int c=0;c<3;++c)tint[c]=std::min(1.0f,color[c]*1.6f+0.08f)*light;
        auto vertex=[&](float angle,float r) {
            Vertex v={ring.x+std::cos(angle)*r,ring.y+std::sin(angle)*r,z,0,0,alpha,0};
            v.red=tint[0];v.green=tint[1];v.blue=tint[2];return v;
        };
        for(int k=0;k<12;++k) {
            float a0=k*2*doomPi/12,a1=(k+1)*2*doomPi/12;
            quad(particleVertices,vertex(a0,radius-1.2f),vertex(a0,radius+1.2f),vertex(a1,radius+1.2f),vertex(a1,radius-1.2f));
        }
        if(slot<4&&std::hypot(ring.x-camera.eye[0],ring.y-camera.eye[1])<1500) {
            float *uniform=camera.ripple[slot++];
            uniform[0]=ring.x;uniform[1]=ring.y;uniform[2]=radius;uniform[3]=ring.strength*fade;
        }
    }
}

// Dust motes: one speck in about half of the 48-unit cells around the eye,
// world-anchored with a slow tic-stepped drift, shown only where the
// flashlight beam or an indoor sunbeam catches it: a point is sunlit when the
// floor point its sun ray leaves from is (the floor sun map; the sun stands
// 40 degrees high). Each is one Doom pixel across at its distance.
void buildDust(const Uniforms &camera) {
    if(!settings.dust||camera.effects[2]!=0||seamCells.empty())return;
    const Flash *beam=flashlightOn&&flashes.count&&flashes.lights[0].direction[3]>0?&flashes.lights[0]:nullptr;
    bool sun=settings.sun&&sunLevel>0&&!contactCells.empty();
    if(!beam&&!sun)return;
    constexpr float cell=48;
    float t=leveltime/(float)TICRATE;
    int gx0=(int)std::floor(camera.eye[0]/cell),gy0=(int)std::floor(camera.eye[1]/cell),gz0=(int)std::floor(camera.eye[2]/cell);
    for(int gz=gz0-2;gz<=gz0+2;++gz)for(int gy=gy0-6;gy<=gy0+6;++gy)for(int gx=gx0-6;gx<=gx0+6;++gx) {
        uint32_t seed=(uint32_t)gz*83492791u;
        if(hashUnit(gx,gy,seed)<0.5f)continue;
        float phase=hashUnit(gx,gy,seed^7u)*2*doomPi;
        float p[3]={(gx+hashUnit(gx,gy,seed^1u))*cell+6*std::sin(t*0.31f+phase),(gy+hashUnit(gx,gy,seed^2u))*cell+6*std::cos(t*0.27f+phase),
                    (gz+hashUnit(gx,gy,seed^3u))*cell+4*std::sin(t*0.19f+phase*1.7f)};
        float dx=p[0]-camera.eye[0],dy=p[1]-camera.eye[1],dz=p[2]-camera.eye[2],distance=std::sqrt(dx*dx+dy*dy+dz*dz);
        if(distance<24||distance>320)continue;
        int cx=(int)std::floor((p[0]-mapOrigin[0])/mapCell),cy=(int)std::floor((p[1]-mapOrigin[1])/mapCell);
        if(cx<0||cy<0||cx>=mapWidth||cy>=mapHeight)continue;
        int own=seamCells[(size_t)cy*mapWidth+cx].own;
        if(own==noSector)continue;
        const sector_t &sector=sectors[own];
        float floor=floorZ(&sector);
        if(p[2]<floor+2||p[2]>ceilZ(&sector)-2)continue;
        float bright=0,color[3]={};
        if(beam) {
            float amount=flashAt(p[0],p[1],p[2],*beam)/beam->strength;
            bright+=amount;for(int c=0;c<3;++c)color[c]+=beam->color[c]*amount;
        }
        if(sun&&(sector.ceilingpic!=skyflatnum||enclosure(&sector)>0.5f)) {
            float lift=(p[2]-floor)/0.8391f;
            int fx=std::clamp((int)std::floor((p[0]-sunAzimuth[0]*lift-mapOrigin[0])/mapCell),0,mapWidth-1);
            int fy=std::clamp((int)std::floor((p[1]-sunAzimuth[1]*lift-mapOrigin[1])/mapCell),0,mapHeight-1);
            float visible=contactCells[4*((size_t)fy*mapWidth+fx)+2]/255.0f*0.6f;
            bright+=visible;for(int c=0;c<3;++c)color[c]+=sunTint[c]*visible;
        }
        if(bright<0.05f)continue;
        float glint=std::min(1.0f,0.55f+0.5f*bright),h=std::max(0.25f,distance/(100*camera.projection[1]))*0.5f,alpha=std::min(0.75f,bright*0.9f);
        auto vertex=[&](float sx,float sz) {
            Vertex v={p[0]+camera.right[0]*sx+camera.up[0]*sz,p[1]+camera.right[1]*sx+camera.up[1]*sz,p[2]+camera.right[2]*sx+camera.up[2]*sz,0,0,alpha,0};
            v.red=std::min(1.0f,color[0]/bright*glint);v.green=std::min(1.0f,color[1]/bright*glint);v.blue=std::min(1.0f,color[2]/bright*glint);
            return v;
        };
        quad(particleVertices,vertex(-h,h),vertex(h,h),vertex(h,-h),vertex(-h,-h));
    }
}

// Door light spill: sectors closed at level start are doors. While one is
// open, its brighter side lights the darker side through the opening: a
// light just outside each face on the dark side, as strong as the light
// difference times how far the door is open (in eighths), tinted by the
// bright side's floor. The four nearest join the dynamic lights.
struct DoorPortal { int door;std::vector<std::pair<int,int>> faces;float fullOpen; }; // faces: line, neighbor sector
std::vector<DoorPortal> doorPortals;
void findDoors() {
    doorPortals.clear();
    for(int i=0;i<numsectors;++i) {
        const sector_t &sector=sectors[i];
        if(sector.ceilingheight>sector.floorheight)continue;
        DoorPortal door={i,{},0};float lowest=1e9f;
        for(int j=0;j<sector.linecount;++j) {
            const line_t &line=*sector.lines[j];
            const sector_t *other=line.frontsector==&sector?line.backsector:line.frontsector;
            if(!other||other==&sector)continue;
            door.faces.push_back({int(&line-lines),int(other-sectors)});lowest=std::min(lowest,units(other->ceilingheight));
        }
        if(door.faces.size()<2)continue;
        door.fullOpen=std::max(8.0f,lowest-4-units(sector.floorheight));
        doorPortals.push_back(std::move(door));
    }
}
void doorSpill(const Uniforms &camera) {
    if(!settings.doorLight)return;
    struct Spill { float x,y,z,radius,strength,distance;std::array<float,3> color; };
    std::vector<Spill> spills;
    for(const auto &door:doorPortals) {
        const sector_t &sector=sectors[door.door];
        float open=ceilZ(&sector)-floorZ(&sector);
        if(open<2)continue;
        float fraction=std::ceil(std::clamp(open/door.fullOpen,0.0f,1.0f)*8)/8;
        for(auto [index,dark]:door.faces) {
            int bright=-1;
            for(auto [other,neighbor]:door.faces)
                if(neighbor!=dark&&(bright<0||lighting(&sectors[neighbor])>lighting(&sectors[bright])))bright=neighbor;
            if(bright<0)continue;
            float difference=lighting(&sectors[bright])-lighting(&sectors[dark]);
            if(difference<0.06f)continue;
            const line_t &line=lines[index];
            float ax=units(line.v1->x),ay=units(line.v1->y),ex=units(line.v2->x)-ax,ey=units(line.v2->y)-ay,len=std::hypot(ex,ey);
            if(len<1)continue;
            // The front side's normal is the right of v1->v2.
            float side=line.frontsector==&sectors[dark]?1.0f:-1.0f,nx=ey/len*side,ny=-ex/len*side;
            float x=ax+ex*0.5f+nx*20,y=ay+ey*0.5f+ny*20,distance=std::hypot(x-camera.eye[0],y-camera.eye[1]);
            if(distance>1400)continue;
            int pic=sectors[bright].floorpic;
            std::array<float,3> color={1,1,1};
            if(pic!=skyflatnum) {
                const auto &average=averageColor(lumpImage(firstflat+flattranslation[pic],true));
                float peak=std::max({average[0],average[1],average[2],0.01f});
                for(int c=0;c<3;++c)color[c]=0.5f+0.5f*average[c]/peak;
            }
            spills.push_back({x,y,floorZ(&sector)+std::min(open,96.0f)*0.55f,std::clamp(len*2+96,128.0f,288.0f),difference*fraction*1.8f,distance,color});
        }
    }
    std::sort(spills.begin(),spills.end(),[](const Spill &a,const Spill &b){return a.distance<b.distance;});
    for(size_t n=0;n<std::min(size_t(4),spills.size());++n) {
        const Spill &s=spills[n];
        appendLight(s.x,s.y,s.z,s.radius,s.strength,s.color,nullptr,0.75f);
    }
}

// The light bake's inputs: 1 emissive textures, 2 bake-only lights.
int lightBakeInputs() {return (settings.emissive?1:0)|(settings.bakeOnlyLights?2:0);}
// Bake-only lights: glowing liquid floors and ceilings (lava, nukage, slime,
// magma, or flats that mostly glow) light as pools; lamps and panels stay
// point lights.
bool poolSurface(int sector,bool ceiling) {
    int pic=ceiling?sectors[sector].ceilingpic:sectors[sector].floorpic;
    if(pic==skyflatnum)return false;
    int lump=firstflat+flattranslation[pic];
    const Image &image=lumpImage(lump,true);
    if(image.emissionWeight<=0)return false;
    char name[9];memcpy(name,lumpinfo[lump].name,8);name[8]=0;
    int kind=doom_emissive_kind(name,1);
    return kind==DOOM_EMISSIVE_LAVA||kind==DOOM_EMISSIVE_SLIME||kind==DOOM_EMISSIVE_MAGMA||image.emissionCoverage>=0.4f;
}
// One area light per pool: neighboring sectors whose floor (or ceiling)
// glows in the same flat at the same height join. Strength and reach grow
// gently with the pool's size, like merged tile lights did. The 0.68 keeps
// the total near what the tile lights gave (E1M1: Doom +12%, Freedoom -10%),
// as the glow now spills further over rims.
void buildPoolAreas() {
    bakeAreas.clear();
    std::vector<float> surface,weight; // Per area: size, and emission weight times size.
    for(int ceiling=0;ceiling<2;++ceiling) {
        std::vector<int> parent(numsectors);
        for(int i=0;i<numsectors;++i)parent[i]=i;
        std::function<int(int)> root=[&](int i) {return parent[i]==i?i:parent[i]=root(parent[i]);};
        std::vector<char> pool(numsectors);
        for(int i=0;i<numsectors;++i)pool[i]=poolSurface(i,ceiling);
        for(int i=0;i<numlines;++i) {
            const line_t &line=lines[i];
            if(!line.frontsector||!line.backsector)continue;
            int a=(int)(line.frontsector-sectors),b=(int)(line.backsector-sectors);
            if(!pool[a]||!pool[b])continue;
            const sector_t &sa=sectors[a],&sb=sectors[b];
            bool same=ceiling?sa.ceilingpic==sb.ceilingpic&&sa.ceilingheight==sb.ceilingheight
                             :sa.floorpic==sb.floorpic&&sa.floorheight==sb.floorheight;
            if(same)parent[root(a)]=root(b);
        }
        std::map<int,size_t> areaOf;
        for(int i=0;i<numsectors;++i) {
            if(!pool[i])continue;
            auto found=areaOf.find(root(i));
            if(found==areaOf.end()) {
                found=areaOf.emplace(root(i),bakeAreas.size()).first;
                BakeArea area;area.ceiling=ceiling;
                area.z=ceiling?bakeMap.sectors[i].ceiling:bakeMap.sectors[i].floor;
                area.clearance=1e9f;
                area.color[0]=area.color[1]=area.color[2]=0;
                bakeAreas.push_back(area);surface.push_back(0);weight.push_back(0);
            }
            size_t index=found->second;BakeArea &area=bakeAreas[index];
            // Rays test against a point up to 48 units off the surface, inside the room.
            area.clearance=std::min(area.clearance,std::clamp(bakeMap.sectors[i].ceiling-bakeMap.sectors[i].floor-2,0.0f,48.0f));
            const Image &image=lumpImage(firstflat+flattranslation[ceiling?sectors[i].ceilingpic:sectors[i].floorpic],true);
            for(int leaf:sectorFloors[i]) {
                const auto &poly=floors[leaf];if(poly.size()<3)continue;
                std::vector<std::array<float,2>> piece;float twice=0;
                for(size_t n=0;n<poly.size();++n) {
                    const Point &p=poly[n],&q=poly[(n+1)%poly.size()];
                    piece.push_back({p.x,p.y});twice+=p.x*q.y-q.x*p.y;
                    area.low[0]=std::min(area.low[0],p.x);area.low[1]=std::min(area.low[1],p.y);
                    area.high[0]=std::max(area.high[0],p.x);area.high[1]=std::max(area.high[1],p.y);
                }
                float size=std::fabs(twice)*0.5f;
                for(int c=0;c<3;++c)area.color[c]+=image.emissionColor[c]*size;
                surface[index]+=size;weight[index]+=image.emissionWeight*size;
                area.pieces.push_back(std::move(piece));
            }
        }
        for(size_t n=0;n<bakeAreas.size();++n) {
            BakeArea &area=bakeAreas[n];
            if(area.ceiling!=(bool)ceiling||surface[n]<=0)continue;
            for(float &c:area.color)c/=surface[n];
            float tiles=surface[n]/4096;
            area.strength=0.68f*1.65f*weight[n]/surface[n]*std::clamp(0.6f+0.2f*std::sqrt(tiles),0.75f,1.5f);
            area.radius=256+std::clamp(std::sqrt(surface[n])*0.125f,0.0f,96.0f);
        }
    }
    bakeAreas.erase(std::remove_if(bakeAreas.begin(),bakeAreas.end(),[](const BakeArea &a){return a.pieces.empty();}),bakeAreas.end());
}
// Bake-only direction texel: where the static light comes from in the
// surface's tangent frame (rg; walls: along the side and up, floors and
// ceilings: map x and y), how much of it comes from that one direction (b),
// and the strongest flicker group with its share in 1/15 steps
// (a: group*16+share). Without a flicker group the low nibble holds the
// share of light from a pool the surface lies in (its sheen; see world.frag).
// Keep the frame equivalent to world.frag.
void encodeDirection(const float normal[3],const float towards[3],float total,const float groups[],float sheen,uint8_t *out) {
    out[0]=out[1]=128;out[2]=out[3]=0;
    float length=std::sqrt(towards[0]*towards[0]+towards[1]*towards[1]+towards[2]*towards[2]);
    if(total<=0||length<=1e-6f)return;
    bool flat=std::fabs(normal[2])>0.5f;
    float t[3]={flat?1.0f:-normal[1],flat?0.0f:normal[0],0},b[3]={0,flat?1.0f:0.0f,flat?0.0f:1.0f};
    float x=(towards[0]*t[0]+towards[1]*t[1]+towards[2]*t[2])/length,y=(towards[0]*b[0]+towards[1]*b[1]+towards[2]*b[2])/length;
    out[0]=(uint8_t)std::lround(std::clamp(x*0.5f+0.5f,0.0f,1.0f)*255);
    out[1]=(uint8_t)std::lround(std::clamp(y*0.5f+0.5f,0.0f,1.0f)*255);
    out[2]=(uint8_t)std::lround(std::clamp(length/total,0.0f,1.0f)*255);
    int best=0;
    for(int g=1;g<flickerGroups;++g)if(groups[g]>0&&groups[g]>(best?groups[best]:0))best=g;
    if(best)out[3]=(uint8_t)(best*16+std::lround(std::clamp(groups[best]/total,0.0f,1.0f)*15));
    else out[3]=(uint8_t)std::lround(std::clamp(sheen/total,0.0f,1.0f)*15);
}
// The static light at a surface point, unflickered, into out (rgb at half
// scale, in 1/32 steps) and, if given, its bake-only direction texel.
void gatherStatic(const BakeMap &map,int sector,float x,float y,float z,const float normal[3],uint8_t *out,uint8_t *direction) {
    int points=(int)bakeSources.size();
    float sum[3]={},towards[3]={},total=0,groups[flickerGroups]={},sheen=0;
    for(int n:bakeGridLights(x,y)) {
        float to[3];bool inside=false;
        const float *color=n<points?bakeSources[n].color:bakeAreas[n-points].color;
        float amount=n<points?addBakedLight(map,bakeSources[n],sector,x,y,z,normal,sum,to)
                             :addBakedArea(map,bakeAreas[n-points],sector,x,y,z,normal,sum,to,&inside);
        if(!direction||amount<=0)continue;
        float weight=amount*luminance(color);
        for(int c=0;c<3;++c)towards[c]+=to[c]*weight;
        total+=weight;
        if(n<points)groups[sourceGroups[n]]+=weight;
        // The pool's own surface (facing the way the pool glows).
        else if(inside&&normal[2]*(bakeAreas[n-points].ceiling?-1:1)>0.5f)sheen+=weight;
    }
    for(int c=0;c<3;++c)out[c]=(uint8_t)std::min(255L,std::lround(std::round(sum[c]*32)/32*0.5f*255));
    if(direction)encodeDirection(normal,towards,total,groups,sheen,direction);
}
// Static light for a floor/ceiling map cell (only: with directions).
void lightCell(const BakeMap &map,size_t i,bool only) {
    static const float up[3]={0,0,1},down[3]={0,0,-1};
    int sector=seamCells[i].own;
    if(sector==noSector)return;
    float px=mapOrigin[0]+(i%mapWidth+0.5f)*mapCell,py=mapOrigin[1]+(i/mapWidth+0.5f)*mapCell;
    const BakeSector &s=map.sectors[sector];
    size_t layer=(size_t)mapWidth*mapHeight;
    if(sectors[sector].floorpic!=skyflatnum)
        gatherStatic(map,sector,px,py,s.floor+1,up,flatCell(floorLight,i),only?&flatDirectionCells[4*i]:nullptr);
    if(sectors[sector].ceilingpic!=skyflatnum)
        gatherStatic(map,sector,px,py,s.ceiling-1,down,flatCell(ceilingLight,i),only?&flatDirectionCells[4*(layer+i)]:nullptr);
}
// Static light for a wall strip's atlas texels.
void lightStrip(const BakeMap &map,const BakeStrip &strip,bool only) {
    const float normal[3]={strip.dy,-strip.dx,0};
    forStripTexels(strip,[&](size_t i,float x,float y,float z) {
        size_t texel=i/atlasStride*atlasWidth+i%atlasStride;
        gatherStatic(map,strip.sector,x,y,z,normal,&wallBakeCells[4*i],only?&wallDirectionCells[4*texel]:nullptr);
    });
}
// Static lights, baked into the floor/ceiling map and the wall atlas rgb:
// fullbright decorations and (with emissive textures) merged surface lights,
// unflickered, with the sector heights in bakeMap. Values step by 1/32, like the
// 32 light rows of COLORMAP. Bake-only lights also light liquid pools as
// areas and record each texel's light direction and flicker group.
void bakeLights() {
    finishRelight();
    lightBakeKey=lightBakeInputs();++lightBakeSerial;
    bool only=lightBakeKey&2;
    if(flatLightCells.empty())return;
    uint64_t start=SDL_GetTicksNS();
    std::vector<BakeLight> &sources=bakeSources;sources.clear();sourceGroups.clear();bakeAreas.clear();
    for(int i=0;i<numsectors;++i)for(mobj_t *thing=sectors[i].thinglist;thing;thing=thing->snext)
        if(decorationLight(*thing)) {sources.push_back(decorationSource(*thing));sourceGroups.push_back((uint8_t)flickerGroup(*thing));}
    if(settings.emissive) {
        if(only)buildPoolAreas();
        std::vector<SurfaceLight> points;
        for(const auto &light:surfaceLights)
            if(!only||light.key[0]!=0||!poolSurface(light.key[1],light.key[2]))points.push_back(light);
        for(const auto &light:mergeSurfaceLights(points)) {
            sources.push_back({light.x,light.y,light.z,light.radius,light.strength,0.35f,{light.color[0],light.color[1],light.color[2]}});
            sourceGroups.push_back(0);
        }
    }
    int points=(int)sources.size();
    bakeGridWidth=(int)std::ceil(mapWidth*mapCell/bakeGridCell);bakeGridHeight=(int)std::ceil(mapHeight*mapCell/bakeGridCell);
    auto gridX=[&](float x){return std::clamp((int)std::floor((x-mapOrigin[0])/bakeGridCell),0,bakeGridWidth-1);};
    auto gridY=[&](float y){return std::clamp((int)std::floor((y-mapOrigin[1])/bakeGridCell),0,bakeGridHeight-1);};
    bakeGrid.assign((size_t)bakeGridWidth*bakeGridHeight,{});
    auto list=[&](int n,float x0,float y0,float x1,float y1) {
        for(int y=gridY(y0);y<=gridY(y1);++y)for(int x=gridX(x0);x<=gridX(x1);++x)bakeGrid[(size_t)y*bakeGridWidth+x].push_back(n);
    };
    for(int n=0;n<points;++n) {
        const BakeLight &light=sources[n];
        list(n,light.x-light.radius,light.y-light.radius,light.x+light.radius,light.y+light.radius);
    }
    for(size_t n=0;n<bakeAreas.size();++n) {
        const BakeArea &area=bakeAreas[n];
        list(points+(int)n,area.low[0]-area.radius,area.low[1]-area.radius,area.high[0]+area.radius,area.high[1]+area.radius);
    }
    if(only) {
        wallDirectionCells.assign((size_t)atlasWidth*atlasHeight*4,0);
        flatDirectionCells.assign((size_t)mapWidth*mapHeight*2*4,0);
    } else {wallDirectionCells.clear();flatDirectionCells.clear();}
    parallelFor(mapHeight,[&](int y) {for(int x=0;x<mapWidth;++x)lightCell(bakeMap,(size_t)y*mapWidth+x,only);});
    parallelFor((int)bakeOrder.size(),[&](int n) {lightStrip(bakeMap,bakeStrips[bakeOrder[n]],only);});
    uploadFlatLight();
    uploadWallBake();
    wallDirectionTexture=nullptr;flatDirectionTexture=nullptr;
    if(only) {
        wallDirectionTexture=gpuCreateTexture(GpuFormat::RGBA8,atlasWidth,atlasHeight,wallDirectionCells.data(),atlasWidth*4);
        flatDirectionTexture=gpuCreateTexture(GpuFormat::RGBA8,mapWidth,mapHeight*2,flatDirectionCells.data(),mapWidth*4);
        if(!wallDirectionTexture||!flatDirectionTexture)I_Error((char*)"Could not allocate surface maps");
    }
    fprintf(stderr,"3D lights: %zu static lights%s baked in %.0f ms.\n",sources.size(),
        only?(", "+std::to_string(bakeAreas.size())+" pools, bake-only").c_str():"",(SDL_GetTicksNS()-start)/1e6);
}
// Which inputs bounce light gathers: 1 always, 2 the sun, 4 baked lights,
// 8 bake-only lights (pools as areas).
int bounceInputs() {
    bool lights=lightBakeKey==lightBakeInputs();
    return 1|(settings.sun&&sunLevel>0?2:0)|(lights?4:0)|(lights&&(lightBakeKey&2)?8:0);
}
// Bounce light: each sample averages what 16 cosine-weighted rays hit,
// albedo (the surface texture's average color) times its sun and baked
// light. Samples are taken every 16 units and spread bilinearly over the
// cells between them within a sector or wall strip; values step by 1/32.
void bakeBounce() {
    finishRelight();
    bounceKey=bounceInputs();
    if(flatLightCells.empty()||wallBakeCells.empty())return;
    uint64_t start=SDL_GetTicksNS();
    bool sun=bounceKey&2,lights=bounceKey&4;
    std::array<float,3> sunEnergy={};
    if(sun)for(int c=0;c<3;++c)sunEnergy[c]=sunTint[c]*sunLevel;
    std::vector<std::array<float,3>> flatAlbedo((size_t)numsectors*2,{0.5f,0.5f,0.5f});
    for(int i=0;i<numsectors;++i)for(int ceiling=0;ceiling<2;++ceiling) {
        int pic=ceiling?sectors[i].ceilingpic:sectors[i].floorpic;
        if(pic!=skyflatnum)flatAlbedo[(size_t)i*2+ceiling]=averageColor(lumpImage(firstflat+flattranslation[pic],true));
    }
    // Per side: middle, upper and lower texture.
    std::vector<std::array<float,3>> sideAlbedo((size_t)numlines*6,{0.5f,0.5f,0.5f});
    for(int i=0;i<numlines;++i)for(int side=0;side<2;++side) {
        if(lines[i].sidenum[side]<0)continue;
        const side_t &s=sides[lines[i].sidenum[side]];
        short textures[3]={s.midtexture,s.toptexture,s.bottomtexture};
        for(int part=0;part<3;++part)if(textures[part]>0)
            sideAlbedo[((size_t)i*2+side)*3+part]=averageColor(wallImage(texturetranslation[textures[part]]));
    }
    auto radiance=[&](const BakeHit &hit,float out[3]) {
        if(hit.kind==BakeHit::thing)return; // Decorations stay dark.
        const uint8_t *light=nullptr;float visible=0;const std::array<float,3> *albedo=nullptr;
        if(hit.kind==BakeHit::wall) {
            const BakeLine &line=bakeMap.lines[hit.line];
            int side=line.front==hit.sector?0:1;
            const BakeStrip &strip=bakeStrips[(size_t)hit.line*2+side];
            albedo=&sideAlbedo[((size_t)hit.line*2+side)*3+hit.part];
            if(strip.x<0)return;
            float along=(hit.x-strip.ax)*strip.dx+(hit.y-strip.ay)*strip.dy;
            int column=std::clamp((int)std::floor(along*strip.scale),0,strip.width-1);
            int row=std::clamp((int)std::floor((hit.z-strip.low)*strip.scale),0,strip.height-1);
            light=&wallBakeCells[4*((size_t)(strip.y+row)*atlasStride+strip.x+column)];
            visible=light[3]/255.0f;
        } else {
            int cx=std::clamp((int)std::floor((hit.x-mapOrigin[0])/mapCell),0,mapWidth-1);
            int cy=std::clamp((int)std::floor((hit.y-mapOrigin[1])/mapCell),0,mapHeight-1);
            size_t cell=(size_t)cy*mapWidth+cx;bool ceiling=hit.kind==BakeHit::ceiling;
            albedo=&flatAlbedo[(size_t)hit.sector*2+ceiling];
            light=flatCell(ceiling?ceilingLight:floorLight,cell);
            visible=ceiling?0:contactCells[4*cell+2]/255.0f;
        }
        for(int c=0;c<3;++c)out[c]=(*albedo)[c]*(sunEnergy[c]*visible+(lights?light[c]/255.0f*2:0));
    };
    auto gather=[&](int sector,float x,float y,float z,const float normal[3],uint32_t seed,float out[3]) {
        // A per-sample rotation of the stratified pattern trades banding for fine noise.
        seed=seed*2654435761u;seed^=seed>>15;float rotation=(seed&1023)/1024.0f;
        out[0]=out[1]=out[2]=0;
        for(int n=0;n<16;++n) {
            float direction[3],hitLight[3]={};BakeHit hit;
            cosineDirection(normal,((n&3)+0.5f)/4,std::fmod(((n>>2)+0.5f)/4+rotation,1.0f),direction);
            if(traceRay(bakeMap,sector,x,y,z,direction,512,SkyCeiling::escape,&hit)||hit.kind==BakeHit::none)continue;
            radiance(hit,hitLight);
            for(int c=0;c<3;++c)out[c]+=hitLight[c]/16;
        }
    };
    auto encode=[](const float value[3],uint8_t *out) {
        for(int c=0;c<3;++c)out[c]=(uint8_t)std::min(255L,std::lround(std::round(value[c]*32)/32*0.5f*255));
    };
    int step=std::max(1,(int)std::lround(16/mapCell));
    // Floors and ceilings: samples at block centers, then each cell blends the
    // samples around it that lie in its own sector; a cell with none nearby
    // (a sector smaller than a block) takes its own sample.
    int blocksX=(mapWidth+step-1)/step,blocksY=(mapHeight+step-1)/step;
    std::vector<std::array<float,3>> samples((size_t)blocksX*blocksY*2);
    std::vector<int> sampleSector((size_t)blocksX*blocksY,noSector);
    static const float up[3]={0,0,1},down[3]={0,0,-1};
    auto flatSample=[&](int sector,int x,int y,bool ceiling,float out[3]) {
        out[0]=out[1]=out[2]=0;
        int pic=ceiling?sectors[sector].ceilingpic:sectors[sector].floorpic;
        if(pic==skyflatnum)return;
        const BakeSector &s=bakeMap.sectors[sector];
        gather(sector,mapOrigin[0]+(x+0.5f)*mapCell,mapOrigin[1]+(y+0.5f)*mapCell,ceiling?s.ceiling-1:s.floor+1,
               ceiling?down:up,(uint32_t)(y*mapWidth+x)*2+ceiling,out);
    };
    parallelFor(blocksY,[&](int by) {
        for(int bx=0;bx<blocksX;++bx) {
            int x=std::min(bx*step+step/2,mapWidth-1),y=std::min(by*step+step/2,mapHeight-1);
            size_t b=(size_t)by*blocksX+bx;int sector=seamCells[(size_t)y*mapWidth+x].own;
            sampleSector[b]=sector;if(sector==noSector)continue;
            for(int ceiling=0;ceiling<2;++ceiling)flatSample(sector,x,y,ceiling,samples[b*2+ceiling].data());
        }
    });
    parallelFor(mapHeight,[&](int y) {
        for(int x=0;x<mapWidth;++x) {
            size_t i=(size_t)y*mapWidth+x;int sector=seamCells[i].own;
            if(sector==noSector)continue;
            float gx=(x-step/2)/(float)step,gy=(y-step/2)/(float)step;
            int x0=(int)std::floor(gx),y0=(int)std::floor(gy);float fx=gx-x0,fy=gy-y0;
            float sum[2][3]={},total=0;
            for(int k=0;k<4;++k) {
                int bx=x0+(k&1),by=y0+(k>>1);
                if(bx<0||by<0||bx>=blocksX||by>=blocksY)continue;
                size_t b=(size_t)by*blocksX+bx;if(sampleSector[b]!=sector)continue;
                float w=((k&1)?fx:1-fx)*((k>>1)?fy:1-fy)+1e-4f;
                for(int ceiling=0;ceiling<2;++ceiling)for(int c=0;c<3;++c)sum[ceiling][c]+=samples[b*2+ceiling][c]*w;
                total+=w;
            }
            for(int ceiling=0;ceiling<2;++ceiling) {
                float value[3];
                if(total>0)for(int c=0;c<3;++c)value[c]=sum[ceiling][c]/total;
                else flatSample(sector,x,y,ceiling,value);
                encode(value,flatCell(ceiling?ceilingBounce:floorBounce,i));
            }
        }
    });
    // Walls: samples every step texels inside each strip, blended bilinearly.
    parallelFor((int)bakeOrder.size(),[&](int n) {
        const BakeStrip &strip=bakeStrips[bakeOrder[n]];
        const float normal[3]={strip.dy,-strip.dx,0};
        int columns=(strip.width+step-1)/step,rows=(strip.height+step-1)/step;
        std::vector<std::array<float,3>> coarse((size_t)columns*rows);
        for(int cy=0;cy<rows;++cy)for(int cx=0;cx<columns;++cx) {
            int column=std::min(cx*step+step/2,strip.width-1),row=std::min(cy*step+step/2,strip.height-1);
            float along=std::clamp((column+0.5f)/strip.scale,0.5f,strip.length-0.5f);
            float z=std::min(strip.low+(row+0.5f)/strip.scale,strip.high);
            gather(strip.sector,strip.ax+strip.dx*along+strip.dy*0.5f,strip.ay+strip.dy*along-strip.dx*0.5f,z,normal,
                   (uint32_t)bakeOrder[n]*7919u+cy*131u+cx,coarse[(size_t)cy*columns+cx].data());
        }
        for(int row=0;row<strip.height;++row)for(int column=0;column<strip.width;++column) {
            float gx=std::clamp((column-step/2)/(float)step,0.0f,columns-1.0f),gy=std::clamp((row-step/2)/(float)step,0.0f,rows-1.0f);
            int x0=std::min((int)gx,columns-1),y0=std::min((int)gy,rows-1),x1=std::min(x0+1,columns-1),y1=std::min(y0+1,rows-1);
            float fx=gx-x0,fy=gy-y0,value[3];
            for(int c=0;c<3;++c)
                value[c]=(coarse[(size_t)y0*columns+x0][c]*(1-fx)+coarse[(size_t)y0*columns+x1][c]*fx)*(1-fy)+
                         (coarse[(size_t)y1*columns+x0][c]*(1-fx)+coarse[(size_t)y1*columns+x1][c]*fx)*fy;
            encode(value,&wallBakeCells[4*((size_t)(strip.y+row)*atlasStride+atlasWidth+strip.x+column)]);
        }
    });
    uploadFlatLight();
    uploadWallBake();
    fprintf(stderr,"3D bounce: %s%s, every %d cells, baked in %.0f ms.\n",sun?"sun":"no sun",lights?" and lights":"",
        step,(SDL_GetTicksNS()-start)/1e6);
}
// Decorations in the bakes: static, non-glowing things that stand on the
// floor or hang from the ceiling (columns, trees, hanging bodies) occlude
// the sun, baked lights, bounce, sky and flow rays with their spawn-state
// sprite. Glowing ones are light sources and stay out, so a lamp never
// shadows its own light. Changing the setting re-bakes the level.
void syncBakeThings() {
    int key=settings.thingShadows;
    if(key==bakeThingsKey)return;
    bool rebake=bakeThingsKey>=0;
    bakeThingsKey=key;
    bakeMap.things.clear();
    for(auto &sector:bakeMap.sectors)sector.things.clear();
    if(key)for(int i=0;i<numsectors;++i)for(mobj_t *thing=sectors[i].thinglist;thing;thing=thing->snext) {
        if(thing->player||(thing->flags&(MF_COUNTKILL|MF_SPECIAL|MF_MISSILE|MF_NOBLOCKMAP|MF_SHOOTABLE|MF_CORPSE)))continue;
        if(!(thing->flags&(MF_SOLID|MF_SPAWNCEILING))||(thing->frame&FF_FULLBRIGHT))continue;
        if(thing->sprite<0||thing->sprite>=numsprites||(thing->frame&FF_FRAMEMASK)>=sprites[thing->sprite].numframes)continue;
        const Image &image=lumpImage(firstspritelump+sprites[thing->sprite].spriteframes[thing->frame&FF_FRAMEMASK].lump[0],false);
        float x=units(thing->x),y=units(thing->y),half=image.width*0.5f;
        int index=(int)bakeMap.things.size();
        bakeMap.things.push_back({x,y,units(thing->z)+image.top,-(float)image.left,image.width,image.height,image.pixels.data()});
        // Also list it in neighbors its sprite reaches across a line.
        std::vector<int> reach={i};
        for(int j=0;j<sectors[i].linecount;++j) {
            const line_t &line=*sectors[i].lines[j];
            const sector_t *other=line.frontsector==&sectors[i]?line.backsector:line.frontsector;
            if(!other)continue;
            float ax=units(line.v1->x),ay=units(line.v1->y),ex=units(line.v2->x)-ax,ey=units(line.v2->y)-ay,length2=ex*ex+ey*ey;
            if(length2<0.01f)continue;
            float t=std::clamp(((x-ax)*ex+(y-ay)*ey)/length2,0.0f,1.0f);
            int o=int(other-sectors);
            if(std::hypot(x-ax-ex*t,y-ay-ey*t)<half&&std::find(reach.begin(),reach.end(),o)==reach.end())reach.push_back(o);
        }
        for(int s:reach)bakeMap.sectors[s].things.push_back(index);
    }
    if(rebake) {sunBaked=false;lightBakeKey=-1;bounceKey=-1;ambientKey=-1;flowKey=-1;}
    if(key)fprintf(stderr,"3D bake: %zu decorations occlude baked light.\n",bakeMap.things.size());
}
// Spreads samples taken every step cells over the cells between them,
// bilinearly within one sector or wall strip; values are per sample channels.
// Floors and ceilings: sample(sector,x,y,ceiling,out) at block centers,
// write(cell,ceiling,value) per cell. Walls: sample(strip,x,y,z,out) and
// write(atlas texel,value).
template<int Channels,class FlatSample,class FlatWrite,class WallSample,class WallWrite>
void bakeSpread(int step,FlatSample flatSample,FlatWrite flatWrite,WallSample wallSample,WallWrite wallWrite) {
    using Value=std::array<float,Channels>;
    int blocksX=(mapWidth+step-1)/step,blocksY=(mapHeight+step-1)/step;
    std::vector<Value> samples((size_t)blocksX*blocksY*2);
    std::vector<int> sampleSector((size_t)blocksX*blocksY,noSector);
    parallelFor(blocksY,[&](int by) {
        for(int bx=0;bx<blocksX;++bx) {
            int x=std::min(bx*step+step/2,mapWidth-1),y=std::min(by*step+step/2,mapHeight-1);
            size_t b=(size_t)by*blocksX+bx;int sector=seamCells[(size_t)y*mapWidth+x].own;
            sampleSector[b]=sector;if(sector==noSector)continue;
            for(int ceiling=0;ceiling<2;++ceiling)flatSample(sector,x,y,ceiling!=0,samples[b*2+ceiling]);
        }
    });
    parallelFor(mapHeight,[&](int y) {
        for(int x=0;x<mapWidth;++x) {
            size_t i=(size_t)y*mapWidth+x;int sector=seamCells[i].own;
            if(sector==noSector)continue;
            float gx=(x-step/2)/(float)step,gy=(y-step/2)/(float)step;
            int x0=(int)std::floor(gx),y0=(int)std::floor(gy);float fx=gx-x0,fy=gy-y0;
            Value sum[2]={};float total=0;
            for(int k=0;k<4;++k) {
                int bx=x0+(k&1),by=y0+(k>>1);
                if(bx<0||by<0||bx>=blocksX||by>=blocksY)continue;
                size_t b=(size_t)by*blocksX+bx;if(sampleSector[b]!=sector)continue;
                float w=((k&1)?fx:1-fx)*((k>>1)?fy:1-fy)+1e-4f;
                for(int ceiling=0;ceiling<2;++ceiling)for(int c=0;c<Channels;++c)sum[ceiling][c]+=samples[b*2+ceiling][c]*w;
                total+=w;
            }
            for(int ceiling=0;ceiling<2;++ceiling) {
                Value value;
                if(total>0)for(int c=0;c<Channels;++c)value[c]=sum[ceiling][c]/total;
                else flatSample(sector,x,y,ceiling!=0,value);
                flatWrite(i,ceiling!=0,value);
            }
        }
    });
    parallelFor((int)bakeOrder.size(),[&](int n) {
        const BakeStrip &strip=bakeStrips[bakeOrder[n]];
        int columns=(strip.width+step-1)/step,rows=(strip.height+step-1)/step;
        std::vector<Value> coarse((size_t)columns*rows);
        for(int cy=0;cy<rows;++cy)for(int cx=0;cx<columns;++cx) {
            int column=std::min(cx*step+step/2,strip.width-1),row=std::min(cy*step+step/2,strip.height-1);
            float along=std::clamp((column+0.5f)/strip.scale,0.5f,strip.length-0.5f);
            float z=std::min(strip.low+(row+0.5f)/strip.scale,strip.high);
            wallSample(strip,strip.ax+strip.dx*along+strip.dy*0.5f,strip.ay+strip.dy*along-strip.dx*0.5f,z,
                       (uint32_t)bakeOrder[n]*7919u+cy*131u+cx,coarse[(size_t)cy*columns+cx]);
        }
        for(int row=0;row<strip.height;++row)for(int column=0;column<strip.width;++column) {
            float gx=std::clamp((column-step/2)/(float)step,0.0f,columns-1.0f),gy=std::clamp((row-step/2)/(float)step,0.0f,rows-1.0f);
            int x0=std::min((int)gx,columns-1),y0=std::min((int)gy,rows-1),x1=std::min(x0+1,columns-1),y1=std::min(y0+1,rows-1);
            float fx=gx-x0,fy=gy-y0;Value value;
            for(int c=0;c<Channels;++c)
                value[c]=(coarse[(size_t)y0*columns+x0][c]*(1-fx)+coarse[(size_t)y0*columns+x1][c]*fx)*(1-fy)+
                         (coarse[(size_t)y1*columns+x0][c]*(1-fx)+coarse[(size_t)y1*columns+x1][c]*fx)*fy;
            wallWrite((size_t)(strip.y+row)*atlasStride+strip.x+column,value);
        }
    });
}
// Sky light and ambient occlusion, baked together from 12 cosine-weighted
// rays per sample, every ~8 units: the share that escapes through a sky
// ceiling (walls see at most half the sky, so theirs counts double) and how
// open the surroundings are within 64 units. Each is stored in 16 steps,
// like Doom's light levels, packed into the alpha of the bounce layers:
// sky in the high nibble, openness in the low one.
void bakeAmbient() {
    finishRelight();
    ambientKey=bakeThingsKey;
    if(flatLightCells.empty()||wallBakeCells.empty())return;
    uint64_t start=SDL_GetTicksNS();
    std::vector<float> skyLights;
    for(int i=0;i<numsectors;++i)if(sectors[i].ceilingpic==skyflatnum)skyLights.push_back(lighting(&sectors[i]));
    skyLevel=0;
    if(!skyLights.empty()) {
        std::nth_element(skyLights.begin(),skyLights.begin()+skyLights.size()/2,skyLights.end());
        skyLevel=skyLights[skyLights.size()/2];
        // The sky's color: the upper rows of the sky texture, above any horizon art.
        const Image &sky=wallImage(skytexture);
        const byte *palette=(const byte*)W_CacheLumpName((char*)"PLAYPAL",PU_CACHE);
        double sum[3]={};
        for(int y=0;y<std::max(1,sky.height*2/5);++y)for(int x=0;x<sky.width;++x) {
            size_t i=(size_t)y*sky.width+x;if(!sky.pixels[2*i+1])continue;
            for(int c=0;c<3;++c)sum[c]+=palette[sky.pixels[2*i]*3+c];
        }
        double peak=std::max({sum[0],sum[1],sum[2],1e-9});
        for(int c=0;c<3;++c)skyTint[c]=(float)(0.5+0.5*sum[c]/peak);
    }
    auto gather=[&](int sector,float x,float y,float z,const float normal[3],uint32_t seed,std::array<float,2> &out) {
        seed=seed*2654435761u;seed^=seed>>15;float rotation=(seed&1023)/1024.0f;
        float sky=0,open=0;
        for(int n=0;n<12;++n) {
            float direction[3];BakeHit hit;
            cosineDirection(normal,((n&3)+0.5f)/4,std::fmod(((n>>2)+0.5f)/3+rotation,1.0f),direction);
            if(traceRay(bakeMap,sector,x,y,z,direction,INFINITY,SkyCeiling::escape,&hit)) {sky+=1;open+=1;continue;}
            if(hit.kind==BakeHit::none) {open+=1;continue;}
            open+=std::min(1.0f,std::sqrt((hit.x-x)*(hit.x-x)+(hit.y-y)*(hit.y-y)+(hit.z-z)*(hit.z-z))/64);
        }
        out={sky/12,open/12};
    };
    auto pack=[](float sky,float open) {
        return (uint8_t)((std::lround(std::clamp(sky,0.0f,1.0f)*15)<<4)|std::lround(std::clamp(open,0.0f,1.0f)*15));
    };
    static const float up[3]={0,0,1},down[3]={0,0,-1};
    bakeSpread<2>(std::max(1,(int)std::lround(8/mapCell)),
        [&](int sector,int x,int y,bool ceiling,std::array<float,2> &out) {
            out={0,1};
            int pic=ceiling?sectors[sector].ceilingpic:sectors[sector].floorpic;
            if(pic==skyflatnum)return;
            const BakeSector &s=bakeMap.sectors[sector];
            gather(sector,mapOrigin[0]+(x+0.5f)*mapCell,mapOrigin[1]+(y+0.5f)*mapCell,ceiling?s.ceiling-1:s.floor+1,
                   ceiling?down:up,(uint32_t)(y*mapWidth+x)*2+ceiling,out);
        },
        [&](size_t cell,bool ceiling,const std::array<float,2> &value) {flatCell(ceiling?ceilingBounce:floorBounce,cell)[3]=pack(value[0],value[1]);},
        [&](const BakeStrip &strip,float x,float y,float z,uint32_t seed,std::array<float,2> &out) {
            const float normal[3]={strip.dy,-strip.dx,0};
            gather(strip.sector,x,y,z,normal,seed,out);out[0]*=2;
        },
        [&](size_t texel,const std::array<float,2> &value) {wallBakeCells[4*(texel+atlasWidth)+3]=pack(value[0],value[1]);});
    uploadFlatLight();
    uploadWallBake();
    fprintf(stderr,"3D sky light and occlusion: sky level %.2f, baked in %.0f ms.\n",skyLevel,(SDL_GetTicksNS()-start)/1e6);
}
// Sector light flow: from points every ~16 units, at chest height, 12 level
// rays check which other sectors are in plain view at 48, 96 and 160 units.
// The most visible one is stored per cell as its flow neighbor (seam map,
// fourth channel) with a weight (contact map alpha); during play the cell's
// light rises toward that sector's current light, so flickering rooms
// flicker faintly beyond their openings. Doors count as they start (closed).
void bakeFlow() {
    finishRelight();
    flowKey=bakeThingsKey;
    if(seamCells.empty())return;
    uint64_t start=SDL_GetTicksNS();
    for(auto &cell:seamCells)cell.flow=noSector;
    struct Flow { int neighbor=noSector;float weight=0; };
    int step=std::max(1,(int)std::lround(16/mapCell));
    int blocksX=(mapWidth+step-1)/step,blocksY=(mapHeight+step-1)/step;
    std::vector<Flow> blocks((size_t)blocksX*blocksY);
    std::vector<int> blockSector((size_t)blocksX*blocksY,noSector);
    parallelFor(blocksY,[&](int by) {
        std::vector<std::pair<int,float>> score;
        for(int bx=0;bx<blocksX;++bx) {
            int x=std::min(bx*step+step/2,mapWidth-1),y=std::min(by*step+step/2,mapHeight-1);
            size_t b=(size_t)by*blocksX+bx;int sector=seamCells[(size_t)y*mapWidth+x].own;
            blockSector[b]=sector;if(sector==noSector)continue;
            const BakeSector &s=bakeMap.sectors[sector];
            if(s.ceiling-s.floor<8)continue;
            float px=mapOrigin[0]+(x+0.5f)*mapCell,py=mapOrigin[1]+(y+0.5f)*mapCell,pz=s.floor+std::min(40.0f,(s.ceiling-s.floor)*0.5f);
            float rotation=hashUnit(bx,by,0x51f1u);
            score.clear();
            for(int k=0;k<12;++k) {
                float angle=(k+rotation)*2*doomPi/12,direction[3]={std::cos(angle),std::sin(angle),0};
                static const std::pair<float,float> lengths[]={{48,1.0f},{96,0.6f},{160,0.35f}};
                for(auto [length,weight]:lengths) {
                    if(!traceRay(bakeMap,sector,px,py,pz,direction,length,SkyCeiling::open))break;
                    float ex=px+direction[0]*length,ey=py+direction[1]*length;
                    int end=int(R_PointInSubsector((fixed_t)(ex*FRACUNIT),(fixed_t)(ey*FRACUNIT))->sector-sectors);
                    if(end==sector)continue;
                    auto found=std::find_if(score.begin(),score.end(),[&](const auto &e){return e.first==end;});
                    if(found==score.end())score.push_back({end,weight});else found->second+=weight;
                }
            }
            for(const auto &entry:score)if(entry.second>blocks[b].weight)blocks[b]={entry.first,entry.second};
            blocks[b].weight=std::min(1.0f,blocks[b].weight*2/(12*1.95f));
        }
    });
    // Cells blend the weights of nearby same-sector blocks that share the
    // strongest one's neighbor.
    parallelFor(mapHeight,[&](int y) {
        for(int x=0;x<mapWidth;++x) {
            size_t i=(size_t)y*mapWidth+x;int sector=seamCells[i].own;
            contactCells[4*i+3]=0;
            if(sector==noSector)continue;
            float gx=(x-step/2)/(float)step,gy=(y-step/2)/(float)step;
            int x0=(int)std::floor(gx),y0=(int)std::floor(gy);float fx=gx-x0,fy=gy-y0;
            int neighbor=noSector;float strongest=0,sum=0,total=0;
            for(int pass=0;pass<2;++pass)for(int k=0;k<4;++k) {
                int bx=x0+(k&1),by=y0+(k>>1);
                if(bx<0||by<0||bx>=blocksX||by>=blocksY)continue;
                size_t b=(size_t)by*blocksX+bx;if(blockSector[b]!=sector)continue;
                const Flow &flow=blocks[b];
                if(pass==0) {if(flow.weight>strongest){strongest=flow.weight;neighbor=flow.neighbor;}continue;}
                float w=((k&1)?fx:1-fx)*((k>>1)?fy:1-fy)+1e-4f;
                sum+=flow.neighbor==neighbor?flow.weight*w:0;total+=w;
            }
            if(neighbor==noSector||total<=0)continue;
            seamCells[i].flow=(uint16_t)neighbor;
            contactCells[4*i+3]=(uint8_t)std::lround(std::clamp(sum/total,0.0f,1.0f)*255);
        }
    });
    seamTexture=gpuCreateTexture(GpuFormat::RGBA16Uint,mapWidth,mapHeight,seamCells.data(),mapWidth*8);
    contactTexture=gpuCreateTexture(GpuFormat::RGBA8,mapWidth,mapHeight,contactCells.data(),mapWidth*4);
    if(!seamTexture||!contactTexture)I_Error((char*)"Could not allocate surface maps");
    fprintf(stderr,"3D light flow: every %d cells, baked in %.0f ms.\n",step,(SDL_GetTicksNS()-start)/1e6);
}
// CPU twin of flowLight in world.frag, for sprites and the weapon.
float flowLightAt(float x,float y,const sector_t *sector,float light) {
    if(!settings.lightFlow||flowKey<0||seamCells.empty())return light;
    int cx=(int)std::floor((x-mapOrigin[0])/mapCell),cy=(int)std::floor((y-mapOrigin[1])/mapCell);
    if(cx<0||cy<0||cx>=mapWidth||cy>=mapHeight)return light;
    size_t i=(size_t)cy*mapWidth+cx;const SeamCell &cell=seamCells[i];
    if(cell.own!=(uint16_t)(sector-sectors)||cell.flow==noSector)return light;
    return light+std::round(std::max(0.0f,lighting(&sectors[cell.flow])-light)*contactCells[4*i+3]/255.0f*0.8f*16)/16;
}
// Moving sectors re-light (settings.movingRelight): bakeMap follows doors,
// lifts and lowering walls, at their exact heights at rest and on an 8-unit
// grid while they move, so things lit by traceStatic see through an open
// door at once. Each change re-bakes, on a background thread, the static
// light within reach of every baked light whose radius touches the sector,
// and the sun on floors and walls up to the distance the sector's opening
// can throw it. Bounce, sky light, flow and sunbeams keep the level's start.
// One re-bake runs at a time; changes meanwhile wait for the next.
struct Relight {
    std::thread worker;
    std::atomic<bool> done{false};
    bool running=false,lights=false,only=false,grid=false,sun=false;
    BakeMap map; // bakeMap as it was when the re-bake started.
    float sunDirection[3]={};
    // Per map cell, wall strip and grid cell: 1 re-light, 2 re-sun.
    std::vector<uint8_t> cells,strips,gridCells;
    int lightBounds[4],sunBounds[4]; // Marked cells: x0, y0, x1, y1 inclusive.
    std::vector<int16_t> sunCells; // Floor sun over sunBounds; -1 unchanged.
    std::vector<size_t> gridList;
    std::vector<StaticCell> gridResults;
} relight;
std::vector<int> relightPending;
// The height a sector's floor or ceiling takes in bakeMap.
float bakeHeight(int sector,bool ceiling) {
    float now=units(ceiling?sectors[sector].ceilingheight:sectors[sector].floorheight);
    if((size_t)sector>=oldHeights.size()||oldHeights[sector][ceiling]==now)return now;
    return std::round(now/8)*8;
}
void markRelight(float x0,float y0,float x1,float y1,uint8_t bit) {
    int cx0=std::clamp((int)std::floor((x0-mapOrigin[0])/mapCell),0,mapWidth-1),cx1=std::clamp((int)std::floor((x1-mapOrigin[0])/mapCell),0,mapWidth-1);
    int cy0=std::clamp((int)std::floor((y0-mapOrigin[1])/mapCell),0,mapHeight-1),cy1=std::clamp((int)std::floor((y1-mapOrigin[1])/mapCell),0,mapHeight-1);
    for(int y=cy0;y<=cy1;++y)for(int x=cx0;x<=cx1;++x)relight.cells[(size_t)y*mapWidth+x]|=bit;
    int *b=bit==1?relight.lightBounds:relight.sunBounds;
    b[0]=std::min(b[0],cx0);b[1]=std::min(b[1],cy0);b[2]=std::max(b[2],cx1);b[3]=std::max(b[3],cy1);
    for(int index:bakeOrder) {
        const BakeStrip &strip=bakeStrips[index];
        float bx=strip.ax+strip.dx*strip.length,by=strip.ay+strip.dy*strip.length;
        if(std::max(strip.ax,bx)<x0||std::min(strip.ax,bx)>x1||std::max(strip.ay,by)<y0||std::min(strip.ay,by)>y1)continue;
        relight.strips[index]|=bit;
    }
    if(bit==1&&relight.grid) {
        int gx0=std::clamp((int)std::floor((x0-mapOrigin[0])/staticCell),0,staticWidth-1),gx1=std::clamp((int)std::floor((x1-mapOrigin[0])/staticCell),0,staticWidth-1);
        int gy0=std::clamp((int)std::floor((y0-mapOrigin[1])/staticCell),0,staticHeight-1),gy1=std::clamp((int)std::floor((y1-mapOrigin[1])/staticCell),0,staticHeight-1);
        for(int y=gy0;y<=gy1;++y)for(int x=gx0;x<=gx1;++x)relight.gridCells[(size_t)y*staticWidth+x]=1;
    }
}
void startRelight() {
    Relight &job=relight;
    job.lights=settings.bakedLights&&lightBakeKey>=0&&lightBakeKey==lightBakeInputs()&&!bakeGrid.empty()&&wallBakeTexture&&flatLightTexture;
    job.only=job.lights&&(lightBakeKey&2)&&wallDirectionTexture&&flatDirectionTexture;
    job.grid=job.lights&&staticSerial==lightBakeSerial&&!staticGrid.empty();
    job.sun=sunBaked&&sunLevel>0&&contactTexture&&wallBakeTexture;
    std::vector<int> pending;pending.swap(relightPending);
    if(!job.lights&&!job.sun)return;
    job.cells.assign((size_t)mapWidth*mapHeight,0);job.strips.assign(bakeStrips.size(),0);
    job.gridCells.assign(job.grid?staticGrid.size():0,0);
    for(int *b:{job.lightBounds,job.sunBounds}) {b[0]=b[1]=INT_MAX;b[2]=b[3]=-1;}
    const float elevation=40*doomPi/180;
    for(int sector:pending) {
        const BakeSector &s=bakeMap.sectors[sector];
        float x0=1e9f,y0=1e9f,x1=-1e9f,y1=-1e9f,low=s.floor,high=s.ceiling;
        for(int index:s.lines) {
            const BakeLine &line=bakeMap.lines[index];
            x0=std::min({x0,line.ax,line.bx});y0=std::min({y0,line.ay,line.by});
            x1=std::max({x1,line.ax,line.bx});y1=std::max({y1,line.ay,line.by});
            int other=line.front==sector?line.back:line.front;
            if(other>=0) {low=std::min(low,bakeMap.sectors[other].floor);high=std::max(high,bakeMap.sectors[other].ceiling);}
        }
        if(x1<x0)continue;
        if(job.lights) {
            for(const BakeLight &light:bakeSources) {
                float dx=std::max({x0-light.x,0.0f,light.x-x1}),dy=std::max({y0-light.y,0.0f,light.y-y1});
                if(dx*dx+dy*dy<light.radius*light.radius)
                    markRelight(light.x-light.radius,light.y-light.radius,light.x+light.radius,light.y+light.radius,1);
            }
            for(const BakeArea &area:bakeAreas) {
                if(area.high[0]+area.radius<x0||area.low[0]-area.radius>x1||area.high[1]+area.radius<y0||area.low[1]-area.radius>y1)continue;
                markRelight(area.low[0]-area.radius,area.low[1]-area.radius,area.high[0]+area.radius,area.high[1]+area.radius,1);
            }
        }
        if(job.sun) {
            // Sun through the sector reaches floors and walls this far away from the sun.
            float reach=std::min((high-low)/std::tan(elevation)+16,1024.0f);
            float sx=-sunAzimuth[0]*reach,sy=-sunAzimuth[1]*reach;
            markRelight(x0+std::min(sx,0.0f),y0+std::min(sy,0.0f),x1+std::max(sx,0.0f),y1+std::max(sy,0.0f),2);
        }
    }
    if(job.lightBounds[2]<0&&job.sunBounds[2]<0)return;
    job.map=bakeMap;
    for(int c=0;c<3;++c)job.sunDirection[c]=sunDirection[c];
    const int *sb=job.sunBounds;
    job.sunCells.assign(sb[2]>=0?(size_t)(sb[2]-sb[0]+1)*(sb[3]-sb[1]+1):0,-1);
    job.gridList.clear();
    for(size_t i=0;i<job.gridCells.size();++i)if(job.gridCells[i])job.gridList.push_back(i);
    job.gridResults.assign(job.gridList.size(),{});
    job.done=false;job.running=true;
    // Half the cores, so the game keeps its own.
    job.worker=std::thread([&job] {
        unsigned threads=std::max(1u,std::thread::hardware_concurrency()/2);
        int y0=std::min(job.lightBounds[1],job.sunBounds[1]),y1=std::max(job.lightBounds[3],job.sunBounds[3]);
        const int *sb=job.sunBounds;
        parallelFor(y1-y0+1,[&](int row) {
            int y=y0+row;
            for(int x=0;x<mapWidth;++x) {
                size_t i=(size_t)y*mapWidth+x;uint8_t mark=job.cells[i];
                if(mark&1)lightCell(job.map,i,job.only);
                if(mark&2)job.sunCells[(size_t)(y-sb[1])*(sb[2]-sb[0]+1)+(x-sb[0])]=(int16_t)sunCell(job.map,i,job.sunDirection);
            }
        },threads);
        std::vector<int> strips;
        for(int index:bakeOrder)if(job.strips[index])strips.push_back(index);
        parallelFor((int)strips.size(),[&](int n) {
            const BakeStrip &strip=bakeStrips[strips[n]];
            if(job.strips[strips[n]]&1)lightStrip(job.map,strip,job.only);
            if(job.strips[strips[n]]&2)sunStrip(job.map,strip,job.sunDirection);
        },threads);
        parallelFor((int)job.gridList.size(),[&](int n) {
            size_t i=job.gridList[n];
            job.gridResults[n]=staticCellAt(job.map,(int)(i%staticWidth),(int)(i/staticWidth));
        },threads);
        job.done=true;
    });
}
void finishRelight(bool apply) {
    Relight &job=relight;
    if(!job.running)return;
    job.worker.join();job.running=false;
    if(!apply)return;
    const int *lb=job.lightBounds,*sb=job.sunBounds;
    if(job.lights&&lb[2]>=0) {
        int w=lb[2]-lb[0]+1,h=lb[3]-lb[1]+1;
        gpuUpdateTexture(flatLightTexture,GpuFormat::RGBA8,{{lb[0],lb[1]+floorLight*mapHeight,w,h},{lb[0],lb[1]+ceilingLight*mapHeight,w,h}},
                         flatLightCells.data(),mapWidth*4);
        if(job.only)gpuUpdateTexture(flatDirectionTexture,GpuFormat::RGBA8,{{lb[0],lb[1],w,h},{lb[0],lb[1]+mapHeight,w,h}},flatDirectionCells.data(),mapWidth*4);
        for(size_t n=0;n<job.gridList.size();++n)staticGrid[job.gridList[n]]=job.gridResults[n];
    }
    if(job.sun&&sb[2]>=0) {
        int w=sb[2]-sb[0]+1,h=sb[3]-sb[1]+1;
        for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
            int16_t visible=job.sunCells[(size_t)y*w+x];
            if(visible>=0)contactCells[4*((size_t)(sb[1]+y)*mapWidth+sb[0]+x)+2]=(uint8_t)visible;
        }
        gpuUpdateTexture(contactTexture,GpuFormat::RGBA8,{{sb[0],sb[1],w,h}},contactCells.data(),mapWidth*4);
    }
    std::vector<std::array<int,4>> rects,directions;
    for(size_t index=0;index<job.strips.size();++index) {
        if(!job.strips[index])continue;
        const BakeStrip &strip=bakeStrips[index];
        rects.push_back({strip.x,strip.y,strip.width,strip.height});
        if(job.only&&(job.strips[index]&1))directions.push_back(rects.back());
    }
    gpuUpdateTexture(wallBakeTexture,GpuFormat::RGBA8,rects,wallBakeCells.data(),atlasStride*4);
    gpuUpdateTexture(wallDirectionTexture,GpuFormat::RGBA8,directions,wallDirectionCells.data(),atlasWidth*4);
}
// Called each frame before geometry: follows sector heights into bakeMap,
// applies a finished re-bake and starts the next.
void relightMovingSectors() {
    if(relight.running&&relight.done)finishRelight();
    if(!settings.movingRelight||bakeMap.sectors.size()!=(size_t)numsectors)return;
    for(int i=0;i<numsectors;++i) {
        BakeSector &s=bakeMap.sectors[i];
        float floor=bakeHeight(i,false),ceiling=bakeHeight(i,true);
        if(floor==s.floor&&ceiling==s.ceiling)continue;
        s.floor=floor;s.ceiling=ceiling;
        if(std::find(relightPending.begin(),relightPending.end(),i)==relightPending.end())relightPending.push_back(i);
    }
    if(!relight.running&&!relightPending.empty())startRelight();
}
void geometry(const Uniforms &camera,float fraction) {
    for(auto &batch:batches)batch.second.clear();
    for(auto &batch:shadowBatches)batch.second.clear();
    surfaceLights.clear();
    std::copy(camera.eye,camera.eye+3,surfaceEye);collectFlashes(fraction,camera);
    collectFlatLights();
    // The nearest reflective liquid below the eye sets this frame's mirror plane.
    reflectionActive=false;float nearest=1024;
    if(settings.reflections&&camera.effects[2]==0)for(int i=0;i<numsectors;++i) {
        if(!sectorMist[i].reflective||sectors[i].floorpic==skyflatnum)continue;
        float z=floorZ(&sectors[i]);if(camera.eye[2]<z+1)continue;
        for(int leaf:sectorFloors[i]) {
            const auto &b=floorBounds[leaf];
            float dx=std::max({b[0]-camera.eye[0],0.0f,camera.eye[0]-b[2]}),dy=std::max({b[1]-camera.eye[1],0.0f,camera.eye[1]-b[3]});
            float d=std::hypot(dx,dy);
            if(d<nearest){nearest=d;reflectionPlane=z;reflectionActive=true;}
        }
    }
    // Vanilla draws sky wherever a sky flat would be, hiding everything past
    // it; sky ceilings and the open band above a lower sky neighbor therefore
    // go into depth with the sky shader. A sky ceiling rises to the tallest
    // sprite in its sector, with sky walls along its edges up to that height,
    // so trees and monsters still stand out against a low sky.
    skyVertices.clear();
    std::vector<float> skyTop(numsectors);
    for(int i=0;i<numsectors;++i) {
        skyTop[i]=ceilZ(&sectors[i]);
        if(sectors[i].ceilingpic!=skyflatnum)continue;
        for(const mobj_t *thing=sectors[i].thinglist;thing;thing=thing->snext) {
            if(thing->sprite<0||thing->sprite>=numsprites)continue;
            const auto &sprite=sprites[thing->sprite];int frame=thing->frame&FF_FRAMEMASK;
            if(frame>=sprite.numframes)continue;
            const auto &sf=sprite.spriteframes[frame];
            float x,y,z;interpolatedPosition(*thing,fraction,x,y,z);
            for(int r=0;r<(sf.rotate?8:1);++r)skyTop[i]=std::max(skyTop[i],z+units(spritetopoffset[sf.lump[r]])+1);
        }
    }
    auto skyWall=[](Point a,Point b,float bottom,float top) {
        if(top<=bottom)return;
        for(const Vertex &v:{Vertex{a.x,a.y,bottom},Vertex{b.x,b.y,bottom},Vertex{b.x,b.y,top},
                             Vertex{a.x,a.y,bottom},Vertex{b.x,b.y,top},Vertex{a.x,a.y,top}})skyVertices.push_back(v);
    };
    for(int i=0;i<numsubsectors;++i) {
        const auto &poly=floors[i]; if(poly.size()<3) continue;
        const sector_t *sector=subsectors[i].sector;
        for(int ceiling=0;ceiling<2;++ceiling) {
            int flat=ceiling?sector->ceilingpic:sector->floorpic;
            if(flat==skyflatnum) {
                float z=ceiling?skyTop[sector-sectors]:floorZ(sector);
                for(size_t n=1;n+1<poly.size();++n) for(Point p:{poly[0],poly[n],poly[n+1]}) skyVertices.push_back({p.x,p.y,z});
                continue;
            }
            int lump=firstflat+flattranslation[flat]; lumpImage(lump,true);
            float z=sectorHeight(sector,ceiling),light=lighting(sector);
            // Mode 32: this floor samples the mirrored pass.
            unsigned mirror=reflectionActive&&!ceiling&&sectorMist[sector-sectors].reflective&&std::abs(z-reflectionPlane)<0.5f?32:0;

            auto &out=batches[-1-lump];
            for(size_t n=1;n+1<poly.size();++n) for(Point p:{poly[0],poly[n],poly[n+1]})
                out.push_back({p.x,p.y,z,p.x,-p.y,light,mirror|sectorMode(sector)});
        }
    }
    for(int i=0;i<numlines;++i) {
        const line_t &line=lines[i];
        for(int side=0;side<2;++side) {
            if(line.sidenum[side]<0) continue;
            const side_t &s=sides[line.sidenum[side]];
            const sector_t *front=s.sector,*back=side?line.frontsector:line.backsector;
            Point a={units((side?line.v2:line.v1)->x),units((side?line.v2:line.v1)->y)};
            Point b={units((side?line.v1:line.v2)->x),units((side?line.v1:line.v2)->y)};
            bool facing=(b.x-a.x)*(camera.eye[1]-a.y)-(b.y-a.y)*(camera.eye[0]-a.x)<=0;
            float floor=floorZ(front),ceil=ceilZ(front),row=units(s.rowoffset),light=lighting(front);
            if(std::abs(b.y-a.y)<0.01) light=std::max(0.1f,light-0.04f);
            if(std::abs(b.x-a.x)<0.01) light=std::min(1.0f,light+0.04f);
            if(!back) {
                float anchor=ceil;
                if(s.midtexture && (line.flags&ML_DONTPEGBOTTOM)) anchor=floor+units(textureheight[s.midtexture]);
                addWall(line,side,a,b,floor,ceil,s.midtexture,anchor+row,light,facing,0);
            } else {
                float bf=floorZ(back),bc=ceilZ(back);
                bool sharedSky=front->ceilingpic==skyflatnum&&back->ceilingpic==skyflatnum;
                if(bc<ceil&&!sharedSky) {
                    float anchor=(line.flags&ML_DONTPEGTOP)?ceil:bc+(s.toptexture?units(textureheight[s.toptexture]):0);
                    addWall(line,side,a,b,std::max(bc,floor),ceil,s.toptexture,anchor+row,light,facing,1);
                }
                if(bf>floor) {
                    float anchor=(line.flags&ML_DONTPEGBOTTOM)?ceil:bf;
                    addWall(line,side,a,b,floor,std::min(bf,ceil),s.bottomtexture,anchor+row,light,facing,2);
                }
                if(s.midtexture) {
                    int tex=texturetranslation[s.midtexture]; Image &image=wallImage(tex);
                    float low=std::max(floor,bf),high=std::min(ceil,bc);
                    float anchor=(line.flags&ML_DONTPEGBOTTOM)?low+image.height:high;
                    anchor+=row;
                    float top=std::min(high,anchor),bottom=std::max(low,anchor-image.height);
                    addWall(line,side,a,b,bottom,top,s.midtexture,anchor,light,facing,3);
                }
            }
            if(front->ceilingpic==skyflatnum)
                skyWall(a,b,back&&back->ceilingpic==skyflatnum?std::max(skyTop[back-sectors],floor):ceil,skyTop[front-sectors]);
        }
    }
    collectSurfaceLights();
    buildImpacts(camera);
    float rx=camera.right[0],ry=camera.right[1];
    struct Caster { const mobj_t *thing;const Image *image;int lump;bool flip;float x,y,z,distance; };
    std::vector<Caster> casters;
    const Flash *beam=settings.flashlightShadows&&flashlightOn&&camera.effects[2]==0&&flashes.count&&flashes.lights[0].direction[3]>0?&flashes.lights[0]:nullptr;
    for(int i=0;i<numsectors;++i) for(mobj_t *thing=sectors[i].thinglist;thing;thing=thing->snext) {
        if(thing==players[displayplayer].mo||thing->sprite<0||thing->sprite>=numsprites) continue;
        const auto &sprite=sprites[thing->sprite]; int frame=thing->frame&FF_FRAMEMASK;
        if(frame>=sprite.numframes) continue;
        const auto &sf=sprite.spriteframes[frame];
        unsigned rotation=0;
        if(sf.rotate) {
            angle_t angle=(angle_t)(int64_t)(atan2(units(thing->y)-camera.eye[1],units(thing->x)-camera.eye[0])*(4294967296.0/(2*doomPi)));
            rotation=(angle-thing->angle+(unsigned)(ANG45/2)*9)>>29;
        }
        int lump=firstspritelump+sf.lump[rotation]; Image &image=lumpImage(lump,false);
        float x,y,z;interpolatedPosition(*thing,fraction,x,y,z);
        addEnemyShadow(*thing,image,lump,sf.flip[rotation],x,y,z,camera);
        if(beam&&!(thing->flags&(MF_MISSILE|MF_NOBLOCKMAP|MF_SPECIAL))&&(thing->flags&(MF_SOLID|MF_SHOOTABLE))&&!(thing->frame&FF_FULLBRIGHT))
            casters.push_back({thing,&image,lump,(bool)sf.flip[rotation],x,y,z,std::hypot(x-camera.eye[0],y-camera.eye[1])});
        std::array<float,3> pulse={};float strongest=0;ShadowLight shadowLight={};
        for(unsigned n=0;n<flashes.count;++n) {
            if(fogOnly[n])continue;
            float contribution=flashAt(x,y,z+units(thing->height)*0.5f,flashes.lights[n]);
            // Static lights reach sprites through the baked light at their feet.
            float dynamic=contribution*(1-(bakedLightsActive?flashes.lights[n].baked:0));
            for(int c=0;c<3;++c) pulse[c]+=dynamic*flashes.lights[n].color[c];
            if(lightSources[n]!=thing&&flashes.lights[n].position[2]>z+8&&contribution>strongest) {
                strongest=contribution;const Flash &f=flashes.lights[n];
                shadowLight={f.position[0],f.position[1],f.position[2],contribution};
            }
        }
        // Static light at the thing's center; with bake-only lights or the
        // grid its strongest light also competes to cast the shadow.
        StaticSample statics;
        auto baked=bakedLightAt(x,y,z+units(thing->height)*0.5f,&sectors[i],&statics);
        ShadowLight fixedLight;
        if(staticShadow(statics,z,fixedLight)&&fixedLight.strength>strongest) {strongest=fixedLight.strength;shadowLight=fixedLight;}
        if(strongest>0.025f) addEnemyShadow(*thing,image,lump,sf.flip[rotation],x,y,z,camera,&shadowLight);
        float left=-(float)image.left,right=left+image.width,top=z+image.top,bottom=top-image.height;
        float u0=sf.flip[rotation]?image.width:0,u1=sf.flip[rotation]?0:image.width;
        float light=lighting(&sectors[i],thing->frame&FF_FULLBRIGHT); unsigned mode=2|(thing->flags&MF_SHADOW?4:0);
        // Mode 8: per-pixel lighting from silhouette normals; 16: mirrored frame.
        if(settings.detail&&!(thing->frame&FF_FULLBRIGHT))mode|=8|(sf.flip[rotation]?16:0);
        // Mode 2048: airborne blood shines like the floor blood (needs the normals of mode 8).
        if(settings.bloodShine&&thing->type==MT_BLOOD&&(mode&8))mode|=2048;
        if(!(thing->frame&FF_FULLBRIGHT))light=sunLightAt(x,y,&sectors[i],flowLightAt(x,y,&sectors[i],seamLightAt(x,y,&sectors[i],light)));
        // Mode 1024: effect sprites (shots, puffs, fogs) dither out at floors
        // and ceilings and draw in front of walls within half their width.
        bool soft=settings.softSprites&&((thing->flags&MF_MISSILE)||((thing->flags&MF_NOBLOCKMAP)&&(thing->flags&MF_NOGRAVITY)));
        if(soft)mode|=1024|sectorMode(&sectors[i]);
        if(thing->flags&MF_SPECIAL) {
            // Pickups stay readable in the dark with a soft pulse and bloom (mode 64).
            float phase=(float)((uintptr_t)thing%997)*0.37f;
            light=std::max(light,0.5f+0.08f*std::sin(gameSeconds()*3.0f+phase));mode|=64;
        }
        quad(batches[-1-lump],{x+rx*left,y+ry*left,top,u0,0,light,mode},
            {x+rx*right,y+ry*right,top,u1,0,light,mode},
            {x+rx*right,y+ry*right,bottom,u1,(float)image.height,light,mode},
            {x+rx*left,y+ry*left,bottom,u0,(float)image.height,light,mode});
        // Emissive WAD frames keep their original white cores and artwork;
        // their light still illuminates surrounding surfaces and other sprites.
        // Detail sprites light themselves per pixel from the light mask, so
        // their tint carries only the baked light. With bake-only lights they
        // shape it by where it comes from (statics).
        if(mode&8)pulse=baked;else for(int c=0;c<3;++c)pulse[c]+=baked[c];
        unsigned packedStatics=0;
        float towards=std::sqrt(statics.towards[0]*statics.towards[0]+statics.towards[1]*statics.towards[1]+statics.towards[2]*statics.towards[2]);
        if(bakeOnlyActive&&(mode&8)&&statics.amount>0&&towards>1e-6f) {
            for(int c=0;c<3;++c)packedStatics|=(unsigned)std::lround((statics.towards[c]/towards*0.5f+0.5f)*255)<<(8*c);
            packedStatics|=(unsigned)std::lround(std::clamp(towards/statics.amount,0.0f,1.0f)*255)<<24;
        }
        if(thing->frame&FF_FULLBRIGHT) pulse={};
        // Mode 128: grounding darkens the feet of things that rest on the floor.
        // Sprites have no sun map, so vSun carries strength and height above floor.
        float lift=units(thing->z-thing->floorz),grounding=1-lift/32;
        bool grounded=settings.detail&&grounding>0&&!(thing->frame&FF_FULLBRIGHT)&&!soft&&thing->type!=MT_BLOOD&&
            !(thing->flags&(MF_SHADOW|MF_MISSILE|MF_SPAWNCEILING));
        auto &vertices=batches[-1-lump];
        for(size_t n=vertices.size()-6;n<vertices.size();++n) {
            vertices[n].red=pulse[0];vertices[n].green=pulse[1];vertices[n].blue=pulse[2];vertices[n].statics=packedStatics;
            if(grounded) {vertices[n].mode|=128;vertices[n].sunU=grounding;vertices[n].sunV=vertices[n].z-z+lift;}
            if(soft)vertices[n].sunU=std::clamp(image.width*0.5f,8.0f,64.0f);
        }
    }
    std::sort(casters.begin(),casters.end(),[](const Caster &a,const Caster &b){return a.distance<b.distance;});
    for(size_t n=0;n<std::min(size_t(6),casters.size());++n) {
        const Caster &c=casters[n];
        flashlightSilhouette(*c.thing,*c.image,c.lump,c.flip,c.x,c.y,c.z,camera,*beam);
    }
    // The player's own shadow: the player sprite as seen from the strongest
    // light above (not the flashlight or the player's own shots), or from
    // the sun where the player stands in it.
    mobj_t *self=players[displayplayer].mo;
    if(settings.playerShadow&&camera.effects[2]==0&&self&&self->sprite>=0&&self->sprite<numsprites) {
        float x,y,z;interpolatedPosition(*self,fraction,x,y,z);
        ShadowLight light={};float strongest=0.025f;bool found=false;
        for(unsigned n=0;n<flashes.count;++n) {
            const Flash &f=flashes.lights[n];
            if(fogOnly[n]||lightSources[n]==self||f.direction[3]>0||f.position[2]<=z+8)continue;
            float amount=flashAt(x,y,z+28,f);
            if(amount>strongest) {strongest=amount;light={f.position[0],f.position[1],f.position[2],amount};found=true;}
        }
        if(bakeOnlyActive||gridLightActive()) {
            StaticSample statics;ShadowLight fixedLight;
            bakedLightAt(x,y,z+28,self->subsector->sector,&statics);
            if(staticShadow(statics,z,fixedLight)&&fixedLight.strength>strongest) {strongest=fixedLight.strength;light=fixedLight;found=true;}
        }
        if(!found&&settings.sun&&sunLevel>0&&!contactCells.empty()) {
            int cx=std::clamp((int)std::floor((x-mapOrigin[0])/mapCell),0,mapWidth-1),cy=std::clamp((int)std::floor((y-mapOrigin[1])/mapCell),0,mapHeight-1);
            float visible=contactCells[4*((size_t)cy*mapWidth+cx)+2]/255.0f;
            if(visible>0.5f) {light={x+sunAzimuth[0]*766,y+sunAzimuth[1]*766,z+643,0.6f*visible};found=true;}
        }
        int frame=self->frame&FF_FRAMEMASK;
        if(found&&frame<sprites[self->sprite].numframes) {
            const auto &sf=sprites[self->sprite].spriteframes[frame];
            unsigned rotation=0;
            if(sf.rotate) {
                angle_t angle=(angle_t)(int64_t)(atan2(light.y-y,light.x-x)*(4294967296.0/(2*doomPi)));
                rotation=(angle-self->angle+(unsigned)(ANG45/2)*9)>>29;
            }
            int lump=firstspritelump+sf.lump[rotation];
            addEnemyShadow(*self,lumpImage(lump,false),lump,sf.flip[rotation],x,y,z,camera,&light,true);
        }
    }
    buildBlood(camera);
    buildHeat(camera);
    buildDust(camera);
}
// One target-color ray at 30 Hz supplies a smooth, subtle ambient tint.
// It creates no virtual lights, portal blocker lists, or per-pixel bounce work.
void updateFlashlightTint(Uniforms &camera) {
    uint64_t now=SDL_GetTicksNS();
    if(flashlightLevelSerial!=r_levelserial) {
        flashlightTint={};flashlightTargetTint={};flashlightTime=0;flashlightSampleTime=0;
        flashlightLevelSerial=r_levelserial;
    }
    if(!flashlightOn||camera.effects[2]!=0||settings.flashlightTintGain==0)flashlightTargetTint={};
    else if(!flashlightSampleTime||now-flashlightSampleTime>=33333333) {
        flashlightSampleTime=now;
        const byte *palette=(const byte*)W_CacheLumpName((char*)"PLAYPAL",PU_CACHE);
        using Vec=std::array<float,3>;
        auto dot=[](Vec a,Vec b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
        auto cross=[](Vec a,Vec b)->Vec{return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};
        Vec direction;
        for(int c=0;c<3;++c) direction[c]=camera.forward[c];
        float length=std::sqrt(dot(direction,direction));for(float &c:direction)c/=length;
        float nearest=900;Vec reflected={};
        for(const auto &batch:batches) {
            const auto &image=images.at(batch.first);const auto &vertices=batch.second;
            for(size_t i=0;i+2<vertices.size();i+=3) {
                const auto &a=vertices[i],&b=vertices[i+1],&c=vertices[i+2];
                Vec ab={b.x-a.x,b.y-a.y,b.z-a.z},ac={c.x-a.x,c.y-a.y,c.z-a.z};
                Vec h=cross(direction,ac);float det=dot(ab,h);if(std::abs(det)<0.0001f)continue;
                Vec origin={camera.eye[0]-a.x,camera.eye[1]-a.y,camera.eye[2]-a.z};
                float u=dot(origin,h)/det;if(u<0||u>1)continue;
                Vec q=cross(origin,ab);float v=dot(direction,q)/det;if(v<0||u+v>1)continue;
                float distance=dot(ac,q)/det;if(distance<1||distance>=nearest)continue;
                int tx=(int)std::floor(a.u+u*(b.u-a.u)+v*(c.u-a.u));
                int ty=(int)std::floor(a.v+u*(b.v-a.v)+v*(c.v-a.v));
                if(a.mode&2) {if(tx<0||ty<0||tx>=image.width||ty>=image.height)continue;}
                else {tx=(tx%image.width+image.width)%image.width;ty=(ty%image.height+image.height)%image.height;}
                size_t pixel=((size_t)ty*image.width+tx)*2;
                if(!image.pixels[pixel+1])continue;
                nearest=distance;
                // Average a small patch of artwork so a single dark mortar
                // pixel doesn't abruptly change the tint while aiming.
                Vec albedo={};float samples=0;
                for(int py=-2;py<=2;++py)for(int px=-2;px<=2;++px) {
                    int x=(tx+px+image.width)%image.width,y=(ty+py+image.height)%image.height;
                    size_t index=((size_t)y*image.width+x)*2;
                    if(!image.pixels[index+1])continue;
                    const byte *rgb=palette+image.pixels[index]*3;
                    for(int c=0;c<3;++c)albedo[c]+=rgb[c]/255.0f;
                    ++samples;
                }
                for(float &c:albedo)c/=std::max(1.0f,samples);
                float gain=0.30f*std::pow(std::max(0.0f,1-distance/900),2.0f)*settings.flashlightTintGain;
                for(int c=0;c<3;++c)reflected[c]=albedo[c]*gain;
            }
        }
        flashlightTargetTint=reflected;
    }
    float seconds=flashlightTime?std::min(0.1f,(now-flashlightTime)/1e9f):1.0f/60;
    flashlightTime=now;float blend=1-std::exp(-seconds/0.16f);
    for(int c=0;c<3;++c) {
        flashlightTint[c]+=(flashlightTargetTint[c]-flashlightTint[c])*blend;
        camera.flashlightTint[c]=camera.effects[2]==0?flashlightTint[c]:0;
    }
}

void selectFogLights(const Uniforms &camera) {
    fogLights={};if(!settings.fog)return;
    std::vector<std::pair<float,unsigned>> candidates;
    for(unsigned i=0;i<flashes.count;++i) {
        const auto &light=flashes.lights[i];
        float distance=std::hypot(light.position[0]-camera.eye[0],light.position[1]-camera.eye[1]);
        if(distance>light.position[3]+512)continue;
        float score=light.direction[3]>0?1000:light.strength/(1+distance/128);
        candidates.emplace_back(score,i);
    }
    std::stable_sort(candidates.begin(),candidates.end(),[](auto a,auto b){return a.first>b.first;});
    for(size_t i=0;i<std::min(size_t(4),candidates.size());++i)
        fogLights.indices[fogLights.count++]=candidates[i].second;
}
void beforeTic() {
    if(gamestate!=GS_LEVEL||!players[displayplayer].mo||paused) {tickTime=0;return;}
    mobj_t *mo=players[displayplayer].mo;
    oldEye[0]=units(mo->x);oldEye[1]=units(mo->y);oldEye[2]=units(players[displayplayer].viewz);
    oldYaw=(float)(mo->angle*(2.0*doomPi/4294967296.0));tickTime=SDL_GetTicksNS();
    oldThings.clear();
    oldHeights.resize(numsectors);
    for(int i=0;i<numsectors;++i) oldHeights[i]={units(sectors[i].floorheight),units(sectors[i].ceilingheight)};
    for(int i=0;i<numsectors;++i) for(mobj_t *thing=sectors[i].thinglist;thing;thing=thing->snext)
        oldThings.emplace(thing,std::array<float,3>{units(thing->x),units(thing->y),units(thing->z)});
    watchSplashes();
}
void acceleratedView(player_t *player) {
    (void)player; worldPending=true; v_overlayactive=true;
    memset(v_overlaymask,255,sizeof(v_overlaymask));
    for(int y=viewwindowy;y<viewwindowy+viewheight;++y) {
        memset(screens[0]+y*SCREENWIDTH+viewwindowx,0,scaledviewwidth);
        memset(v_overlaymask+y*SCREENWIDTH+viewwindowx,0,scaledviewwidth);
    }
}
void applySettings() {
    R_AcceleratedView=settings.accelerated?acceleratedView:nullptr;
    R_BeforeTic=settings.accelerated?beforeTic:nullptr;
    R_MuzzleFlash=settings.accelerated?muzzleFlash:nullptr;
    R_WallImpact=settings.accelerated?wallImpact:nullptr;
    R_BloodHit=settings.accelerated?bloodHit:nullptr;
    if(!settings.accelerated) flashPulses.clear();
    r_uncapped=settings.accelerated;
    if(!settings.look) pitch=0;
    R_SetViewSize(10,0);
}
void loadSettings() {
    FILE *file=fopen(graphicsConfig,"r"); if(!file) return;
    char key[64];float value;
    while(fscanf(file,"%63s %f",key,&value)==2) {
        if(!strcmp(key,"fov")) settings.fov=std::clamp(value,60.0f,120.0f);
        else if(!strcmp(key,"flashlight_tint")) settings.flashlightTintGain=std::clamp(value,0.0f,2.0f);
        else if(!strcmp(key,"scale")) settings.scale=value==50?50:value==75?75:100;
        else if(!strcmp(key,"sprite_filter")) settings.spriteFilter=value==0?0:value==1?1:2;
        else if(!strcmp(key,"sun_shadows")) settings.sun=value!=0;
        else if(!strcmp(key,"baked_lights")) settings.bakedLights=value!=0;
        else if(!strcmp(key,"bounce_light")) settings.bounce=value!=0;
        else if(!strcmp(key,"caustics")) settings.caustics=value!=0;
        else if(!strcmp(key,"filter")) settings.filter=std::clamp((int)value,0,2);
        else if(!strcmp(key,"sharp_softness")) settings.sharpSoftness=std::clamp(value,0.0f,1.0f);
        else if(!strcmp(key,"palette_mipmaps")) settings.paletteMips=value!=0;
        else if(!strcmp(key,"detail_textures")) settings.detailTextures=std::clamp((int)value,0,2);
        else if(!strcmp(key,"detail_strength")) settings.detailStrength=std::clamp(value,0.05f,0.6f);
        else if(!strcmp(key,"detail_scale")) settings.detailScale=std::clamp(std::round(value),2.0f,8.0f);
        else if(!strcmp(key,"detail_fade")) settings.detailFade=std::clamp(value,32.0f,256.0f);
        else {
            int v=value!=0;
            if(!strcmp(key,"accelerated"))settings.accelerated=v;
            if(!strcmp(key,"widescreen"))settings.widescreen=v;
            if(!strcmp(key,"crosshair"))settings.crosshair=v;
            if(!strcmp(key,"look"))settings.look=v;
            if(!strcmp(key,"retro"))settings.retro=v;
            if(!strcmp(key,"fps"))settings.fps=v;
            if(!strcmp(key,"emissive"))settings.emissive=v;
            if(!strcmp(key,"fog"))settings.fog=v;
            if(!strcmp(key,"palette"))settings.palette=v;
            if(!strcmp(key,"surface_detail"))settings.detail=v;
            if(!strcmp(key,"soft_light"))settings.softLight=v;
            if(!strcmp(key,"reflections"))settings.reflections=v;
            if(!strcmp(key,"retro_reflections"))settings.retroReflections=v;
            if(!strcmp(key,"blood"))settings.blood=v;
            if(!strcmp(key,"blood_shine"))settings.bloodShine=v;
            if(!strcmp(key,"flashlight_shadows"))settings.flashlightShadows=v;
            if(!strcmp(key,"soft_effects"))settings.softSprites=v;
            if(!strcmp(key,"heat_haze"))settings.heatHaze=v;
            if(!strcmp(key,"eye_adaptation"))settings.eyeAdaptation=v;
            if(!strcmp(key,"splashes"))settings.splashes=v;
            if(!strcmp(key,"dust_motes"))settings.dust=v;
            if(!strcmp(key,"player_shadow"))settings.playerShadow=v;
            if(!strcmp(key,"door_light"))settings.doorLight=v;
            if(!strcmp(key,"moving_relight"))settings.movingRelight=v;
            if(!strcmp(key,"texel_lighting"))settings.texelLight=v;
            if(!strcmp(key,"sky_light"))settings.skyLight=v;
            if(!strcmp(key,"baked_occlusion"))settings.bakedAO=v;
            if(!strcmp(key,"decoration_shadows"))settings.thingShadows=v;
            if(!strcmp(key,"light_flow"))settings.lightFlow=v;
            if(!strcmp(key,"ceiling_caustics"))settings.ceilingCaustics=v;
            if(!strcmp(key,"caustics_computed"))settings.causticsComputed=v;
            if(!strcmp(key,"caustics_grow"))settings.causticsGrow=v;
            if(!strcmp(key,"caustics_angle"))settings.causticsAngle=v;
            if(!strcmp(key,"caustics_sway"))settings.causticsSway=v;
            if(!strcmp(key,"caustics_sprites"))settings.causticsSprites=v;
            if(!strcmp(key,"caustics_shots"))settings.causticsShots=v;
            if(!strcmp(key,"damp_shores"))settings.dampShores=v;
            if(!strcmp(key,"glossy_screens"))settings.glossyScreens=v;
            if(!strcmp(key,"sun_shafts"))settings.sunShafts=v;
            if(!strcmp(key,"sun_disc"))settings.sunDisc=v;
            if(!strcmp(key,"sun_scatter"))settings.sunScatter=v;
            if(!strcmp(key,"varied_highlights"))settings.variedHighlights=v;
            if(!strcmp(key,"bake_only_lights"))settings.bakeOnlyLights=v;
            if(!strcmp(key,"grid_sprite_light"))settings.gridSpriteLight=v;
            if(!strcmp(key,"unoccluded_surface_lights"))settings.unoccludedSurfaceLights=v;
        }
    } fclose(file);
}
void saveSettings() {
    FILE *file=fopen(graphicsConfig,"w"); if(!file)return;
    fprintf(file,"accelerated %d\nwidescreen %d\nfilter %d\ncrosshair %d\nlook %d\nretro %d\nfps %d\nscale %d\nfov %.1f\nsprite_filter %d\nemissive %d\nflashlight_tint %.2f\nfog %d\npalette %d\nsurface_detail %d\nsoft_light %d\nreflections %d\nretro_reflections %d\nsun_shadows %d\nbaked_lights %d\nbounce_light %d\ncaustics %d\n"
        "blood %d\nblood_shine %d\nflashlight_shadows %d\nsoft_effects %d\nheat_haze %d\neye_adaptation %d\nsplashes %d\ndust_motes %d\nplayer_shadow %d\ndoor_light %d\nmoving_relight %d\ntexel_lighting %d\n"
        "sky_light %d\nbaked_occlusion %d\ndecoration_shadows %d\nlight_flow %d\nceiling_caustics %d\ncaustics_computed %d\ncaustics_grow %d\ncaustics_angle %d\ncaustics_sway %d\ncaustics_sprites %d\ncaustics_shots %d\ndamp_shores %d\nglossy_screens %d\nsun_shafts %d\nsun_disc %d\nsun_scatter %d\nvaried_highlights %d\n"
        "bake_only_lights %d\ngrid_sprite_light %d\nunoccluded_surface_lights %d\ndetail_textures %d\ndetail_strength %.2f\ndetail_scale %.0f\ndetail_fade %.0f\nsharp_softness %.2f\npalette_mipmaps %d\n",
        settings.accelerated,settings.widescreen,settings.filter,settings.crosshair,settings.look,settings.retro,settings.fps,settings.scale,settings.fov,settings.spriteFilter,settings.emissive,settings.flashlightTintGain,settings.fog,settings.palette,settings.detail,settings.softLight,settings.reflections,settings.retroReflections,settings.sun,settings.bakedLights,settings.bounce,settings.caustics,
        settings.blood,settings.bloodShine,settings.flashlightShadows,settings.softSprites,settings.heatHaze,settings.eyeAdaptation,settings.splashes,
        settings.dust,settings.playerShadow,settings.doorLight,settings.movingRelight,settings.texelLight,
        settings.skyLight,settings.bakedAO,settings.thingShadows,settings.lightFlow,settings.ceilingCaustics,settings.causticsComputed,settings.causticsGrow,settings.causticsAngle,settings.causticsSway,settings.causticsSprites,settings.causticsShots,settings.dampShores,settings.glossyScreens,settings.sunShafts,settings.sunDisc,settings.sunScatter,settings.variedHighlights,
        settings.bakeOnlyLights,settings.gridSpriteLight,settings.unoccludedSurfaceLights,settings.detailTextures,settings.detailStrength,settings.detailScale,settings.detailFade,settings.sharpSoftness,settings.paletteMips);
    fclose(file);
}
void settingsChanged() {applySettings();saveSettings();tickTime=0;}
void sceneInit(SDL_Window *window) {
    gameWindow=window;
    int config=M_CheckParm((char*)"-graphics");
    if(config&&config+1<myargc)graphicsConfig=myargv[config+1];
    loadSettings();if(M_CheckParm((char*)"-classic"))settings.accelerated=0;
    if(M_CheckParm((char*)"-rendercheck"))settings.accelerated=1;
    int fov=M_CheckParm((char*)"-fov");if(fov&&fov+1<myargc)settings.fov=std::clamp((float)atof(myargv[fov+1]),60.0f,120.0f);
    flashlightOn=M_CheckParm((char*)"-flashlight")!=0;
    int tintOption=M_CheckParm((char*)"-flashlighttint");
    if(tintOption&&tintOption+1<myargc)
        settings.flashlightTintGain=std::clamp((float)atof(myargv[tintOption+1]),0.0f,2.0f);
    int fogOption=M_CheckParm((char*)"-fog");
    if(fogOption&&fogOption+1<myargc)settings.fog=atoi(myargv[fogOption+1])!=0;
    int paletteOption=M_CheckParm((char*)"-palette");
    if(paletteOption&&paletteOption+1<myargc)settings.palette=atoi(myargv[paletteOption+1])!=0;
}
void sceneShutdown() {
    finishRelight(false);
    R_AcceleratedView=nullptr;R_BeforeTic=nullptr;R_MuzzleFlash=nullptr;R_WallImpact=nullptr;R_BloodHit=nullptr;r_uncapped=0;v_overlayactive=0;
    flashPulses.clear();lightBlockers.clear();flashes={};
    flashlightTint={};flashlightTargetTint={};flashlightTime=0;flashlightSampleTime=0;
    glowColors.clear();images.clear();floors.clear();floorBounds.clear();sectorFloors.clear();batches.clear();shadowBatches.clear();oldThings.clear();
    surfaceLights.clear();surfaceSelection.clear();flatLightSamples.clear();surfaceLightTime=0;
    burstBarrels.clear();scorches.clear();seamCells.clear();contactCells.clear();bakeStrips.clear();bakeOrder.clear();wallBakeCells.clear();flatLightCells.clear();bakeMap={};
    bakeSources.clear();bakeGrid.clear();bakeAreas.clear();sourceGroups.clear();staticGrid.clear();staticFog.clear();
    wallDirectionCells.clear();flatDirectionCells.clear();wallDirectionTexture=nullptr;flatDirectionTexture=nullptr;bakeOnlyActive=false;
    wallBakeTexture=nullptr;flatLightTexture=nullptr;causticTexture=nullptr;causticPattern=nullptr;shoreTexture=nullptr;sunBaked=false;sunLevel=0;lightBakeKey=-1;bounceKey=-1;ambientKey=-1;flowKey=-1;bakeThingsKey=-1;skyLevel=0;bakedLightsActive=false;seamTexture=nullptr;contactTexture=nullptr;oldHeights.clear();wallDecals.clear();debris.clear();decalBatches.clear();particleVertices.clear();
    sectorMist.clear();mistVertices.clear();skyVertices.clear();cloudTexture=nullptr;sunShafts.clear();shaftVertices.clear();skyOpeningOf.clear();skyOpenings.clear();
    bloodFloors.clear();bloodWalls.clear();bloodPools.clear();pooledCorpses.clear();bloodShades.clear();bloodShadesLoaded=false;
    splashWatch.clear();rings.clear();heatVertices.clear();doorPortals.clear();relightPending.clear();steadyLights.clear();adaptedLight=-1;exposure=1;
}
// Binds a batch texture with its next animation frame and crossfade amount.
// The engine switches frames before leveltime++, so the phase uses leveltime-1.
SurfaceBinding surfaceBinding(int key) {
    Image &image=images.at(key);
    const Image *next=&image;float blend=0;
    int pic=key>=0?key:-1-key-firstflat,nextPic=0;
    bool flat=key<0&&pic>=0&&pic<numflats;
    int speed=key>=0||flat?P_PicAnimationNext(key>=0,pic,&nextPic):0;
    // Detail textures: walls and flats pick a grain from their own colors
    // once; animated ones (liquids, falls, fire) take none.
    if((key>=0||flat)&&image.detail<0) {
        const byte *palette=(const byte*)W_CacheLumpName((char*)"PLAYPAL",PU_CACHE);
        image.detail=doom_detail_class(image.width,image.height,image.pixels.data(),palette);
    }
    float detail=(key>=0||flat)&&speed==0?float((image.detail&3)+1):0,detailSwap=image.detail>=4?1.0f:0.0f;
    if((key>=0||flat)&&leveltime>0) {
        if(speed>0) {
            const Image &candidate=key>=0?wallImage(nextPic):lumpImage(firstflat+nextPic,true);
            if(candidate.width==image.width&&candidate.height==image.height) {
                next=&candidate;blend=((leveltime-1)%speed+renderFraction)/speed;
            }
        }
    }
    return {&image,next,blend,detail,detailSwap};
}
// A sphere that misses a triangle's bounds cannot illuminate any fragment
// of it. The conservative mask preserves every light inside its radius.
void assignLightMasks() {
    for(auto &batch:batches) {
        auto &vertices=batch.second;
        for(size_t n=0;n+2<vertices.size();n+=3) {
            unsigned mask[2]={};
            if(!(vertices[n].mode&2)||(vertices[n].mode&8)) {
                const auto &a=vertices[n],&b=vertices[n+1],&c=vertices[n+2];
                float low[3]={std::min({a.x,b.x,c.x}),std::min({a.y,b.y,c.y}),std::min({a.z,b.z,c.z})};
                float high[3]={std::max({a.x,b.x,c.x}),std::max({a.y,b.y,c.y}),std::max({a.z,b.z,c.z})};
                for(unsigned i=0;i<flashes.count;++i) {
                    const auto &light=flashes.lights[i];
                    if(!fogOnly[i]&&doom_flash_reaches_bounds(&light,low,high))mask[i/32]|=1u<<(i%32);
                }
            }
            for(size_t v=n;v<n+3;++v)std::copy(mask,mask+2,vertices[v].lightMask);
        }
    }
}
void screenQuad(std::vector<Vertex> &out,float x,float y,float w,float h,float u0,float v0,float u1,float v1,float light,unsigned mode) {
    quad(out,{x,y,0,u0,v0,light,mode},{x+w,y,0,u1,v0,light,mode},
        {x+w,y-h,0,u1,v1,light,mode},{x,y-h,0,u0,v1,light,mode});
}
FrameView prepareFrame(int w,int h) {
    FrameView view={};Uniforms &camera=view.camera;
    camera.projection[2]=1;camera.projection[3]=32768;
    camera.effects[0]=settings.filter;camera.effects[1]=settings.retro;
    camera.texFilter[0]=settings.sharpSoftness;camera.texFilter[1]=settings.paletteMips&&settings.filter;
    camera.effects[2]=players[displayplayer].fixedcolormap;
    camera.effects[3]=settings.spriteFilter;camera.materials[0]=settings.emissive;
    // Bit flags: 1 surface detail, 2 retro reflections, 4 caustics, 8 texel-aligned bake,
    // 16 ceiling caustics, 32 light flow, 64 baked occlusion, 128 glossy screens,
    // caustics: 256 computed layers, 512 growing shapes, 1024 reflection angle, 2048 sway, 4096 sprites,
    // 8192 shots; 16384 damp shores; 32768 varied highlights.
    camera.flashlightTint[3]=(settings.detail?1:0)+(settings.retroReflections?2:0)+(settings.caustics&&causticTexture?4:0)+(settings.texelLight?8:0)+
        (settings.caustics&&settings.ceilingCaustics&&causticTexture?16:0)+(settings.lightFlow&&flowKey>=0&&flowKey==bakeThingsKey?32:0)+
        (settings.bakedAO&&ambientKey>=0&&ambientKey==bakeThingsKey?64:0)+(settings.glossyScreens?128:0)+
        (settings.causticsComputed?256:0)+(settings.causticsGrow?512:0)+(settings.causticsAngle?1024:0)+(settings.causticsSway?2048:0)+
        (settings.causticsSprites?4096:0)+(settings.causticsShots?8192:0)+(settings.dampShores&&shoreTexture?16384:0)+
        (settings.detail&&settings.variedHighlights?32768:0);
    camera.map[0]=mapOrigin[0];camera.map[1]=mapOrigin[1];camera.map[2]=1/mapCell;
    camera.map[3]=settings.softLight&&seamTexture&&worldPending;
    camera.materials[2]=settings.fog&&worldPending;
    camera.materials[3]=settings.palette&&worldPending;
    // No bloom while the in-game settings panel covers the view: it would glow
    // through the dimmed backdrop and soften the panel's text.
    camera.materials[1]=settings.emissive&&worldPending&&camera.effects[2]==0&&!I_Render3DSettingsOpen();
    camera.fx[0]=1;
    // Detail textures: smooth grain with smooth walls and floors, stepped
    // grain over crisp pixels only when asked for.
    bool detailed=settings.detailTextures==2||(settings.detailTextures==1&&settings.filter);
    camera.detail[0]=detailed&&worldPending?settings.detailStrength:0;
    camera.detail[1]=settings.detailScale;camera.detail[2]=settings.detailFade;
    float uiWidth=std::min((float)w,h*4.0f/3),uiHeight=uiWidth*0.75f,uiX=(w-uiWidth)/2;
    float uiY=worldPending&&settings.widescreen?h-uiHeight:(h-uiHeight)/2;
    float worldX=uiX+uiWidth*viewwindowx/320,worldY=uiY+uiHeight*viewwindowy/200.0f;
    float worldW=uiWidth*scaledviewwidth/320,worldH=uiHeight*viewheight/200.0f;
    if(settings.widescreen&&scaledviewwidth==320)
    { worldX=0;worldY=0;worldW=w;worldH=h-uiHeight*(1-viewheight/200.0f); }
    float referenceAspect=(4.0f/3)*(scaledviewwidth/320.0f)/(viewheight/200.0f);
    camera.projection[1]=referenceAspect/tan(settings.fov*doomPi/360);
    camera.projection[0]=camera.projection[1]/(worldW/std::max(1.0f,worldH));
    camera.fx2[0]=worldH/200;
    float fraction=1;
    if(worldPending) {
        if(levelSerial!=r_levelserial||floors.empty())buildMap();
        syncBakeThings();
        relightMovingSectors();
        if(settings.sun&&!sunBaked)bakeSun();
        bool sun=settings.sun&&sunLevel>0;
        for(int c=0;c<3;++c)camera.sun[c]=sun?sunTint[c]:0;
        camera.sun[3]=sun?sunLevel:0;
        camera.fx2[2]=sunYaw;camera.fx2[3]=sun&&settings.sunDisc?sunGlow:0;
        bakedLightsActive=settings.bakedLights&&lightBakeKey==lightBakeInputs();
        bakeOnlyActive=bakedLightsActive&&(lightBakeKey&2);
        camera.bake[0]=bakedLightsActive;camera.bake[2]=bakeOnlyActive&&wallDirectionTexture&&flatDirectionTexture;
        for(int g=0;g<flickerGroups;++g)camera.flicker[g/4][g%4]=bakeOnlyActive?groupFlicker(g):1;
        player_t *player=&players[displayplayer];mobj_t *mo=player->mo;
        camera.eye[0]=units(mo->x);camera.eye[1]=units(mo->y);camera.eye[2]=units(player->viewz);
        float yaw=(float)(mo->angle*(2.0*doomPi/4294967296.0));
        if(tickTime&&!paused&&!menuactive&&!demoplayback) {
            fraction=std::clamp((SDL_GetTicksNS()-tickTime)/(1e9/TICRATE),0.0,1.0);
            if(std::hypot(camera.eye[0]-oldEye[0],camera.eye[1]-oldEye[1])<128) {
                for(int i=0;i<3;++i)camera.eye[i]=oldEye[i]+(camera.eye[i]-oldEye[i])*fraction;
                yaw=oldYaw+std::remainder(yaw-oldYaw,2*doomPi)*fraction;
            }
        }
        float look=demoplayback?0:pitch;
        camera.right[0]=sin(yaw);camera.right[1]=-cos(yaw);
        camera.forward[0]=cos(yaw)*cos(look);camera.forward[1]=sin(yaw)*cos(look);camera.forward[2]=sin(look);
        camera.up[0]=-cos(yaw)*sin(look);camera.up[1]=-sin(yaw)*sin(look);camera.up[2]=cos(look);
        camera.fog[0]=0.00045f*(player->mo->subsector->sector->ceilingpic==skyflatnum?0.35f:1.0f);
        camera.fog[1]=0.012f;camera.fog[2]=fogReferenceHeight;
        camera.fog[3]=(leveltime+fraction)/TICRATE;
        renderFraction=fraction;
        // Caustics follow the liquid's frame and crossfade like its floor (surfaceBinding).
        if(!causticFrames.empty()&&causticSpeed>0&&leveltime>0) {
            auto at=std::find(causticFrames.begin(),causticFrames.end(),flattranslation[causticFrames[0]]);
            camera.texFilter[2]=at!=causticFrames.end()?float(at-causticFrames.begin()):0;
            camera.texFilter[3]=((leveltime-1)%causticSpeed+fraction)/causticSpeed;
        }
        geometry(camera,fraction);
        // Surface lights come from this frame's geometry; the bake applies from the next frame.
        if(settings.bakedLights&&lightBakeKey!=lightBakeInputs())bakeLights();
        if(settings.bakedLights&&settings.gridSpriteLight&&lightBakeKey==lightBakeInputs()&&staticSerial!=lightBakeSerial)bakeStaticGrid();
        if(settings.bounce&&bounceKey!=bounceInputs())bakeBounce();
        if((settings.skyLight||settings.bakedAO)&&ambientKey!=bakeThingsKey)bakeAmbient();
        if(settings.lightFlow&&flowKey!=bakeThingsKey)bakeFlow();
        camera.bake[1]=settings.bounce&&bounceKey>=0?bounceStrength:0;
        bool ambient=ambientKey==bakeThingsKey;
        camera.fx2[1]=settings.skyLight&&ambient?skyLevel:0;
        for(int c=0;c<3;++c)camera.fx[1+c]=skyTint[c];
        buildMist(camera);
        buildShafts(camera);
        buildRings(camera);
        updateExposure(camera);
        updateFlashlightTint(camera);
        selectFogLights(camera);
    }
    if(!worldPending) {mistVertices.clear();shaftVertices.clear();heatVertices.clear();skyVertices.clear();}
    assignLightMasks();
    view.uiX=uiX;view.uiY=uiY;view.uiWidth=uiWidth;view.uiHeight=uiHeight;
    view.worldX=worldX;view.worldY=worldY;view.worldW=worldW;view.worldH=worldH;
    return view;
}
std::vector<unsigned> hudPixels(const unsigned *pixels) {
    std::vector<unsigned> overlay(pixels,pixels+SCREENWIDTH*SCREENHEIGHT);
    if(worldPending) for(int i=0;i<SCREENWIDTH*SCREENHEIGHT;++i)overlay[i]=(overlay[i]&0xffffff)|(unsigned)v_overlaymask[i]<<24;
    return overlay;
}
// Current sector light and heights for seam blending and contact shading;
// w is 0 indoors, and under a sky packOutdoor.
std::vector<std::array<float,4>> sectorInfo() {
    std::vector<std::array<float,4>> info(std::max(1,numsectors));
    if(worldPending)for(int i=0;i<numsectors;++i)
        info[i]={lighting(&sectors[i]),floorZ(&sectors[i]),ceilZ(&sectors[i]),
            sectors[i].ceilingpic==skyflatnum?packOutdoor(&sectors[i]):0.0f};
    return info;
}
std::vector<SpriteDraw> weaponDraws(const Uniforms &camera) {
    std::vector<SpriteDraw> draws;
    player_t *player=&players[displayplayer];
    for(int i=0;i<NUMPSPRITES;++i) {
        auto psp=player->psprites[i];if(!psp.state)continue;
        auto sf=sprites[psp.state->sprite].spriteframes[psp.state->frame&FF_FRAMEMASK];
        Image &image=lumpImage(firstspritelump+sf.lump[0],false);
        float x=units(psp.sx)-image.left,y=viewwindowy+viewheight/2.0f-100+units(psp.sy)-image.top;
        std::vector<Vertex> weapon;
        float u0=sf.flip[0]?image.width:0,u1=sf.flip[0]?0:image.width;
        screenQuad(weapon,x/160-1,1-y/100,image.width/160.0f,image.height/100.0f,u0,0,u1,image.height,
            (psp.state->frame&FF_FULLBRIGHT)?1.0f:sunLightAt(camera.eye[0],camera.eye[1],player->mo->subsector->sector,
                flowLightAt(camera.eye[0],camera.eye[1],player->mo->subsector->sector,
                seamLightAt(camera.eye[0],camera.eye[1],player->mo->subsector->sector,lighting(player->mo->subsector->sector)))),3);
        // The weapon takes a restrained, capped share of the light around the
        // eye so nearby lamps tint it without washing out the artwork.
        if(!(psp.state->frame&FF_FULLBRIGHT)) {
            auto baked=bakedLightAt(camera.eye[0],camera.eye[1],camera.eye[2],player->mo->subsector->sector);
            for(float &c:baked)c=std::min(0.5f,c*0.4f);
            for(auto &vertex:weapon) {vertex.red=baked[0];vertex.green=baked[1];vertex.blue=baked[2];}
        }
        draws.push_back({&image,std::move(weapon)});
    }
    return draws;
}
std::vector<Vertex> crosshairVertices(float worldW,float worldH) {
    std::vector<Vertex> cross;
    if(!worldPending||!settings.crosshair||menuactive)return cross;
    float px=2/worldW,py=2/worldH;
    screenQuad(cross,-7*px,py,5*px,2*py,0,0,1,1);screenQuad(cross,2*px,py,5*px,2*py,0,0,1,1);
    screenQuad(cross,-px,7*py,2*px,5*py,0,0,1,1);screenQuad(cross,-px,-2*py,2*px,5*py,0,0,1,1);
    return cross;
}
bool renderCheckDue() {return M_CheckParm((char*)"-rendercheck")&&worldPending&&gametic>10;}
void saveScreenshot(const void *bgra,int width,int height,int stride) {
    SDL_CreateDirectory("Screenshots");
    char path[256];snprintf(path,sizeof(path),"Screenshots/DOOM-%llu.png",(unsigned long long)SDL_GetTicksNS());
    int test=M_CheckParm((char*)"-rendercheck");
    const char *output=test&&test+1<myargc?myargv[test+1]:path;
    SDL_Surface *surface=bgra?SDL_CreateSurfaceFrom(width,height,SDL_PIXELFORMAT_BGRA32,(void*)bgra,stride):nullptr;
    bool saved=surface&&SDL_SavePNG(surface,output);SDL_DestroySurface(surface);
    if(!test) players[consoleplayer].message=(char*)(saved?"SCREENSHOT SAVED":"SCREENSHOT FAILED");
    fprintf(stderr,"Screenshot %s: %s (%dx%d).\n",saved?"saved":"failed",output,width,height);
    if(test) {if(!saved)I_Error((char*)"Render check could not save image");I_Quit();}
}
void updateTitle(int width,int height,const char *renderer) {
    ++frameCount;
    uint64_t now=SDL_GetTicksNS();
    if(now-titleTime>1000000000) {
        fps=frameCount*1e9/(now-titleTime);frameCount=0;titleTime=now;
        char rate[32]="";if(settings.fps)snprintf(rate,sizeof(rate)," · %.0f fps",fps);
        char title[256];snprintf(title,sizeof(title),"DOOM · %s · %dx%d%s · F: flashlight · F4: graphics · F7: screenshot · Esc: menu",
            settings.accelerated?renderer:"Classic",width,height,rate);
        SDL_SetWindowTitle(gameWindow,title);
    }
}
} // namespace doom3d

// Backend-independent parts of the renderer interface.
using namespace doom3d;
void I_Render3DStartFrame(void) {worldPending=false;v_overlayactive=false;}
int I_Render3DIsAccelerated(void) {return settings.accelerated;}
void I_Render3DLook(float delta) {if(settings.look&&settings.accelerated&&!demoplayback)pitch=std::clamp(pitch-delta*0.002f,-1.2f,1.2f);}
void I_Render3DToggleFlashlight(void) {if(settings.accelerated)flashlightOn=!flashlightOn;}
void I_Render3DScreenshot(void) {screenshotPending=true;}
