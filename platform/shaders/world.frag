#version 450
#extension GL_GOOGLE_include_directive : require
// World and sprite shading; OPAQUE enables early depth for full-coverage textures.
#ifdef OPAQUE
layout(early_fragment_tests) in;
#endif
#include "common.glsl"
layout(set=2,binding=0) uniform sampler2D image;
layout(set=2,binding=1) uniform sampler2D palette;
layout(set=2,binding=2) uniform sampler2D nextImage;
// RGBA16 cells as UNORM: SDL_gpu cannot sample integer formats.
layout(set=2,binding=3) uniform sampler2D seams;
layout(set=2,binding=4) uniform sampler2D contact;
layout(set=2,binding=5) uniform sampler2D reflection;
layout(set=2,binding=6) uniform sampler3D paletteLUT;
// Baked lighting: walls in an atlas (rgb light at half scale, a sun; bounce in
// the right half), and four map-space layers stacked in rows: floor light,
// ceiling light, floor bounce, ceiling bounce (rgb at half scale).
layout(set=2,binding=7) uniform sampler2D wallBake;
layout(set=2,binding=8) uniform sampler2D flatLight;
// Caustics: map cells near liquids hold the liquid's tint and a proximity;
// the pattern (R8, 64x64) comes from the level's main liquid flat.
layout(set=2,binding=9) uniform sampler2D causticMap;
layout(set=2,binding=10) uniform sampler2D causticPattern;
// Bake-only lights: where static light comes from (see encodeDirection in
// scene3d.cpp), walls at the wall bake's light coordinates, floors then
// ceilings in map space.
layout(set=2,binding=11) uniform sampler2D wallDirection;
layout(set=2,binding=12) uniform sampler2D flatDirection;
// Monitor glass (platform/screen_glass.h): texture alpha below 255 marks
// glass; this holds, per glass texel, its x and y from its screen's top-left
// texel, then the screen's width and height.
layout(set=2,binding=13) uniform sampler2D glassFrame;
// Detail textures (platform/detail_texture.h): grayscale grain centred on
// 0.5, one layer per material, with mips that fade back to 0.5.
layout(set=2,binding=14) uniform sampler2DArray detailMaps;
// Storage buffers follow the samplers in SPIR-V but the four uniform blocks in
// SDL_gpu's Metal layout; METAL selects the second numbering.
#ifdef METAL
#define BLOCKER_BINDING 4
#define SECTOR_BINDING 5
#else
#define BLOCKER_BINDING 15
#define SECTOR_BINDING 16
#endif
layout(std430,set=2,binding=BLOCKER_BINDING) readonly buffer Blockers { LightBlocker blockers[]; };
layout(std430,set=2,binding=SECTOR_BINDING) readonly buffer Sectors { vec4 sectorInfo[]; };
layout(std140,set=3,binding=0) uniform Camera CAMERA_BLOCK c;
layout(std140,set=3,binding=1) uniform Lights FLASH_BLOCK;
layout(std140,set=3,binding=2) uniform Fog FOG_BLOCK;
// surfaceDetail: 1 + the surface's detail layer, 0 for none; detailSwap:
// its fibres run along u.
layout(std140,set=3,binding=3) uniform Blend { float blend,surfaceDetail,detailSwap; };
layout(location=0) out vec4 outColor;
#define BLOCKERS
#include "indexed.glsl"
#include "xbr.glsl"
#include "lighting.glsl"

// Surface detail: a Sobel filter over texel luminance tilts the face normal,
// so dynamic lights rake across mortar and panel seams. Kept deliberately low.
float texelLuma(ivec2 p) {
    return dot(texel(image,palette,p,true).rgb,vec3(0.299,0.587,0.114));
}
// Cotangent frame from screen derivatives: directions of increasing u and
// v, scaled together so the longer is unit length (one texel per map unit).
// Derivatives are taken once in uniform control flow and passed in.
void textureFrame(vec3 normal,vec3 dp1,vec3 dp2,vec2 duv1,vec2 duv2,out vec3 tangent,out vec3 bitangent) {
    vec3 dp2perp=cross(dp2,normal),dp1perp=cross(normal,dp1);
    tangent=dp2perp*duv1.x+dp1perp*duv2.x;bitangent=dp2perp*duv1.y+dp1perp*duv2.y;
    float scale=inversesqrt(max(1e-12,max(dot(tangent,tangent),dot(bitangent,bitangent))));
    tangent*=scale;bitangent*=scale;
}
bool isGlass(vec2 uv) {
    return texelFetch(image,wrapTexel(ivec2(floor(uv)),textureSize(image,0)),0).a<0.999;
}
// Position across the screen in [-1,1], and the screen's top-left texel and
// size in texels.
vec2 glassScreen(vec2 uv,out vec2 origin,out vec2 extent) {
    vec4 f=texelFetch(glassFrame,wrapTexel(ivec2(floor(uv)),textureSize(image,0)),0)*255.0;
    origin=floor(uv)-round(f.rg);extent=max(round(f.ba),vec2(1));
    return clamp((uv-origin)/extent*2.0-1.0,vec2(-1),vec2(1));
}
// The CRT dome, peaking at a quarter of the shorter side: its smooth normal.
vec3 glassDome(vec2 screen,vec2 extent,vec3 normal,vec3 tangent,vec3 bitangent) {
    float peak=min(extent.x,extent.y)*0.25;
    vec2 slope=-peak*4.0*screen*(1.0-screen.yx*screen.yx)/extent;
    return normalize(normal-(slope.x*tangent+slope.y*bitangent));
}
// Tilts the face normal against a height slope per texel along u and v.
vec3 tiltNormal(vec3 normal,vec2 slope,vec3 dp1,vec3 dp2,vec2 duv1,vec2 duv2) {
    vec3 tangent,bitangent;textureFrame(normal,dp1,dp2,duv1,duv2,tangent,bitangent);
    return normalize(normal-(slope.x*tangent+slope.y*bitangent));
}
vec3 detailNormal(vec2 uv,vec3 normal,vec3 dp1,vec3 dp2,vec2 duv1,vec2 duv2) {
    ivec2 p=ivec2(floor(uv));
    float tl=texelLuma(p+ivec2(-1,-1)),t=texelLuma(p+ivec2(0,-1)),tr=texelLuma(p+ivec2(1,-1));
    float l=texelLuma(p+ivec2(-1,0)),r=texelLuma(p+ivec2(1,0));
    float bl=texelLuma(p+ivec2(-1,1)),b=texelLuma(p+ivec2(0,1)),br=texelLuma(p+ivec2(1,1));
    vec2 slope=vec2(tr+2.0*r+br-tl-2.0*l-bl,bl+2.0*b+br-tl-2.0*t-tr)*0.25;
    return tiltNormal(normal,slope*0.9,dp1,dp2,duv1,duv2);
}
// Crude materials from the palette's ramps: greys and blues read as metal,
// greens as wet slime or panels; browns, tans and flesh stay matte.
float paletteGloss(uint index) {
    if(index>=80u&&index<112u) return 0.35;   // Greys.
    if(index>=192u&&index<208u) return 0.30;  // Blues.
    if(index>=240u&&index<248u) return 0.20;  // Dark blues.
    if(index>=112u&&index<128u) return 0.18;  // Greens.
    if(index>=160u&&index<168u) return 0.12;  // Pale yellows.
    return 0.03;
}
// Wall and flat texture reads (c.effects.x: 0 crisp, 1 bilinear, 2 sharp
// bilinear). pixel: texels per screen pixel along u and v. Sharp blends each
// texel edge over one screen pixel plus c.texFilter.x texels, so near views
// keep flat texels with clean edges. Once texels shrink below a pixel,
// palette mip levels (c.texFilter.y) take over; between levels only the
// middle half of each step blends, so the nearer level stays crisp longer.
vec4 surfaceLevel(sampler2D source,vec2 uv,vec2 pixel,int level) {
    vec2 scale=vec2(textureSize(source,level))/vec2(textureSize(source,0));
    uv*=scale;
    if(c.effects.x==2.0) uv=sharpen(uv,pixel*scale+c.texFilter.x);
    return indexedAt(source,palette,uv,c.effects.x>0.0,true,level);
}
vec4 surfaceTexture(sampler2D source,vec2 uv,vec2 pixel,float lod) {
    int top=textureQueryLevels(source)-1;
    if(c.texFilter.y==0.0||lod<=0.25||top==0) return surfaceLevel(source,uv,pixel,0);
    lod=min(lod,float(top));
    int level=int(floor(lod));float t=smoothstep(0.25,0.75,lod-float(level));
    vec4 near=surfaceLevel(source,uv,pixel,level);
    if(t<=0.0||level>=top) return near;
    vec4 far=surfaceLevel(source,uv,pixel,level+1);
    near.rgb*=near.a;far.rgb*=far.a;
    vec4 result=mix(near,far,t);
    if(result.a>0.0) result.rgb/=result.a;
    return result;
}
// Detail textures, as in late-90s engines: a fine grain multiplied over the
// artwork only up close. It fades out by c.detail.z units and where its
// pixels shrink toward screen pixels, so normal views keep the original look.
// Smooth walls take a trilinear grain; crisp pixels take one grain value per
// detail cell, snapped to the texel grid and stepped in 1/16 like colormaps.
float detailGrain(vec2 uv,float footprint,vec2 duv1,vec2 duv2) {
    float scale=c.detail.y,layer=surfaceDetail-1.0;
    if(detailSwap>0.5) {uv=uv.yx;duv1=duv1.yx;duv2=duv2.yx;}
    float fade=1.0-smoothstep(c.detail.z*0.4,c.detail.z,vDistance);
    float size=footprint*scale; // Detail pixels per screen pixel.
    if(c.effects.x>0.0) {
        fade*=1.0-smoothstep(1.0,2.0,size);
        if(fade<=0.0) return 1.0;
        vec2 k=vec2(scale/128.0);
        float d=textureGrad(detailMaps,vec3(uv*k,layer),duv1*k,duv2*k).r-0.5;
        return 1.0+d*2.0*c.detail.x*fade;
    }
    fade*=1.0-smoothstep(0.3,0.6,size);
    if(fade<=0.0) return 1.0;
    ivec2 q=ivec2(floor(uv*scale))&127;
    float d=texelFetch(detailMaps,ivec3(q,int(layer)),0).r-0.5;
    return 1.0+round(d*2.0*c.detail.x*fade*16.0)/16.0;
}
// Sector light blended across open lines, from cells of this sector or of
// the neighbor it blends with. Other cells (void, unrelated sectors) are skipped.
float seamLight(vec2 xy,uint sector,float fallback) {
    vec2 g=(xy-c.map.xy)*c.map.z-0.5;ivec2 q=ivec2(floor(g));vec2 f=fract(g);
    ivec2 size=textureSize(seams,0);
    float value=0.0,total=0.0;
    for(int n=0;n<4;++n) {
        ivec2 o=ivec2(n&1,n>>1);
        uvec4 cell=uvec4(round(texelFetch(seams,clamp(q+o,ivec2(0),size-1),0)*65535.0));
        if(cell.x!=sector&&cell.y!=sector) continue;
        float w=(o.x!=0?f.x:1.0-f.x)*(o.y!=0?f.y:1.0-f.y);
        float l=sectorInfo[cell.x].x;
        if(cell.y!=0xFFFFu) l=mix(l,sectorInfo[cell.y].x,float(cell.z)/65535.0);
        value+=l*w;total+=w;
    }
    return total>0.0001?value/total:fallback;
}
// Contact shading: floors and ceilings darken near walls, walls near their
// sector's floor and (non-sky) ceiling. Sector heights follow moving floors.
float contactShade(vec3 world,vec3 normal,uint sector) {
    vec2 uv=(world.xy-c.map.xy)*c.map.z/vec2(textureSize(contact,0));
    vec4 info=sectorInfo[sector];
    if(normal.z>0.5) return 1.0-0.35*(1.0-smoothstep(0.0,1.0,textureLod(contact,uv,0.0).r));
    if(normal.z<-0.5) return 1.0-0.25*(1.0-smoothstep(0.0,1.0,textureLod(contact,uv,0.0).g));
    float shade=1.0-0.3*(1.0-smoothstep(0.0,40.0,world.z-info.y));
    if(info.w==0.0) shade*=1.0-0.2*(1.0-smoothstep(0.0,24.0,info.z-world.z));
    return shade;
}
// Baked surface data: rgb static light, a sun visibility (negative where a
// surface has none, as on ceilings), and bounce light. Floors keep their sun
// in the contact map's blue channel.
// Texel-aligned (flag 8): the four nearest cells blend smoothly, read at the
// center of the texture pixel the fragment lies in, so light steps follow
// the artwork's own pixels instead of the bake grid. Floors skip cells of
// other sectors like seamLight; walls stay inside their atlas strip, whose
// origin and scale arrive in vTint. Otherwise one nearest sample per cell.
// ambient: sky visibility and openness, packed in the bounce alpha as two
// 16-step nibbles (bakeAmbient). direction: with bake-only lights (bake.z),
// the light direction texel, blended like the light except its flicker
// group (alpha), which comes from the nearest cell.
vec2 unpackAmbient(float packed) {
    float v=round(packed*255.0);
    return vec2(floor(v/16.0),mod(v,16.0))/15.0;
}
vec4 bakedSurface(vec3 normal,uint sector,out vec3 bounce,out vec2 ambient,out vec4 direction) {
    bounce=vec3(0);ambient=vec2(0,1);direction=vec4(0.5,0.5,0,0);
    bool texelAligned=(uint(c.flashlightTint.w)&8u)!=0u,directed=c.bake.z>0.0;
    if(abs(normal.z)>0.5&&texelAligned) {
        ivec2 lightSize=textureSize(flatLight,0),layer=max(lightSize/ivec2(1,4),ivec2(1));
        int row=normal.z<-0.5?layer.y:0;
        vec2 g=(floor(vWorld.xy)+0.5-c.map.xy)*c.map.z-0.5;
        ivec2 q=ivec2(floor(g));vec2 f=fract(g);
        vec3 light=vec3(0),bounced=vec3(0),toward=vec3(0);vec2 open=vec2(0);float sun=0.0,total=0.0,heaviest=0.0,group=0.0;
        int directionRow=normal.z<-0.5?layer.y:0;
        for(int n=0;n<4;++n) {
            ivec2 o=ivec2(n&1,n>>1),cell=clamp(q+o,ivec2(0),layer-1);
            if(uint(round(texelFetch(seams,cell,0).x*65535.0))!=sector) continue;
            float w=(o.x!=0?f.x:1.0-f.x)*(o.y!=0?f.y:1.0-f.y)+1e-4;
            light+=texelFetch(flatLight,cell+ivec2(0,row),0).rgb*w;
            vec4 b=texelFetch(flatLight,min(cell+ivec2(0,row+2*layer.y),lightSize-1),0);
            bounced+=b.rgb*w;open+=unpackAmbient(b.a)*w;
            sun+=texelFetch(contact,cell,0).b*w;total+=w;
            if(directed) {
                vec4 d=texelFetch(flatDirection,cell+ivec2(0,directionRow),0);
                toward+=d.rgb*w;
                if(w>heaviest) {heaviest=w;group=d.a;}
            }
        }
        if(total>0.0) {
            bounce=bounced/total*2.0;ambient=open/total;
            if(directed) direction=vec4(toward/total,group);
            return vec4(light/total*2.0,normal.z>0.5?sun/total:-1.0);
        }
    }
    if(abs(normal.z)>0.5) {
        ivec2 cell=ivec2(floor((vWorld.xy-c.map.xy)*c.map.z)),size=textureSize(contact,0);
        float sun=normal.z>0.5?texelFetch(contact,clamp(cell,ivec2(0),size-1),0).b:-1.0;
        ivec2 lightSize=textureSize(flatLight,0),layer=max(lightSize/ivec2(1,4),ivec2(1));
        ivec2 lightCell=clamp(cell,ivec2(0),layer-1)+ivec2(0,normal.z<-0.5?layer.y:0);
        vec4 b=texelFetch(flatLight,min(lightCell+ivec2(0,2*layer.y),lightSize-1),0);
        bounce=b.rgb*2.0;ambient=unpackAmbient(b.a);
        if(directed) direction=texelFetch(flatDirection,clamp(cell,ivec2(0),layer-1)+ivec2(0,normal.z<-0.5?layer.y:0),0);
        return vec4(texelFetch(flatLight,min(lightCell,lightSize-1),0).rgb*2.0,sun);
    }
    if(vSun.x<0.0) return vec4(0,0,0,-1);
    ivec2 size=textureSize(wallBake,0),lightSize=max(size/ivec2(2,1),ivec2(1));
    if(texelAligned&&vTint.z>0.0&&(vMode&256u)==0u) {
        // Wall texture u runs along the wall, v down from the anchor.
        vec2 offset=vec2(floor(vUV.x)+0.5-vUV.x,vUV.y-floor(vUV.y)-0.5);
        vec2 g=vSun+offset*vTint.z-0.5;
        ivec2 q=ivec2(floor(g)),origin=ivec2(vTint.xy);vec2 f=fract(g);
        vec4 baked=vec4(0);vec3 bounced=vec3(0),toward=vec3(0);vec2 open=vec2(0);float heaviest=-1.0,group=0.0;
        for(int n=0;n<4;++n) {
            ivec2 o=ivec2(n&1,n>>1),cell=clamp(max(q+o,origin),ivec2(0),lightSize-1);
            float w=(o.x!=0?f.x:1.0-f.x)*(o.y!=0?f.y:1.0-f.y);
            baked+=texelFetch(wallBake,cell,0)*w;
            vec4 b=texelFetch(wallBake,min(cell+ivec2(lightSize.x,0),size-1),0);
            bounced+=b.rgb*w;open+=unpackAmbient(b.a)*w;
            if(directed) {
                vec4 d=texelFetch(wallDirection,cell,0);
                toward+=d.rgb*w;
                if(w>heaviest) {heaviest=w;group=d.a;}
            }
        }
        bounce=bounced*2.0;ambient=open;
        if(directed) direction=vec4(toward,group);
        return vec4(baked.rgb*2.0,baked.a);
    }
    ivec2 cell=clamp(ivec2(floor(vSun)),ivec2(0),lightSize-1);
    vec4 b=texelFetch(wallBake,min(cell+ivec2(lightSize.x,0),size-1),0);
    bounce=b.rgb*2.0;ambient=unpackAmbient(b.a);
    if(directed) direction=texelFetch(wallDirection,cell,0);
    vec4 baked=texelFetch(wallBake,cell,0);
    return vec4(baked.rgb*2.0,baked.a);
}
// Sky sectors: x how enclosed their opening is (0 open air, 1 a hole in a
// ceiling), y the outdoor floor; unpacks packOutdoor in scene3d.cpp.
vec2 outdoorShape(uint sector) {
    float v=sectorInfo[sector].w-1.0;
    return vec2(floor(v)/255.0,round(fract(v)*256.0)/255.0);
}
// Sky sectors drop whole Doom light steps in shadow; indoor surfaces rise
// toward the outdoor level where sun falls through an opening, and so do
// enclosed sky sectors, by their enclosure. share is the sunlit part of the
// result, which takes the sun's tint. Keep sunLightAt in scene3d.cpp equivalent.
float sunLight(uint sector,float light,float visible,out float share) {
    float lift=round(max(0.0,c.sun.w-light)*visible*16.0)/16.0;
    if(sectorInfo[sector].w>0.0) {
        float shaded=light-round(light*0.3*(1.0-visible)*16.0)/16.0;
        float result=round(mix(shaded,light+lift,outdoorShape(sector).x)*16.0)/16.0;
        share=max(0.0,result-light)/max(result,0.001);
        return result;
    }
    share=lift/max(light+lift,0.001);
    return light+lift;
}
// Sector light flow (bakeFlow): the cell's light rises toward the current
// light of the sector it sees through openings, in 1/16 steps. Walls read
// the cell one step toward their visible side. Keep flowLightAt in
// scene3d.cpp equivalent.
float flowLight(vec3 world,vec3 normal,uint sector,float light) {
    vec2 at=abs(normal.z)>0.5?world.xy:world.xy+normal.xy/c.map.z;
    if((uint(c.flashlightTint.w)&8u)!=0u&&abs(normal.z)>0.5) at=floor(at)+0.5;
    ivec2 cell=clamp(ivec2(floor((at-c.map.xy)*c.map.z)),ivec2(0),textureSize(seams,0)-1);
    uvec4 s=uvec4(round(texelFetch(seams,cell,0)*65535.0));
    if(s.x!=sector||s.w==0xFFFFu) return light;
    return light+round(max(0.0,sectorInfo[s.w].x-light)*texelFetch(contact,cell,0).a*0.8*16.0)/16.0;
}
// Walls: liquids sit in pits, so the light plays on their sides. Two
// copies of the pattern drift across each other in whole texels at the tic
// rate, one texel per unit like the flats; their crossings flare. Walls
// sample one cell toward their visible side and fade with height above
// their floor. Ceilings (flag 16) take the pattern in map space and fade as
// their sector gets taller.
vec3 caustics(vec3 world,vec3 normal,uint sector,float light) {
    bool ceiling=normal.z<-0.5;
    vec2 at=ceiling?world.xy:world.xy+normal.xy/c.map.z;
    vec4 cell=textureLod(causticMap,(at-c.map.xy)*c.map.z/vec2(textureSize(causticMap,0)),0.0);
    if(cell.a<0.004) return vec3(0);
    vec4 info=sectorInfo[sector];
    float fade=ceiling?1.0-smoothstep(32.0,224.0,info.z-info.y):1.0-smoothstep(0.0,96.0,world.z-info.y);
    ivec2 p=ceiling?ivec2(floor(world.xy)):ivec2(floor(vec2(dot(world.xy,vec2(-normal.y,normal.x)),-world.z)));
    ivec2 size=textureSize(causticPattern,0);
    float tic=floor(c.fog.w*35.0);
    float a=texelFetch(causticPattern,wrapTexel(p+ivec2(floor(tic*vec2(0.23,0.14))),size),0).r;
    float b=texelFetch(causticPattern,wrapTexel(ivec2(p.y,-p.x)+ivec2(floor(tic*vec2(-0.17,0.2))),size),0).r;
    float ripple=round(saturate(a*b*2.0+(a+b)*0.25)*4.0)/4.0;
    return cell.rgb*ripple*cell.a*fade*0.3*(0.35+0.65*saturate(light));
}
void main() {
    bool sprite=(vMode&2u)!=0u;
    vec3 dp1=dFdx(vWorld),dp2=dFdy(vWorld);vec2 duv1=dFdx(vUV),duv2=dFdy(vUV);
    float footprint=max(length(duv1),length(duv2));
    // Mip level from the shorter axis, allowing 2:1 stretch along the longer
    // one so floors at grazing angles stay sharper.
    vec2 texelPixel=abs(duv1)+abs(duv2);
    float lod=log2(max(min(length(duv1),length(duv2)),footprint*0.5));
    // Map surfaces are planar, so screen derivatives give the exact face
    // normal; orient it toward the eye because either side may be drawn.
    vec3 normal=normalize(cross(dp1,dp2));
    if(dot(normal,c.eye.xyz-vWorld)<0.0) normal=-normal;
    // Mode 256: blood decals. vTint carries wetness (shine) and how far the
    // color has darkened while drying.
    bool blood=(vMode&256u)!=0u;
    uint bakeFlags=uint(c.flashlightTint.w);
    // Flag 128: monitor glass in computer textures. Its curved CRT dome
    // shapes light highlights and an edge sheen, and like a weak lens it
    // enlarges the picture's middle slightly (edges still meet the rim).
    bool glassy=!sprite&&!blood&&(bakeFlags&128u)!=0u&&c.effects.z==0.0&&isGlass(vUV);
    vec3 screenNormal=normal;vec2 screen=vec2(0),uv=vUV;
    if(glassy) {
        vec2 origin,extent;screen=glassScreen(vUV,origin,extent);
        vec3 tangent,bitangent;textureFrame(normal,dp1,dp2,duv1,duv2,tangent,bitangent);
        screenNormal=glassDome(screen,extent,normal,tangent,bitangent);
        vec2 lensed=screen*(0.88+0.12*screen*screen);
        uv=clamp(origin+(lensed*0.5+0.5)*extent,origin+0.5,origin+extent-0.5);
    }
    vec4 color;
    if(sprite&&c.effects.w==2.0&&footprint<1.0) color=xbrSprite(image,palette,vUV);
    else if(sprite) color=indexed(image,palette,uv,c.effects.w==1.0||(c.effects.w==2.0&&footprint>=1.0),false);
    else color=surfaceTexture(image,uv,texelPixel,lod);
    // Animated flats and walls crossfade toward their next frame.
    if(blend>0.0) color=mix(color,surfaceTexture(nextImage,uv,texelPixel,lod),blend);
    if(color.a<0.45) discard;
    if(blood) color.rgb*=vTint.g;
    // The thick rim of the glass shades the picture's edges, in light steps.
    if(glassy) color.rgb*=round((1.0-0.5*smoothstep(0.7,1.0,max(abs(screen.x),abs(screen.y))))*16.0)/16.0;
    float grain=1.0;
    if(!sprite&&!blood&&!glassy&&surfaceDetail>0.0&&c.detail.x>0.0&&c.effects.z==0.0&&(vMode&5u)==0u)
        grain=detailGrain(uv,footprint,duv1,duv2);
    float light=vLight,sunShare=0.0;
    // Map surfaces carry sector+1 in mode bits 12+; 0 means no sector.
    bool mapped=(vMode>>12)>0u&&!sprite&&c.effects.z==0.0;
    uint sector=(vMode>>12)-1u;
    if(mapped&&c.map.w>0.0&&abs(normal.z)>0.5) light=seamLight(vWorld.xy,sector,light);
    if(mapped&&(bakeFlags&32u)!=0u) light=flowLight(vWorld,normal,sector,light);
    if(mapped&&c.map.w>0.0) light*=contactShade(vWorld,normal,sector);
    vec3 bounce=vec3(0);vec2 ambient=vec2(0,1);vec4 direction=vec4(0.5,0.5,0,0);
    vec4 baked=mapped?bakedSurface(normal,sector,bounce,ambient,direction):vec4(0,0,0,-1);
    // Under a sky, sun shadow and sky occlusion stop at the light of the
    // indoor sectors the sector opens into (see outdoorShape).
    float outdoorFloor=mapped&&sectorInfo[sector].w>0.0?min(light,outdoorShape(sector).y):0.0;
    if(mapped&&c.sun.w>0.0&&baked.a>=0.0) light=sunLight(sector,light,baked.a,sunShare);
    // Sky light: outdoors, surfaces that see less sky (under overhangs, in
    // alcoves) drop light steps and the rest take a little of the sky's tint;
    // indoors, surfaces that see sky through openings rise toward the sky
    // sectors' light, the lift taking the tint; enclosed sky sectors blend
    // toward the indoor rule by their enclosure. All in 1/16 steps.
    float skyShare=0.0;
    if(mapped&&c.fx2.y>0.0) {
        float lift=round(max(0.0,c.fx2.y-light)*saturate(ambient.x*1.2)*0.7*16.0)/16.0;
        float indoorShare=lift/max(light+lift,0.001);
        if(sectorInfo[sector].w>0.0) {
            float enclosed=outdoorShape(sector).x;
            float shaded=light-round(light*0.25*(1.0-ambient.x)*16.0)/16.0;
            light=round(mix(shaded,light+lift,enclosed)*16.0)/16.0;
            skyShare=mix(0.3*ambient.x,indoorShare,enclosed);
        } else {skyShare=indoorShare;light+=lift;}
    }
    light=max(light,outdoorFloor);
    // Baked occlusion (flag 64) darkens the sector and baked light, not dynamic lights.
    float occlusion=1.0;
    if(mapped&&(bakeFlags&64u)!=0u) {occlusion=round((1.0-0.45*(1.0-ambient.y))*16.0)/16.0;light*=occlusion;}
    if(c.effects.z==0.0) light*=clamp(1.0-vDistance/3200.0,0.35,1.0);
    else light=1.0;
    // Grounding: things resting on the floor darken over their lowest 24
    // units in colormap-like 1/16 steps; vSun holds strength and height.
    float grounding=1.0;
    if(sprite&&(vMode&128u)!=0u&&c.effects.z==0.0)
        grounding=round(mix(1.0,mix(0.72,1.0,smoothstep(0.0,24.0,vSun.y)),vSun.x)*16.0)/16.0;
    light*=grounding;
    // Preserve the sprite's painted shading with one RGB increment for the
    // whole billboard; world surfaces evaluate colored lighting per pixel.
    vec3 illumination=vec3(light)*mix(vec3(1),c.sun.rgb,sunShare)*mix(vec3(1),c.fx.yzw,skyShare),specular=vec3(0);
    if(c.effects.z==0.0) {
        // Sprites carry their baked light (and, without detail, their
        // dynamic light) in the tint.
        vec3 pulse=sprite?vTint*grounding:vec3(0);
        if(sprite&&(vMode&8u)!=0u) {
            // Billboards face the camera horizontally and stand upright.
            ivec2 size=textureSize(image,0);
            vec2 packed=texelFetch(image,clamp(ivec2(floor(vUV)),ivec2(0),size-1),0).ba*2.0-1.0;
            if((vMode&16u)!=0u) packed.x=-packed.x;
            vec3 toward=normalize(vec3(-c.forward.x,-c.forward.y,0.0001));
            vec3 across=vec3(c.right.xy,0);
            vec3 n=normalize(across*packed.x-vec3(0,0,1)*packed.y+toward*sqrt(saturate(1.0-dot(packed,packed))));
            vec3 view=normalize(c.eye.xyz-vWorld);
            uvec2 mask=vLightMask;
            // Mode 2048: airborne blood glints on its rounded normals like the
            // floor blood, in a brighter palette red and two hard steps.
            bool wetSprite=(vMode&2048u)!=0u;
            vec3 wetColor=wetSprite?texelFetch(paletteLUT,ivec3(round(saturate(color.rgb*1.8+0.04)*31.0)),0).rgb:vec3(0);
            if(c.bake.z>0.0&&vStatic!=0u) {
                // Bake-only lights: the static light in the tint takes the
                // same shaping as a dynamic light from where it mostly comes.
                vec4 statics=unpackUnorm4x8(vStatic);
                vec3 toLight=normalize(statics.xyz*2.0-1.0);
                float facing=saturate(dot(n,toLight));
                float rim=pow(1.0-saturate(dot(n,view)),2.0)*facing*saturate(0.35-dot(toLight,view));
                vec3 tint=pulse;
                pulse*=mix(1.0,0.4+0.6*facing+1.6*rim,statics.w);
                if(wetSprite) {
                    float highlight=pow(saturate(dot(n,normalize(toLight+view))),24.0);
                    highlight=highlight>0.5?1.0:highlight>0.2?0.5:0.0;
                    specular+=wetColor*tint*statics.w*highlight*0.8;
                }
            }
            while(any(notEqual(mask,uvec2(0)))) {
                uint word=mask.x!=0u?0u:1u;
                uint i=word*32u+uint(findLSB(mask[word]));
                mask[word]&=mask[word]-1u;
                float amount=flashAt(vWorld,i);
                if(amount<=0.0) continue;
                vec3 toLight=normalize(lights[i].position.xyz-vWorld);
                float facing=saturate(dot(n,toLight));
                // Backlit edges: silhouette normals that face the light glow.
                float rim=pow(1.0-saturate(dot(n,view)),2.0)*facing*saturate(0.35-dot(toLight,view));
                // A static light's flat share already arrives as baked tint;
                // this keeps only its shaping across the silhouette.
                float bakedShare=c.bake.x>0.0?lights[i].baked:0.0;
                pulse+=lights[i].color.rgb*amount*(0.4+0.6*facing+1.6*rim-bakedShare);
                if(wetSprite) {
                    float highlight=pow(saturate(dot(n,normalize(toLight+view))),24.0);
                    highlight=highlight>0.5?1.0:highlight>0.2?0.5:0.0;
                    specular+=wetColor*lights[i].color.rgb*amount*highlight*0.8;
                }
            }
        }
        if(!sprite) {
            uvec2 mask=vLightMask;
            // Bake-only lights: the bake's direction (share in b) stands in
            // for the static lights' dynamic copies, so its light also takes
            // bump detail and gloss where it is bright enough to show them.
            bool useBake=mapped&&c.bake.x>0.0;
            vec3 staticLight=useBake?baked.rgb*occlusion:vec3(0);
            float staticShare=useBake&&c.bake.z>0.0?direction.b:0.0;
            bool staticDetail=staticShare>0.05&&dot(staticLight,vec3(0.299,0.587,0.114))>0.02;
            bool detail=(uint(c.flashlightTint.w)&1u)!=0u&&(any(notEqual(mask,uvec2(0)))||staticDetail);
            // Glass is smooth: its bulge replaces the artwork's bump detail,
            // and it takes a tighter, stronger highlight than painted metal.
            vec3 bumped=glassy?screenNormal:detail?detailNormal(vUV,normal,dp1,dp2,duv1,duv2):normal;
            float gloss=0.0,shininess=glassy?64.0:24.0;
            if(glassy) gloss=0.9;
            else if(detail) {
                ivec2 p=wrapTexel(ivec2(floor(vUV)),textureSize(image,0));
                gloss=paletteGloss(uint(round(texelFetch(image,p,0).r*255.0)));
            }
            vec3 view=normalize(c.eye.xyz-vWorld);
            // Wet blood glints only under dynamic lights, in a brighter palette
            // color of its own red rather than white: a small, sharp highlight
            // in two hard steps, so a flat splat never lights up as a whole.
            bool wet=blood&&vTint.r>0.0;
            vec3 wetColor=wet?texelFetch(paletteLUT,ivec3(round(saturate(color.rgb*1.8+0.04)*31.0)),0).rgb:vec3(0);
            // Static lights come from the bake; their dynamic copies only add
            // what it lacks: flicker, bump detail and gloss. With bake-only
            // lights the bake supplies those itself: flicker by group (alpha:
            // group*16+share), and shading from where the light comes from.
            // A pool's own surface (alpha without a group: its share) is lit
            // by the glow hugging it, not from above: it takes no overhead
            // highlight but a sheen where its ripples mirror the horizon,
            // which shows from afar at grazing angles.
            if(useBake&&c.bake.z>0.0) {
                uint packed=uint(round(direction.a*255.0)),group=packed>>4u;
                float sheenShare=group==0u?float(packed&15u)/15.0:0.0;
                if(group>0u) staticLight*=1.0-float(packed&15u)/15.0*(1.0-c.flicker[group>>2u][group&3u]);
                if(staticShare>0.0) {
                    vec2 frame=direction.rg*2.0-1.0;
                    vec3 t=abs(normal.z)>0.5?vec3(1,0,0):vec3(-normal.y,normal.x,0);
                    vec3 b=abs(normal.z)>0.5?vec3(0,1,0):vec3(0,0,1);
                    vec3 toLight=normalize(t*frame.x+b*frame.y+normal*sqrt(saturate(1.0-dot(frame,frame))));
                    vec3 shaded=staticLight;
                    if(detail) {
                        float flatFacing=mix(1.0,saturate(dot(normal,toLight)),0.75);
                        float bumpFacing=mix(1.0,saturate(dot(bumped,toLight)),0.75);
                        shaded*=mix(1.0,bumpFacing/flatFacing,staticShare);
                    }
                    float grazing=saturate(dot(normal,toLight)*4.0);
                    if(gloss>0.0) {
                        float highlight=pow(saturate(dot(bumped,normalize(toLight+view))),shininess);
                        specular+=staticLight*staticShare*(1.0-sheenShare)*highlight*gloss*grazing;
                        if(sheenShare>0.0) {
                            float horizon=pow(1.0-saturate(dot(reflect(-view,bumped),normal)),6.0);
                            specular+=staticLight*sheenShare*horizon*gloss*1.5;
                        }
                    }
                    if(wet) {
                        float highlight=pow(saturate(dot(bumped,normalize(toLight+view))),40.0);
                        highlight=highlight>0.6?1.0:highlight>0.3?0.5:0.0;
                        specular+=wetColor*staticLight*staticShare*highlight*vTint.r*0.6*grazing;
                    }
                    staticLight=shaded;
                }
            }
            pulse+=staticLight;
            if(mapped) pulse+=bounce*c.bake.y*occlusion;
            while(any(notEqual(mask,uvec2(0)))) {
                uint word=mask.x!=0u?0u:1u;
                uint i=word*32u+uint(findLSB(mask[word]));
                mask[word]&=mask[word]-1u;
                float amount=flashAt(vWorld,i);
                if(amount<=0.0) continue;
                float bakedShare=useBake?lights[i].baked:0.0;
                pulse+=lights[i].color.rgb*amount*(flashFacing(vWorld,bumped,i)-bakedShare*flashFacing(vWorld,normal,i));
                if(gloss>0.0) {
                    vec3 toLight=normalize(lights[i].position.xyz-vWorld);
                    float highlight=pow(saturate(dot(bumped,normalize(toLight+view))),shininess);
                    specular+=lights[i].color.rgb*amount*highlight*gloss*saturate(dot(normal,toLight)*4.0);
                }
                if(wet) {
                    vec3 toLight=normalize(lights[i].position.xyz-vWorld);
                    float highlight=pow(saturate(dot(bumped,normalize(toLight+view))),40.0);
                    highlight=highlight>0.6?1.0:highlight>0.3?0.5:0.0;
                    specular+=wetColor*lights[i].color.rgb*amount*highlight*vTint.r*0.6*saturate(dot(normal,toLight)*4.0);
                }
            }
        }
        illumination=clamp(illumination+pulse+c.flashlightTint.rgb,vec3(0),vec3(2.2));
        if(glassy) {
            // Where the pane curves away from the eye it catches the room's
            // light as a thin sheen, in brightness only, in 1/16 steps.
            float edge=pow(1.0-saturate(dot(screenNormal,normalize(c.eye.xyz-vWorld))),3.0);
            specular+=vec3(round(edge*saturate(dot(illumination,vec3(0.299,0.587,0.114)))*0.45*16.0)/16.0);
        }
    }
    float emission=0.0;
    if(!sprite&&c.materials.x>0.0&&c.effects.z==0.0) {
        // The blue channel carries a mask, independent of palette/coverage.
        vec2 at=c.effects.x==2.0?sharpen(uv,texelPixel+c.texFilter.x):uv;
        emission=maskAt(image,at,c.effects.x>0.0);
        if(blend>0.0) emission=mix(emission,maskAt(nextImage,at,c.effects.x>0.0),blend);
        illumination=max(illumination,vec3(emission));
    }
    // Glowing texels stay clean of the detail grain.
    color.rgb=color.rgb*mix(grain,1.0,emission)*illumination+specular;
    if(mapped&&(bakeFlags&4u)!=0u&&(abs(normal.z)<=0.5||(normal.z<-0.5&&(bakeFlags&16u)!=0u))) color.rgb+=caustics(vWorld,normal,sector,light);
    if((vMode&32u)!=0u&&c.water.z>0.0&&c.effects.z==0.0) {
        // The mirrored camera maps the surface to the same screen point; a
        // scrolling wobble distorts it, and grazing views reflect more.
        // Retro style steps the wobble at 12 Hz, snaps it to whole reflection
        // pixels, skips filtering and quantizes to the WAD palette.
        bool retro=(uint(c.flashlightTint.w)&2u)!=0u;
        float t=retro?floor(c.fog.w*12.0)/12.0:c.fog.w;
        vec2 wobble=vec2(sin(vWorld.x*0.07+t*1.9)+sin(vWorld.y*0.11+t*1.3),
                         cos(vWorld.y*0.08-t*1.6)+cos(vWorld.x*0.05+t*2.1))*(retro?0.005:0.003);
        // Splash rings push the mirror image outward along their crest.
        for(int k=0;k<4;++k) {
            vec4 ring=c.ripple[k];
            if(ring.w<=0.0) continue;
            vec2 away=vWorld.xy-ring.xy;float d=length(away);
            float band=1.0-saturate(abs(d-ring.z)/5.0);
            if(retro) band=step(0.5,band);
            if(band>0.0&&d>0.001) wobble+=away/d*band*ring.w*0.02;
        }
        vec2 uv=(gl_FragCoord.xy-c.water.xy)*c.water.zw+wobble;
        vec3 mirrored;
        if(retro) {
            ivec2 size=textureSize(reflection,0);
            mirrored=texelFetch(reflection,clamp(ivec2(floor(uv*vec2(size))),ivec2(0),size-1),0).rgb;
            mirrored=texelFetch(paletteLUT,ivec3(round(saturate(mirrored)*31.0)),0).rgb;
        } else mirrored=textureLod(reflection,uv,0.0).rgb;
        float grazing=1.0-saturate(abs(normalize(c.eye.xyz-vWorld).z));
        color.rgb=mix(color.rgb,mirrored,0.2+0.4*pow(grazing,3.0));
    }
    if((vMode&1u)==0u) {
        vec4 fog=fogAlong(vWorld);
        color.rgb=color.rgb*fog.a+fog.rgb;
    }
    // Eye adaptation: a stepped exposure while the view settles to new light.
    if(c.effects.z==0.0) color.rgb*=c.fx.x;
    if(c.effects.z==32.0) color.rgb=1.0-color.rgb;
    if((vMode&4u)!=0u) {color.rgb*=0.3;color.a=0.65;}
    if(c.effects.y>0.0) {
        color.rgb*=0.88+0.12*sin(gl_FragCoord.y*3.14159265);
        color.rgb=floor(color.rgb*31.0+0.5)/31.0;
    }
#ifndef OPAQUE
    // Mode 1024: effect sprites (explosions, puffs, fogs). They dissolve on
    // an ordered dither in Doom pixels toward their sector's floor and
    // ceiling, and their depth is pulled toward the eye by vSun.x so walls
    // they stand against no longer slice the billboard.
    float depth=gl_FragCoord.z;
    if((vMode&1024u)!=0u) {
        vec4 info=sectorInfo[(vMode>>12)-1u];
        float room=min(vWorld.z-info.y,info.w>0.0?1e4:info.z-vWorld.z);
        if(saturate(room/12.0)<bayer4(ivec2(floor(gl_FragCoord.xy/max(c.fx2.x,1.0))))) discard;
        float n=c.projection.z,f=c.projection.w,z=max(n,vDistance-vSun.x);
        depth=f/(f-n)-n*f/((f-n)*z);
    }
    gl_FragDepth=depth;
#endif
    outColor=color;
}
