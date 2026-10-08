/* SDL_gpu renderer for the 3D scene in scene3d.cpp: Metal on macOS, Vulkan on
   Linux and Windows. Shaders live in platform/shaders as GLSL and are embedded
   as SPIR-V and MSL by scripts/compile_gpu_shaders.cmake. */
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>
extern "C" {
#include "doomdef.h"
#include "doomstat.h"
#include "r_sky.h"
#include "m_argv.h"
#include "i_system.h"
extern boolean paused;
}
#include <SDL3/SDL.h>
#include "i_render3d.h"
#include "scene3d.h"
#include "gpu_shaders.h"
#include "detail_texture.h"

struct GpuTexture {
    SDL_GPUTexture *texture;
    ~GpuTexture();
};

namespace {
using namespace doom3d;
SDL_GPUDevice *device;
SDL_GPUShaderFormat shaderFormat;
SDL_GPUCommandBuffer *uploadCommand; // Texture uploads, submitted before the frame that uses them.
SDL_GPUTextureFormat depthFormat,swapchainFormat;
SDL_GPUSampleCount sampleCount=SDL_GPU_SAMPLECOUNT_1;
SDL_GPUSampler *linearRepeat,*linearClamp,*nearestClamp,*detailSampler;
SDL_GPUShader *worldVertexShader,*reflectVertexShader,*screenVertexShader,*skyVertexShader;
SDL_GPUGraphicsPipeline *opaquePipeline,*skyPipeline,*skySurfacePipeline,*reflectSkySurfacePipeline,*worldPipeline,*shadowPipeline,*decalPipeline,*particlePipeline,*heatPipeline,
    *weaponPipeline,*hudPipeline,*solidPipeline,*mistPipeline,*shaftPipeline,*overlayWeaponPipeline,*overlayHudPipeline,*overlaySolidPipeline,
    *reflectOpaquePipeline,*reflectSkyPipeline,*reflectWorldPipeline,*emissionPipeline,*blurPipeline,*presentPipeline,*depthPipeline;
SDL_GPUTexture *hudTexture,*paletteTexture,*paletteLUT,*dummySeam,*dummyContact,*dummyBake,*solidTexel,
    *multisampleTexture,*colorTexture,*depthTexture,*sceneDepth,*emissionTexture,*emissionDepth,*bloomScratch,*bloomTexture,
    *reflectionTexture,*reflectionDepth,*detailTexture;
int width,height,reflectionWidth,reflectionHeight;
std::array<unsigned,256> paletteKey;
struct Arena { SDL_GPUBuffer *buffer=nullptr; Uint32 capacity=0; };
Arena vertexArena,blockerArena,sectorArena;
SDL_GPUTransferBuffer *frameTransfer;
Uint32 frameTransferCapacity;
std::unordered_map<const std::vector<Vertex>*,Uint32> vertexOffsets;

SDL_GPUTexture *gpu(const GpuTextureRef &texture) {return texture?texture->texture:nullptr;}
SDL_GPUTexture *createTexture(SDL_GPUTextureFormat format,int w,int h,SDL_GPUTextureUsageFlags usage,
                              SDL_GPUSampleCount samples=SDL_GPU_SAMPLECOUNT_1,SDL_GPUTextureType type=SDL_GPU_TEXTURETYPE_2D,int depth=1) {
    SDL_GPUTextureCreateInfo info={};
    info.type=type;info.format=format;info.usage=usage;info.width=(Uint32)w;info.height=(Uint32)h;
    info.layer_count_or_depth=(Uint32)depth;info.num_levels=1;info.sample_count=samples;
    return SDL_CreateGPUTexture(device,&info);
}
template<class T> void release(T *&object,void (*destroy)(SDL_GPUDevice*,T*)) {if(object&&device)destroy(device,object);object=nullptr;}
void release(SDL_GPUTexture *&texture) {release(texture,SDL_ReleaseGPUTexture);}
void release(SDL_GPUGraphicsPipeline *&pipeline) {release(pipeline,SDL_ReleaseGPUGraphicsPipeline);}
// Uploads go through their own command buffer so they can be recorded at any
// point, even while the frame is inside a render pass.
void upload(SDL_GPUTexture *texture,int w,int h,int depth,const void *pixels,Uint32 bytes,Uint32 pixelsPerRow) {
    SDL_GPUTransferBufferCreateInfo info={SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,bytes,0};
    SDL_GPUTransferBuffer *transfer=SDL_CreateGPUTransferBuffer(device,&info);
    if(!transfer)I_Error((char*)"Could not allocate a GPU upload: %s",SDL_GetError());
    memcpy(SDL_MapGPUTransferBuffer(device,transfer,false),pixels,bytes);
    SDL_UnmapGPUTransferBuffer(device,transfer);
    if(!uploadCommand)uploadCommand=SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(uploadCommand);
    SDL_GPUTextureTransferInfo source={transfer,0,pixelsPerRow,(Uint32)h};
    SDL_GPUTextureRegion target={};
    target.texture=texture;target.w=(Uint32)w;target.h=(Uint32)h;target.d=(Uint32)depth;
    SDL_UploadToGPUTexture(copy,&source,&target,true);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device,transfer);
}
void flushUploads() {
    if(uploadCommand&&!SDL_SubmitGPUCommandBuffer(uploadCommand))
        fprintf(stderr,"GPU upload failed: %s\n",SDL_GetError());
    uploadCommand=nullptr;
}
// Detail texture layers (detail_texture.h) with their full mip chains, so
// distant and grazing surfaces average back to the neutral 0.5.
SDL_GPUTexture *createDetailTexture() {
    SDL_GPUTextureCreateInfo info={};
    info.type=SDL_GPU_TEXTURETYPE_2D_ARRAY;info.format=SDL_GPU_TEXTUREFORMAT_R8_UNORM;info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width=info.height=DOOM_DETAIL_SIZE;info.layer_count_or_depth=DOOM_DETAIL_LAYERS;info.num_levels=DOOM_DETAIL_LEVELS;
    info.sample_count=SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *texture=SDL_CreateGPUTexture(device,&info);
    if(!texture)return nullptr;
    std::vector<byte> levels(doom_detail_bytes());
    if(doom_detail_build(levels.data(),0.16f)!=levels.size()) {SDL_ReleaseGPUTexture(device,texture);return nullptr;}
    SDL_GPUTransferBufferCreateInfo transferInfo={SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,(Uint32)levels.size(),0};
    SDL_GPUTransferBuffer *transfer=SDL_CreateGPUTransferBuffer(device,&transferInfo);
    if(!transfer) {SDL_ReleaseGPUTexture(device,texture);return nullptr;}
    memcpy(SDL_MapGPUTransferBuffer(device,transfer,false),levels.data(),levels.size());
    SDL_UnmapGPUTransferBuffer(device,transfer);
    if(!uploadCommand)uploadCommand=SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(uploadCommand);
    Uint32 offset=0;
    for(Uint32 level=0,size=DOOM_DETAIL_SIZE;level<DOOM_DETAIL_LEVELS;++level,size/=2)
        for(Uint32 layer=0;layer<DOOM_DETAIL_LAYERS;++layer,offset+=size*size) {
            SDL_GPUTextureTransferInfo source={transfer,offset,size,size};
            SDL_GPUTextureRegion target={};
            target.texture=texture;target.mip_level=level;target.layer=layer;target.w=target.h=size;target.d=1;
            SDL_UploadToGPUTexture(copy,&source,&target,false);
        }
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device,transfer);
    return texture;
}
void updatePaletteLUT(const unsigned *palette) {
    if(paletteLUT&&std::equal(palette,palette+256,paletteKey.begin()))return;
    std::copy(palette,palette+256,paletteKey.begin());
    std::vector<unsigned> colors=paletteLUTColors(palette);
    if(!paletteLUT)paletteLUT=createTexture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,32,32,SDL_GPU_TEXTUREUSAGE_SAMPLER,
        SDL_GPU_SAMPLECOUNT_1,SDL_GPU_TEXTURETYPE_3D,32);
    if(!paletteLUT)I_Error((char*)"Could not allocate palette lookup");
    upload(paletteLUT,32,32,32,colors.data(),(Uint32)(colors.size()*4),32);
}

struct ShaderCode { const unsigned char *spirv; size_t spirvSize; const unsigned char *msl; size_t mslSize; };
SDL_GPUShader *shader(const ShaderCode &code,SDL_GPUShaderStage stage,Uint32 samplers,Uint32 storage,Uint32 uniforms) {
    SDL_GPUShaderCreateInfo info={};
    bool msl=shaderFormat==SDL_GPU_SHADERFORMAT_MSL;
    info.code=msl?code.msl:code.spirv;info.code_size=msl?code.mslSize:code.spirvSize;
    // SPIRV-Cross renames the entry point because main is reserved in MSL.
    info.entrypoint=msl?"main0":"main";info.format=shaderFormat;
    info.stage=stage;info.num_samplers=samplers;info.num_storage_buffers=storage;info.num_uniform_buffers=uniforms;
    SDL_GPUShader *result=SDL_CreateGPUShader(device,&info);
    if(!result)fprintf(stderr,"GPU shader: %s\n",SDL_GetError());
    return result;
}
#define SHADER_CODE(name) ShaderCode{doom_spv_##name,sizeof(doom_spv_##name),doom_msl_##name,sizeof(doom_msl_##name)}
#define VERTEX_SHADER(name,uniforms) shader(SHADER_CODE(name),SDL_GPU_SHADERSTAGE_VERTEX,0,0,uniforms)
#define FRAGMENT_SHADER(name,samplers,storage,uniforms) shader(SHADER_CODE(name),SDL_GPU_SHADERSTAGE_FRAGMENT,samplers,storage,uniforms)
// heatBlend writes only alpha, keeping the lowest (hottest) value.
enum Blend { opaqueBlend, alphaBlend, heatBlend, addBlend };
enum Depth { noDepth, depthWrite, depthTest, depthAlways, depthOnly };
// Geometry pipelines read the 60-byte Vertex; screen and sky passes have no vertex input.
SDL_GPUGraphicsPipeline *pipeline(SDL_GPUShader *vertex,SDL_GPUShader *fragment,Blend blend,Depth depth,
                                  SDL_GPUSampleCount samples,SDL_GPUTextureFormat color) {
    if(!vertex||!fragment)return nullptr;
    static const SDL_GPUVertexBufferDescription buffer={0,sizeof(Vertex),SDL_GPU_VERTEXINPUTRATE_VERTEX,0};
    static const SDL_GPUVertexAttribute attributes[]={
        {0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(Vertex,x)},{1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,offsetof(Vertex,u)},
        {2,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT,offsetof(Vertex,light)},{3,0,SDL_GPU_VERTEXELEMENTFORMAT_UINT,offsetof(Vertex,mode)},
        {4,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(Vertex,red)},{5,0,SDL_GPU_VERTEXELEMENTFORMAT_UINT2,offsetof(Vertex,lightMask)},
        {6,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,offsetof(Vertex,sunU)},{7,0,SDL_GPU_VERTEXELEMENTFORMAT_UINT,offsetof(Vertex,statics)}};
    SDL_GPUGraphicsPipelineCreateInfo info={};
    info.vertex_shader=vertex;info.fragment_shader=fragment;
    if(vertex==worldVertexShader||vertex==reflectVertexShader) {
        info.vertex_input_state.vertex_buffer_descriptions=&buffer;info.vertex_input_state.num_vertex_buffers=1;
        info.vertex_input_state.vertex_attributes=attributes;info.vertex_input_state.num_vertex_attributes=8;
    }
    info.primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode=SDL_GPU_FILLMODE_FILL;info.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.enable_depth_clip=true;
    info.multisample_state.sample_count=samples;
    SDL_GPUColorTargetDescription target={};
    target.format=color;
    if(blend==alphaBlend) {
        auto &b=target.blend_state;b.enable_blend=true;
        b.src_color_blendfactor=SDL_GPU_BLENDFACTOR_SRC_ALPHA;b.dst_color_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        b.src_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE;b.dst_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        b.color_blend_op=b.alpha_blend_op=SDL_GPU_BLENDOP_ADD;
    } else if(blend==addBlend) {
        // Light added by alpha; the target's alpha (heat shimmer marks) is kept.
        auto &b=target.blend_state;b.enable_blend=true;
        b.src_color_blendfactor=SDL_GPU_BLENDFACTOR_SRC_ALPHA;b.dst_color_blendfactor=SDL_GPU_BLENDFACTOR_ONE;
        b.src_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ZERO;b.dst_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE;
        b.color_blend_op=b.alpha_blend_op=SDL_GPU_BLENDOP_ADD;
    } else if(blend==heatBlend) {
        auto &b=target.blend_state;b.enable_blend=true;b.enable_color_write_mask=true;b.color_write_mask=SDL_GPU_COLORCOMPONENT_A;
        b.src_color_blendfactor=b.src_alpha_blendfactor=b.dst_color_blendfactor=b.dst_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE;
        b.color_blend_op=SDL_GPU_BLENDOP_ADD;b.alpha_blend_op=SDL_GPU_BLENDOP_MIN;
    }
    if(depth!=depthOnly) {info.target_info.color_target_descriptions=&target;info.target_info.num_color_targets=1;}
    if(depth!=noDepth) {
        info.target_info.has_depth_stencil_target=true;info.target_info.depth_stencil_format=depthFormat;
        info.depth_stencil_state.enable_depth_test=depth!=depthAlways;
        info.depth_stencil_state.enable_depth_write=depth==depthWrite||depth==depthOnly;
        info.depth_stencil_state.compare_op=SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    }
    SDL_GPUGraphicsPipeline *result=SDL_CreateGPUGraphicsPipeline(device,&info);
    if(!result)fprintf(stderr,"GPU pipeline: %s\n",SDL_GetError());
    return result;
}
bool createPipelines() {
    worldVertexShader=VERTEX_SHADER(world_vert,1);reflectVertexShader=VERTEX_SHADER(reflect_vert,2);
    screenVertexShader=VERTEX_SHADER(screen_vert,0);skyVertexShader=VERTEX_SHADER(sky_vert,0);
    SDL_GPUShader *world=FRAGMENT_SHADER(world_frag,15,2,4),*opaque=FRAGMENT_SHADER(opaque_frag,15,2,4);
    SDL_GPUShader *sky=FRAGMENT_SHADER(sky_frag,2,0,3),*skySurface=FRAGMENT_SHADER(sky_surface_frag,2,0,3),*shadow=FRAGMENT_SHADER(shadow_frag,1,0,0),*decal=FRAGMENT_SHADER(decal_frag,1,0,0);
    SDL_GPUShader *particle=FRAGMENT_SHADER(particle_frag,0,0,0),*solid=FRAGMENT_SHADER(solid_frag,0,0,0),*hud=FRAGMENT_SHADER(hud_frag,1,0,0);
    SDL_GPUShader *heat=FRAGMENT_SHADER(heat_frag,0,0,0);
    SDL_GPUShader *emission=FRAGMENT_SHADER(emission_frag,3,0,1),*blur=FRAGMENT_SHADER(blur_frag,1,0,1);
    SDL_GPUShader *mist=FRAGMENT_SHADER(mist_frag,2,0,1),*present=FRAGMENT_SHADER(present_frag,4,0,2),*depth=FRAGMENT_SHADER(depth_frag,1,0,0);
    const auto color=SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,glow=SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    const auto one=SDL_GPU_SAMPLECOUNT_1,msaa=sampleCount;
    SDL_GPUShader *wv=worldVertexShader,*rv=reflectVertexShader,*sv=screenVertexShader,*kv=skyVertexShader;
    opaquePipeline=pipeline(wv,opaque,opaqueBlend,depthWrite,msaa,color);
    skyPipeline=pipeline(kv,sky,opaqueBlend,depthTest,msaa,color);
    skySurfacePipeline=pipeline(wv,skySurface,opaqueBlend,depthWrite,msaa,color);
    worldPipeline=pipeline(wv,world,alphaBlend,depthWrite,msaa,color);
    shadowPipeline=pipeline(wv,shadow,alphaBlend,depthTest,msaa,color);
    decalPipeline=pipeline(wv,decal,alphaBlend,depthTest,msaa,color);
    particlePipeline=pipeline(wv,particle,alphaBlend,depthTest,msaa,color);
    heatPipeline=pipeline(wv,heat,heatBlend,depthTest,msaa,color);
    weaponPipeline=pipeline(wv,world,alphaBlend,depthAlways,msaa,color);
    hudPipeline=pipeline(wv,hud,alphaBlend,depthAlways,msaa,color);
    solidPipeline=pipeline(wv,solid,alphaBlend,depthAlways,msaa,color);
    // After the mist, overlays continue in a pass without a depth target.
    mistPipeline=pipeline(wv,mist,alphaBlend,noDepth,msaa,color);
    shaftPipeline=pipeline(wv,mist,addBlend,noDepth,msaa,color);
    overlayWeaponPipeline=pipeline(wv,world,alphaBlend,noDepth,msaa,color);
    overlayHudPipeline=pipeline(wv,hud,alphaBlend,noDepth,msaa,color);
    overlaySolidPipeline=pipeline(wv,solid,alphaBlend,noDepth,msaa,color);
    reflectOpaquePipeline=pipeline(rv,opaque,opaqueBlend,depthWrite,one,color);
    reflectSkyPipeline=pipeline(kv,sky,opaqueBlend,depthTest,one,color);
    reflectSkySurfacePipeline=pipeline(rv,skySurface,opaqueBlend,depthWrite,one,color);
    reflectWorldPipeline=pipeline(rv,world,alphaBlend,depthWrite,one,color);
    emissionPipeline=pipeline(wv,emission,opaqueBlend,depthWrite,one,glow);
    blurPipeline=pipeline(sv,blur,opaqueBlend,noDepth,one,glow);
    presentPipeline=pipeline(sv,present,opaqueBlend,noDepth,one,swapchainFormat);
    depthPipeline=pipeline(wv,depth,opaqueBlend,depthOnly,one,color);
    for(SDL_GPUShader *s:{world,opaque,sky,skySurface,shadow,decal,particle,heat,solid,hud,emission,blur,mist,present,depth})
        if(s)SDL_ReleaseGPUShader(device,s);
    for(SDL_GPUShader **s:{&worldVertexShader,&reflectVertexShader,&screenVertexShader,&skyVertexShader})
        if(*s) {SDL_ReleaseGPUShader(device,*s);*s=nullptr;}
    for(auto *p:{opaquePipeline,skyPipeline,skySurfacePipeline,reflectSkySurfacePipeline,worldPipeline,shadowPipeline,decalPipeline,particlePipeline,heatPipeline,weaponPipeline,hudPipeline,
                 solidPipeline,mistPipeline,shaftPipeline,overlayWeaponPipeline,overlayHudPipeline,overlaySolidPipeline,reflectOpaquePipeline,
                 reflectSkyPipeline,reflectWorldPipeline,emissionPipeline,blurPipeline,presentPipeline,depthPipeline})
        if(!p)return false;
    return true;
}
SDL_GPUSampler *sampler(SDL_GPUFilter filter,SDL_GPUSamplerAddressMode address) {
    SDL_GPUSamplerCreateInfo info={};
    info.min_filter=info.mag_filter=filter;info.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.address_mode_u=info.address_mode_v=info.address_mode_w=address;
    return SDL_CreateGPUSampler(device,&info);
}
void releaseTargets() {
    for(SDL_GPUTexture **t:{&multisampleTexture,&colorTexture,&depthTexture,&sceneDepth,&emissionTexture,&emissionDepth,&bloomScratch,&bloomTexture})
        release(*t);
}
void allocateTargets(int w,int h) {
    releaseTargets();
    const auto color=SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,glow=SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    const auto target=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER;
    bool msaa=sampleCount!=SDL_GPU_SAMPLECOUNT_1;
    if(msaa)multisampleTexture=createTexture(color,w,h,SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,sampleCount);
    colorTexture=createTexture(color,w,h,target);
    // Single-sample depth is read by the mist directly; MSAA depth uses a prepass copy.
    depthTexture=createTexture(depthFormat,w,h,SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET|(msaa?0:SDL_GPU_TEXTUREUSAGE_SAMPLER),sampleCount);
    if(msaa)sceneDepth=createTexture(depthFormat,w,h,SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if(!colorTexture||!depthTexture||(msaa&&(!multisampleTexture||!sceneDepth)))
        I_Error((char*)"Could not allocate GPU render targets at %dx%d: %s",w,h,SDL_GetError());
    int bw=std::max(1,w/2),bh=std::max(1,h/2);
    emissionTexture=createTexture(glow,bw,bh,target);
    bloomScratch=createTexture(glow,bw,bh,target);
    bloomTexture=createTexture(glow,bw,bh,target);
    emissionDepth=createTexture(depthFormat,bw,bh,SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    if(!emissionTexture||!bloomScratch||!bloomTexture||!emissionDepth) I_Error((char*)"Could not allocate bloom targets");
}
// One copy pass per frame uploads every vertex batch and the light/sector data.
void reserve(Arena &arena,Uint32 bytes,SDL_GPUBufferUsageFlags usage) {
    if(arena.buffer&&arena.capacity>=bytes)return;
    if(arena.buffer)SDL_ReleaseGPUBuffer(device,arena.buffer);
    arena.capacity=std::max<Uint32>(65536,bytes+bytes/2);
    SDL_GPUBufferCreateInfo info={usage,arena.capacity,0};
    arena.buffer=SDL_CreateGPUBuffer(device,&info);
    if(!arena.buffer)I_Error((char*)"Could not allocate GPU buffer: %s",SDL_GetError());
}
void uploadFrame(SDL_GPUCommandBuffer *command,const std::vector<const std::vector<Vertex>*> &transient,
                 const std::vector<std::array<float,4>> &sectors,const unsigned *palette,const std::vector<unsigned> &hud) {
    vertexOffsets.clear();Uint32 bytes=0;
    auto place=[&](const std::vector<Vertex> &vertices) {
        if(vertices.empty())return;
        vertexOffsets[&vertices]=bytes;
        bytes=(bytes+(Uint32)(vertices.size()*sizeof(Vertex))+255)&~Uint32(255);
    };
    for(const auto *groups:{&batches,&shadowBatches,&decalBatches})for(const auto &batch:*groups)place(batch.second);
    place(mistVertices);place(shaftVertices);place(particleVertices);place(heatVertices);place(skyVertices);
    for(const auto *vertices:transient)place(*vertices);
    LightBlocker emptyBlocker={};
    Uint32 blockerBytes=(Uint32)(std::max(size_t(1),lightBlockers.size())*sizeof(LightBlocker));
    Uint32 sectorBytes=(Uint32)(sectors.size()*sizeof(sectors[0]));
    Uint32 vertexBytes=std::max<Uint32>(bytes,256);
    reserve(vertexArena,vertexBytes,SDL_GPU_BUFFERUSAGE_VERTEX);
    reserve(blockerArena,blockerBytes,SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
    reserve(sectorArena,sectorBytes,SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
    Uint32 imageOffset=vertexBytes+blockerBytes+sectorBytes,hudBytes=(Uint32)(hud.size()*4);
    Uint32 total=imageOffset+1024+hudBytes;
    if(!frameTransfer||frameTransferCapacity<total) {
        if(frameTransfer)SDL_ReleaseGPUTransferBuffer(device,frameTransfer);
        frameTransferCapacity=std::max<Uint32>(262144,total+total/2);
        SDL_GPUTransferBufferCreateInfo info={SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,frameTransferCapacity,0};
        frameTransfer=SDL_CreateGPUTransferBuffer(device,&info);
        if(!frameTransfer)I_Error((char*)"Could not allocate GPU frame upload: %s",SDL_GetError());
    }
    byte *mapped=(byte*)SDL_MapGPUTransferBuffer(device,frameTransfer,true);
    for(const auto &entry:vertexOffsets)
        memcpy(mapped+entry.second,entry.first->data(),entry.first->size()*sizeof(Vertex));
    memcpy(mapped+vertexBytes,lightBlockers.empty()?&emptyBlocker:(const void*)lightBlockers.data(),blockerBytes);
    memcpy(mapped+vertexBytes+blockerBytes,sectors.data(),sectorBytes);
    memcpy(mapped+imageOffset,palette,1024);
    memcpy(mapped+imageOffset+1024,hud.data(),hudBytes);
    SDL_UnmapGPUTransferBuffer(device,frameTransfer);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(command);
    auto send=[&](Uint32 offset,SDL_GPUBuffer *buffer,Uint32 size) {
        SDL_GPUTransferBufferLocation source={frameTransfer,offset};
        SDL_GPUBufferRegion target={buffer,0,size};
        SDL_UploadToGPUBuffer(copy,&source,&target,true);
    };
    if(bytes)send(0,vertexArena.buffer,bytes);
    send(vertexBytes,blockerArena.buffer,blockerBytes);
    send(vertexBytes+blockerBytes,sectorArena.buffer,sectorBytes);
    auto image=[&](Uint32 offset,SDL_GPUTexture *texture,Uint32 w,Uint32 h) {
        SDL_GPUTextureTransferInfo source={frameTransfer,offset,w,h};
        SDL_GPUTextureRegion target={};target.texture=texture;target.w=w;target.h=h;target.d=1;
        SDL_UploadToGPUTexture(copy,&source,&target,true);
    };
    image(imageOffset,paletteTexture,256,1);
    image(imageOffset+1024,hudTexture,SCREENWIDTH,SCREENHEIGHT);
    SDL_EndGPUCopyPass(copy);
}
void drawVertices(SDL_GPURenderPass *pass,const std::vector<Vertex> &vertices) {
    if(vertices.empty())return;
    auto uploaded=vertexOffsets.find(&vertices);
    if(uploaded==vertexOffsets.end())return;
    SDL_GPUBufferBinding binding={vertexArena.buffer,uploaded->second};
    SDL_BindGPUVertexBuffers(pass,0,&binding,1);
    SDL_DrawGPUPrimitives(pass,(Uint32)vertices.size(),1,0,0);
}
void setView(SDL_GPURenderPass *pass,float x,float y,float w,float h,const SDL_Rect *scissor=nullptr) {
    SDL_GPUViewport viewport={x,y,w,h,0,1};SDL_SetGPUViewport(pass,&viewport);
    SDL_Rect clip=scissor?*scissor:SDL_Rect{(int)x,(int)y,(int)w,(int)h};
    SDL_SetGPUScissor(pass,&clip);
}
// World fragment resources: image, palette, next frame, seams, contact,
// reflection, palette lookup, wall bake, flat light, caustic map and pattern,
// wall and flat light directions, the surface's screen glass frame, detail
// texture layers; blockers and sector data;
// four uniform blocks.
// Bindings reset with every render pass, so each pass binds them again.
void bindWorld(SDL_GPURenderPass *pass,SDL_GPUTexture *reflection) {
    SDL_GPUTextureSamplerBinding samplers[]={
        {paletteTexture,nearestClamp},{seamTexture?gpu(seamTexture):dummySeam,nearestClamp},
        {contactTexture?gpu(contactTexture):dummyContact,linearClamp},{reflection?reflection:paletteTexture,linearClamp},
        {paletteLUT,nearestClamp},{wallBakeTexture?gpu(wallBakeTexture):dummyBake,nearestClamp},
        {flatLightTexture?gpu(flatLightTexture):dummyBake,nearestClamp},
        {causticTexture?gpu(causticTexture):dummyContact,linearClamp},{causticPattern?gpu(causticPattern):dummyContact,nearestClamp},
        {wallDirectionTexture?gpu(wallDirectionTexture):dummyBake,nearestClamp},{flatDirectionTexture?gpu(flatDirectionTexture):dummyBake,nearestClamp},
        {dummyBake,nearestClamp},{detailTexture,detailSampler}};
    SDL_BindGPUFragmentSamplers(pass,1,&samplers[0],1);
    SDL_BindGPUFragmentSamplers(pass,3,&samplers[1],12);
    SDL_GPUBuffer *storage[]={blockerArena.buffer,sectorArena.buffer};
    SDL_BindGPUFragmentStorageBuffers(pass,0,storage,2);
}
// Binds a batch texture with its next animation frame and crossfade amount;
// world passes also take its screen glass frame.
void bindSurface(SDL_GPUCommandBuffer *command,SDL_GPURenderPass *pass,int key,Uint32 blendSlot,bool world=true) {
    SurfaceBinding binding=surfaceBinding(key);
    SDL_GPUTextureSamplerBinding image={gpu(binding.image->texture),linearRepeat},next={gpu(binding.next->texture),linearRepeat};
    SDL_BindGPUFragmentSamplers(pass,0,&image,1);
    SDL_BindGPUFragmentSamplers(pass,2,&next,1);
    if(world) {
        SDL_GPUTextureSamplerBinding frame={binding.image->glassFrame?gpu(binding.image->glassFrame):dummyBake,nearestClamp};
        SDL_BindGPUFragmentSamplers(pass,13,&frame,1);
    }
    float blend[4]={binding.blend,binding.detail,binding.detailSwap,0};
    SDL_PushGPUFragmentUniformData(command,blendSlot,blend,sizeof(blend));
}
void bindImage(SDL_GPURenderPass *pass,const GpuTextureRef &texture,SDL_GPUSampler *filter) {
    SDL_GPUTextureSamplerBinding binding={gpu(texture),filter};
    SDL_BindGPUFragmentSamplers(pass,0,&binding,1);
}
void pushWorldUniforms(SDL_GPUCommandBuffer *command,const Uniforms &view) {
    float noBlend[4]={};
    SDL_PushGPUVertexUniformData(command,0,&view,sizeof(view));
    SDL_PushGPUFragmentUniformData(command,0,&view,sizeof(view));
    SDL_PushGPUFragmentUniformData(command,1,&flashes,sizeof(flashes));
    SDL_PushGPUFragmentUniformData(command,2,&fogLights,sizeof(fogLights));
    SDL_PushGPUFragmentUniformData(command,3,noBlend,sizeof(noBlend));
}
bool opaqueBatch(const std::pair<const int,std::vector<Vertex>> &batch) {
    return !batch.second.empty()&&!(batch.second.front().mode&2)&&images.at(batch.first).opaque;
}
// The world drawn opaque-first, then sky surfaces into depth and the sky into
// uncovered pixels, then cutouts and sprites. Shared by the main view and the
// liquid reflection.
void drawWorld(SDL_GPUCommandBuffer *command,SDL_GPURenderPass *pass,SDL_GPUGraphicsPipeline *opaque,SDL_GPUGraphicsPipeline *skySurface,
               SDL_GPUGraphicsPipeline *sky,SDL_GPUGraphicsPipeline *world,SDL_GPUTexture *reflection) {
    SDL_BindGPUGraphicsPipeline(pass,opaque);bindWorld(pass,reflection);
    for(const auto &batch:batches) if(opaqueBatch(batch)) {bindSurface(command,pass,batch.first,3);drawVertices(pass,batch.second);}
    SDL_GPUTextureSamplerBinding skyImage[]={{gpu(wallImage(skytexture).texture),linearRepeat},{paletteTexture,nearestClamp}};
    if(!skyVertices.empty()) {
        SDL_BindGPUGraphicsPipeline(pass,skySurface);SDL_BindGPUFragmentSamplers(pass,0,skyImage,2);
        drawVertices(pass,skyVertices);
    }
    SDL_BindGPUGraphicsPipeline(pass,sky);
    SDL_BindGPUFragmentSamplers(pass,0,skyImage,2);
    SDL_DrawGPUPrimitives(pass,3,1,0,0);
    SDL_BindGPUGraphicsPipeline(pass,world);bindWorld(pass,reflection);
    for(int sprites=0;sprites<2;++sprites)for(const auto &batch:batches) {
        if(batch.second.empty())continue;
        bool sprite=(batch.second.front().mode&2)!=0;
        if(sprite!=bool(sprites)||(!sprite&&images.at(batch.first).opaque))continue;
        bindSurface(command,pass,batch.first,3);drawVertices(pass,batch.second);
    }
    float noBlend[4]={};SDL_PushGPUFragmentUniformData(command,3,noBlend,sizeof(noBlend));
}
void drawOverlays(SDL_GPUCommandBuffer *command,SDL_GPURenderPass *pass,const FrameView &view,
                  const std::vector<SpriteDraw> &weapons,const std::vector<Vertex> &hud,const std::vector<Vertex> &cross,bool depth) {
    SDL_Rect full={0,0,width,height};
    setView(pass,view.uiX,view.uiY,view.uiWidth,view.uiHeight,&full);
    if(worldPending&&!weapons.empty()) {
        SDL_BindGPUGraphicsPipeline(pass,depth?weaponPipeline:overlayWeaponPipeline);bindWorld(pass,nullptr);
        float noBlend[4]={};SDL_PushGPUFragmentUniformData(command,3,noBlend,sizeof(noBlend));
        for(const auto &draw:weapons) {
            SDL_GPUTextureSamplerBinding image[]={{gpu(draw.image->texture),linearRepeat},{paletteTexture,nearestClamp},{gpu(draw.image->texture),linearRepeat}};
            SDL_BindGPUFragmentSamplers(pass,0,image,3);drawVertices(pass,draw.vertices);
        }
    }
    SDL_BindGPUGraphicsPipeline(pass,depth?hudPipeline:overlayHudPipeline);
    SDL_GPUTextureSamplerBinding hudImage={hudTexture,nearestClamp};
    SDL_BindGPUFragmentSamplers(pass,0,&hudImage,1);
    drawVertices(pass,hud);
    if(!cross.empty()) {
        setView(pass,view.worldX,view.worldY,view.worldW,view.worldH,&full);
        SDL_BindGPUGraphicsPipeline(pass,depth?solidPipeline:overlaySolidPipeline);drawVertices(pass,cross);
    }
}
void present(SDL_GPUCommandBuffer *command,SDL_GPUTexture *target,const FrameView &view) {
    SDL_GPUColorTargetInfo info={};
    info.texture=target;info.load_op=SDL_GPU_LOADOP_DONT_CARE;info.store_op=SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass=SDL_BeginGPURenderPass(command,&info,1,nullptr);
    SDL_BindGPUGraphicsPipeline(pass,presentPipeline);
    float hudRect[4]={view.uiX/width,view.uiY/height,view.uiWidth/width,view.uiHeight/height};
    SDL_PushGPUFragmentUniformData(command,0,hudRect,sizeof(hudRect));
    SDL_PushGPUFragmentUniformData(command,1,&view.camera,sizeof(view.camera));
    SDL_GPUTextureSamplerBinding samplers[]={{colorTexture,linearClamp},{bloomTexture,linearClamp},{hudTexture,linearClamp},{paletteLUT,nearestClamp}};
    SDL_BindGPUFragmentSamplers(pass,0,samplers,4);
    SDL_DrawGPUPrimitives(pass,3,1,0,0);
    SDL_EndGPURenderPass(pass);
}
void capture(SDL_GPUCommandBuffer *command,const FrameView &view,int w,int h) {
    SDL_GPUTexture *image=createTexture(swapchainFormat,w,h,SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    SDL_GPUTransferBufferCreateInfo info={SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,(Uint32)(w*h*4),0};
    SDL_GPUTransferBuffer *download=image?SDL_CreateGPUTransferBuffer(device,&info):nullptr;
    if(!download) {
        release(image);SDL_SubmitGPUCommandBuffer(command);
        saveScreenshot(nullptr,0,0,0);return;
    }
    present(command,image,view);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureRegion source={};source.texture=image;source.w=(Uint32)w;source.h=(Uint32)h;source.d=1;
    SDL_GPUTextureTransferInfo target={download,0,(Uint32)w,(Uint32)h};
    SDL_DownloadFromGPUTexture(copy,&source,&target);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if(fence) {SDL_WaitForGPUFences(device,true,&fence,1);SDL_ReleaseGPUFence(device,fence);}
    std::vector<unsigned> pixels((size_t)w*h);
    memcpy(pixels.data(),SDL_MapGPUTransferBuffer(device,download,false),pixels.size()*4);
    SDL_UnmapGPUTransferBuffer(device,download);
    SDL_ReleaseGPUTransferBuffer(device,download);release(image);
    if(swapchainFormat==SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)
        for(unsigned &p:pixels)p=(p&0xff00ff00u)|((p&0xffu)<<16)|((p>>16)&0xffu);
    saveScreenshot(fence?pixels.data():nullptr,w,h,w*4);
}
} // namespace

GpuTexture::~GpuTexture() {if(texture&&device)SDL_ReleaseGPUTexture(device,texture);}
GpuTextureRef gpuCreateTexture(GpuFormat format,int w,int h,const void *pixels,int bytesPerRow,int levels) {
    SDL_GPUTextureFormat textureFormat=format==GpuFormat::R8?SDL_GPU_TEXTUREFORMAT_R8_UNORM:format==GpuFormat::RG8?SDL_GPU_TEXTUREFORMAT_R8G8_UNORM:
        format==GpuFormat::RGBA8?SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM;
    int pixelBytes=format==GpuFormat::R8?1:format==GpuFormat::RG8?2:format==GpuFormat::RGBA8?4:8;
    if(levels<=1) {
        SDL_GPUTexture *texture=createTexture(textureFormat,w,h,SDL_GPU_TEXTUREUSAGE_SAMPLER);
        if(!texture)return nullptr;
        upload(texture,w,h,1,pixels,(Uint32)(bytesPerRow*h),(Uint32)(bytesPerRow/pixelBytes));
        return GpuTextureRef(new GpuTexture{texture});
    }
    SDL_GPUTextureCreateInfo info={};
    info.type=SDL_GPU_TEXTURETYPE_2D;info.format=textureFormat;info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width=(Uint32)w;info.height=(Uint32)h;info.layer_count_or_depth=1;info.num_levels=(Uint32)levels;info.sample_count=SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *texture=SDL_CreateGPUTexture(device,&info);
    if(!texture)return nullptr;
    Uint32 bytes=0;
    for(int level=0;level<levels;++level)bytes+=(Uint32)(std::max(1,w>>level)*std::max(1,h>>level)*pixelBytes);
    SDL_GPUTransferBufferCreateInfo transferInfo={SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,bytes,0};
    SDL_GPUTransferBuffer *transfer=SDL_CreateGPUTransferBuffer(device,&transferInfo);
    if(!transfer) {SDL_ReleaseGPUTexture(device,texture);return nullptr;}
    memcpy(SDL_MapGPUTransferBuffer(device,transfer,false),pixels,bytes);
    SDL_UnmapGPUTransferBuffer(device,transfer);
    if(!uploadCommand)uploadCommand=SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(uploadCommand);
    Uint32 offset=0;
    for(int level=0;level<levels;++level) {
        Uint32 lw=(Uint32)std::max(1,w>>level),lh=(Uint32)std::max(1,h>>level);
        SDL_GPUTextureTransferInfo source={transfer,offset,lw,lh};
        SDL_GPUTextureRegion target={};
        target.texture=texture;target.mip_level=(Uint32)level;target.w=lw;target.h=lh;target.d=1;
        SDL_UploadToGPUTexture(copy,&source,&target,false);
        offset+=lw*lh*(Uint32)pixelBytes;
    }
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device,transfer);
    return GpuTextureRef(new GpuTexture{texture});
}
bool platformReadMask(const char *path,int w,int h,std::vector<float> &values) {
    SDL_Surface *loaded=SDL_LoadPNG(path);
    SDL_Surface *surface=loaded?SDL_ConvertSurface(loaded,SDL_PIXELFORMAT_RGBA32):nullptr;
    SDL_DestroySurface(loaded);
    bool valid=surface&&surface->w==w&&surface->h==h;
    if(valid) {
        values.resize((size_t)w*h);
        for(int y=0;y<h;++y) {
            const byte *row=(const byte*)surface->pixels+(size_t)y*surface->pitch;
            for(int x=0;x<w;++x) {
                const byte *p=row+x*4;
                values[(size_t)y*w+x]=(p[0]+p[1]+p[2])/(3.0f*255.0f)*(p[3]/255.0f);
            }
        }
    }
    SDL_DestroySurface(surface);
    return valid;
}

int I_Render3DInit(SDL_Window *window) {
    bool debug=M_CheckParm((char*)"-gpudebug")!=0;
    // SDL picks Metal on macOS and Vulkan elsewhere; SDL_GPU_DRIVER overrides it.
    device=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV|SDL_GPU_SHADERFORMAT_MSL,debug,nullptr);
    if(!device) {fprintf(stderr,"GPU renderer unavailable: %s\n",SDL_GetError());return 0;}
    shaderFormat=(SDL_GetGPUShaderFormats(device)&SDL_GPU_SHADERFORMAT_SPIRV)?SDL_GPU_SHADERFORMAT_SPIRV:SDL_GPU_SHADERFORMAT_MSL;
    if(!SDL_ClaimWindowForGPUDevice(device,window)) {
        fprintf(stderr,"GPU renderer: %s\n",SDL_GetError());
        SDL_DestroyGPUDevice(device);device=nullptr;return 0;
    }
    swapchainFormat=SDL_GetGPUSwapchainTextureFormat(device,window);
    const auto sampled=SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER;
    depthFormat=SDL_GPUTextureSupportsFormat(device,SDL_GPU_TEXTUREFORMAT_D32_FLOAT,SDL_GPU_TEXTURETYPE_2D,sampled)?
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT:SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    sampleCount=SDL_GPUTextureSupportsSampleCount(device,SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,SDL_GPU_SAMPLECOUNT_4)&&
                SDL_GPUTextureSupportsSampleCount(device,depthFormat,SDL_GPU_SAMPLECOUNT_4)?SDL_GPU_SAMPLECOUNT_4:SDL_GPU_SAMPLECOUNT_1;
    // Exercise the single-sample depth path on MSAA-capable devices.
    int msaa=M_CheckParm((char*)"-gpumsaa");
    if(msaa&&msaa+1<myargc&&atoi(myargv[msaa+1])==1)sampleCount=SDL_GPU_SAMPLECOUNT_1;
    linearRepeat=sampler(SDL_GPU_FILTER_LINEAR,SDL_GPU_SAMPLERADDRESSMODE_REPEAT);
    linearClamp=sampler(SDL_GPU_FILTER_LINEAR,SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE);
    nearestClamp=sampler(SDL_GPU_FILTER_NEAREST,SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE);
    // Trilinear and anisotropic, so the grain fades to grey instead of shimmering.
    SDL_GPUSamplerCreateInfo detailInfo={};
    detailInfo.min_filter=detailInfo.mag_filter=SDL_GPU_FILTER_LINEAR;detailInfo.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    detailInfo.address_mode_u=detailInfo.address_mode_v=detailInfo.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    detailInfo.enable_anisotropy=true;detailInfo.max_anisotropy=8;detailInfo.max_lod=DOOM_DETAIL_LEVELS;
    detailSampler=SDL_CreateGPUSampler(device,&detailInfo);
    if(!linearRepeat||!linearClamp||!nearestClamp||!detailSampler||!createPipelines()) {I_Render3DShutdown();return 0;}
    hudTexture=createTexture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,SCREENWIDTH,SCREENHEIGHT,SDL_GPU_TEXTUREUSAGE_SAMPLER);
    paletteTexture=createTexture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,256,1,SDL_GPU_TEXTUREUSAGE_SAMPLER);
    dummySeam=createTexture(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM,1,1,SDL_GPU_TEXTUREUSAGE_SAMPLER);
    dummyContact=createTexture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,1,1,SDL_GPU_TEXTUREUSAGE_SAMPLER);
    dummyBake=createTexture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,1,1,SDL_GPU_TEXTUREUSAGE_SAMPLER);
    solidTexel=createTexture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,1,1,SDL_GPU_TEXTUREUSAGE_SAMPLER);
    detailTexture=createDetailTexture();
    if(!hudTexture||!paletteTexture||!dummySeam||!dummyContact||!dummyBake||!solidTexel||!detailTexture) {I_Render3DShutdown();return 0;}
    std::array<byte,8> zero={};const byte covered[4]={0,255,0,255}; // Index 0, full coverage, no emission.
    upload(dummySeam,1,1,1,zero.data(),8,1);upload(dummyContact,1,1,1,zero.data(),4,1);upload(dummyBake,1,1,1,zero.data(),4,1);
    upload(solidTexel,1,1,1,covered,4,1);
    buildCloudTexture();
    flushUploads();
    sceneInit(window);
    applySettings();
    fprintf(stderr,"GPU renderer: %s; %dx MSAA, native pixels, widescreen and depth-tested 3D.\n",
        SDL_GetGPUDeviceDriver(device),(int)(sampleCount==SDL_GPU_SAMPLECOUNT_4?4:1));
    return 1;
}
void I_Render3DShutdown(void) {
    if(device)SDL_WaitForGPUIdle(device);
    sceneShutdown();
    flushUploads();
    releaseTargets();release(reflectionTexture);release(reflectionDepth);reflectionWidth=reflectionHeight=0;width=height=0;
    for(SDL_GPUTexture **t:{&hudTexture,&paletteTexture,&paletteLUT,&dummySeam,&dummyContact,&dummyBake,&solidTexel,&detailTexture})release(*t);
    for(SDL_GPUGraphicsPipeline **p:{&opaquePipeline,&skyPipeline,&skySurfacePipeline,&reflectSkySurfacePipeline,&worldPipeline,&shadowPipeline,&decalPipeline,&particlePipeline,&heatPipeline,
            &weaponPipeline,&hudPipeline,&solidPipeline,&mistPipeline,&shaftPipeline,&overlayWeaponPipeline,&overlayHudPipeline,&overlaySolidPipeline,
            &reflectOpaquePipeline,&reflectSkyPipeline,&reflectWorldPipeline,&emissionPipeline,&blurPipeline,&presentPipeline,&depthPipeline})
        release(*p);
    for(SDL_GPUSampler **s:{&linearRepeat,&linearClamp,&nearestClamp,&detailSampler})release(*s,SDL_ReleaseGPUSampler);
    for(Arena *a:{&vertexArena,&blockerArena,&sectorArena}) {release(a->buffer,SDL_ReleaseGPUBuffer);a->capacity=0;}
    release(frameTransfer,SDL_ReleaseGPUTransferBuffer);frameTransferCapacity=0;vertexOffsets.clear();
    if(device) {
        if(gameWindow)SDL_ReleaseWindowFromGPUDevice(device,gameWindow);
        SDL_DestroyGPUDevice(device);
    }
    device=nullptr;
}
void I_Render3DPresent(const unsigned *pixels,const unsigned *palette) {
    SDL_GPUCommandBuffer *command=SDL_AcquireGPUCommandBuffer(device);
    if(!command) {fprintf(stderr,"GPU command buffer: %s\n",SDL_GetError());return;}
    SDL_GPUTexture *swapchain=nullptr;Uint32 drawableWidth=0,drawableHeight=0;
    if(!SDL_WaitAndAcquireGPUSwapchainTexture(command,gameWindow,&swapchain,&drawableWidth,&drawableHeight)||!swapchain) {
        flushUploads();SDL_SubmitGPUCommandBuffer(command);return;
    }
    int w=std::max(1,(int)drawableWidth*settings.scale/100),h=std::max(1,(int)drawableHeight*settings.scale/100);
    if(w!=width||h!=height||!colorTexture) {
        width=w;height=h;allocateTargets(w,h);
        fprintf(stderr,"GPU resolution: %dx%d (display %ux%u, %d%%).\n",w,h,drawableWidth,drawableHeight,settings.scale);
    }
    if(!paletteLUT||((settings.palette||(settings.reflections&&settings.retroReflections))&&worldPending))updatePaletteLUT(palette);
    std::vector<unsigned> overlay=hudPixels(pixels);
    FrameView view=prepareFrame(w,h);Uniforms &camera=view.camera;
    bool drawMist=!mistVertices.empty()||!shaftVertices.empty();
    bool msaa=sampleCount!=SDL_GPU_SAMPLECOUNT_1;
    // Transient quads join the frame upload; textures created while drawing
    // go through the upload command buffer, which is submitted first.
    std::vector<SpriteDraw> weapons;if(worldPending)weapons=weaponDraws(camera);
    std::vector<Vertex> hud;screenQuad(hud,-1,1,2,2,0,0,1,1);
    std::vector<Vertex> cross=crosshairVertices(view.worldW,view.worldH);
    std::vector<const std::vector<Vertex>*> transient={&hud,&cross};
    for(const auto &draw:weapons)transient.push_back(&draw.vertices);
    uploadFrame(command,transient,sectorInfo(),palette,overlay);
    // Liquid reflections: the world drawn again at reduced resolution from a
    // camera mirrored at the liquid height, clipped to above the surface.
    camera.water[2]=0;
    if(worldPending&&reflectionActive) {
        int divisor=settings.retroReflections?4:2;
        int rw=std::max(1,(int)(view.worldW/divisor)),rh=std::max(1,(int)(view.worldH/divisor));
        if(!reflectionTexture||rw!=reflectionWidth||rh!=reflectionHeight) {
            release(reflectionTexture);release(reflectionDepth);
            reflectionTexture=createTexture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,rw,rh,SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER);
            reflectionDepth=createTexture(depthFormat,rw,rh,SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
            if(!reflectionTexture||!reflectionDepth)I_Error((char*)"Could not allocate reflection targets");
            reflectionWidth=rw;reflectionHeight=rh;
        }
        Uniforms mirror=camera;
        mirror.eye[2]=2*reflectionPlane-camera.eye[2];mirror.forward[2]=-camera.forward[2];mirror.up[2]=-camera.up[2];
        SDL_GPUColorTargetInfo color={};
        color.texture=reflectionTexture;color.load_op=SDL_GPU_LOADOP_CLEAR;color.store_op=SDL_GPU_STOREOP_STORE;
        color.clear_color={0.025f,0.025f,0.03f,1};
        SDL_GPUDepthStencilTargetInfo depth={};
        depth.texture=reflectionDepth;depth.clear_depth=1;depth.load_op=SDL_GPU_LOADOP_CLEAR;depth.store_op=SDL_GPU_STOREOP_DONT_CARE;
        depth.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE;depth.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass *pass=SDL_BeginGPURenderPass(command,&color,1,&depth);
        pushWorldUniforms(command,mirror);
        float plane[4]={reflectionPlane,0,0,0};SDL_PushGPUVertexUniformData(command,1,plane,sizeof(plane));
        drawWorld(command,pass,reflectOpaquePipeline,reflectSkySurfacePipeline,reflectSkyPipeline,reflectWorldPipeline,nullptr);
        SDL_EndGPURenderPass(pass);
        camera.water[0]=view.worldX;camera.water[1]=view.worldY;camera.water[2]=1/view.worldW;camera.water[3]=1/view.worldH;
    }
    // Soft mist needs single-sample scene depth; under MSAA a depth-only prepass supplies it.
    if(drawMist&&msaa) {
        SDL_GPUDepthStencilTargetInfo depth={};
        depth.texture=sceneDepth;depth.clear_depth=1;depth.load_op=SDL_GPU_LOADOP_CLEAR;depth.store_op=SDL_GPU_STOREOP_STORE;
        depth.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE;depth.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass *pass=SDL_BeginGPURenderPass(command,nullptr,0,&depth);
        setView(pass,view.worldX,view.worldY,view.worldW,view.worldH);
        SDL_BindGPUGraphicsPipeline(pass,depthPipeline);
        SDL_PushGPUVertexUniformData(command,0,&camera,sizeof(camera));
        for(const auto &batch:batches) if(!batch.second.empty()) {bindImage(pass,images.at(batch.first).texture,nearestClamp);drawVertices(pass,batch.second);}
        SDL_GPUTextureSamplerBinding solid={solidTexel,nearestClamp};
        SDL_BindGPUFragmentSamplers(pass,0,&solid,1);drawVertices(pass,skyVertices);
        SDL_EndGPURenderPass(pass);
    }
    SDL_GPUColorTargetInfo color={};
    color.texture=msaa?multisampleTexture:colorTexture;color.clear_color={0.025f,0.025f,0.03f,1};
    color.load_op=SDL_GPU_LOADOP_CLEAR;
    color.store_op=msaa&&!drawMist?SDL_GPU_STOREOP_RESOLVE:SDL_GPU_STOREOP_STORE;
    color.resolve_texture=msaa&&!drawMist?colorTexture:nullptr;
    SDL_GPUDepthStencilTargetInfo depth={};
    depth.texture=depthTexture;depth.clear_depth=1;depth.load_op=SDL_GPU_LOADOP_CLEAR;
    depth.store_op=drawMist&&!msaa?SDL_GPU_STOREOP_STORE:SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE;depth.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass *pass=SDL_BeginGPURenderPass(command,&color,1,&depth);
    pushWorldUniforms(command,camera);
    if(worldPending) {
        setView(pass,view.worldX,view.worldY,view.worldW,view.worldH);
        // Only fully covered textures use early depth writes. Cutouts and
        // sprites retain discard-aware shading after the sky background.
        drawWorld(command,pass,opaquePipeline,skySurfacePipeline,skyPipeline,worldPipeline,camera.water[2]>0?reflectionTexture:nullptr);
        // The completed world depth hides shadows behind walls and enemies.
        SDL_BindGPUGraphicsPipeline(pass,shadowPipeline);
        for(const auto &batch:shadowBatches) {bindImage(pass,images.at(-1-batch.first).texture,linearClamp);drawVertices(pass,batch.second);}
        SDL_BindGPUGraphicsPipeline(pass,decalPipeline);
        for(const auto &batch:decalBatches) {bindImage(pass,images.at(-1-batch.first).texture,linearClamp);drawVertices(pass,batch.second);}
        SDL_BindGPUGraphicsPipeline(pass,particlePipeline);drawVertices(pass,particleVertices);
        // Hot air marks the color target's alpha for the shimmer in present.
        if(!heatVertices.empty()) {SDL_BindGPUGraphicsPipeline(pass,heatPipeline);drawVertices(pass,heatVertices);}
    }
    if(drawMist) {
        SDL_EndGPURenderPass(pass);
        color.load_op=SDL_GPU_LOADOP_LOAD;
        color.store_op=msaa?SDL_GPU_STOREOP_RESOLVE:SDL_GPU_STOREOP_STORE;
        color.resolve_texture=msaa?colorTexture:nullptr;
        pass=SDL_BeginGPURenderPass(command,&color,1,nullptr);
        setView(pass,view.worldX,view.worldY,view.worldW,view.worldH);
        SDL_BindGPUGraphicsPipeline(pass,mistPipeline);
        SDL_PushGPUVertexUniformData(command,0,&camera,sizeof(camera));
        SDL_PushGPUFragmentUniformData(command,0,&camera,sizeof(camera));
        SDL_GPUTextureSamplerBinding mist[]={{gpu(cloudTexture),linearRepeat},{msaa?sceneDepth:depthTexture,nearestClamp}};
        SDL_BindGPUFragmentSamplers(pass,0,mist,2);
        drawVertices(pass,mistVertices);
        if(!shaftVertices.empty()) {
            SDL_BindGPUGraphicsPipeline(pass,shaftPipeline);SDL_BindGPUFragmentSamplers(pass,0,mist,2);
            drawVertices(pass,shaftVertices);
        }
        pushWorldUniforms(command,camera);
    }
    drawOverlays(command,pass,view,weapons,hud,cross,!drawMist);
    SDL_EndGPURenderPass(pass);
    if(camera.materials[1]>0) {
        int bw=std::max(1,width/2),bh=std::max(1,height/2);
        float sx=(float)bw/w,sy=(float)bh/h;
        SDL_GPUColorTargetInfo glow={};
        glow.texture=emissionTexture;glow.load_op=SDL_GPU_LOADOP_CLEAR;glow.store_op=SDL_GPU_STOREOP_STORE;glow.clear_color={0,0,0,1};
        SDL_GPUDepthStencilTargetInfo glowDepth={};
        glowDepth.texture=emissionDepth;glowDepth.clear_depth=1;glowDepth.load_op=SDL_GPU_LOADOP_CLEAR;
        glowDepth.store_op=SDL_GPU_STOREOP_DONT_CARE;glowDepth.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE;glowDepth.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass *glowPass=SDL_BeginGPURenderPass(command,&glow,1,&glowDepth);
        SDL_Rect area={0,0,bw,bh};
        setView(glowPass,view.worldX*sx,view.worldY*sy,view.worldW*sx,view.worldH*sy,&area);
        SDL_BindGPUGraphicsPipeline(glowPass,emissionPipeline);
        SDL_PushGPUVertexUniformData(command,0,&camera,sizeof(camera));
        SDL_GPUTextureSamplerBinding palette={paletteTexture,nearestClamp};
        SDL_BindGPUFragmentSamplers(glowPass,1,&palette,1);
        for(const auto &batch:batches) if(!batch.second.empty()) {bindSurface(command,glowPass,batch.first,0,false);drawVertices(glowPass,batch.second);}
        // The sky hides glow behind it: a covered texel with no emission.
        SDL_GPUTextureSamplerBinding solid={solidTexel,nearestClamp};
        SDL_BindGPUFragmentSamplers(glowPass,0,&solid,1);SDL_BindGPUFragmentSamplers(glowPass,2,&solid,1);
        float noBlend[4]={};SDL_PushGPUFragmentUniformData(command,0,noBlend,sizeof(noBlend));
        drawVertices(glowPass,skyVertices);
        SDL_EndGPURenderPass(glowPass);
        for(int axis=0;axis<2;++axis) {
            SDL_GPUColorTargetInfo blur={};
            blur.texture=axis?bloomTexture:bloomScratch;blur.load_op=SDL_GPU_LOADOP_DONT_CARE;blur.store_op=SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass *blurPass=SDL_BeginGPURenderPass(command,&blur,1,nullptr);
            SDL_BindGPUGraphicsPipeline(blurPass,blurPipeline);
            SDL_GPUTextureSamplerBinding source={axis?bloomScratch:emissionTexture,linearClamp};
            SDL_BindGPUFragmentSamplers(blurPass,0,&source,1);
            // Radius scales with resolution, retaining a similar halo at 50–100%.
            float direction[4]={axis?0:0.0035f,axis?0.0056f:0,0,0};
            SDL_PushGPUFragmentUniformData(command,0,direction,sizeof(direction));
            SDL_DrawGPUPrimitives(blurPass,3,1,0,0);
            SDL_EndGPURenderPass(blurPass);
        }
    }
    present(command,swapchain,view);
    flushUploads();
    if(screenshotPending||renderCheckDue()) {screenshotPending=false;capture(command,view,(int)drawableWidth,(int)drawableHeight);}
    else if(!SDL_SubmitGPUCommandBuffer(command)) I_Error((char*)"GPU rendering failed: %s",SDL_GetError());
    char renderer[64];snprintf(renderer,sizeof(renderer),"%s 3D",SDL_GetGPUDeviceDriver(device));
    if(renderer[0])renderer[0]=(char)toupper((unsigned char)renderer[0]);
    updateTitle(w,h,renderer);
}

#ifndef __APPLE__
// A minimal settings prompt until an in-game options menu replaces it; macOS
// uses the native dialog in i_mac_settings.mm.
void I_Render3DSettings(void) {
    bool wasPaused=paused;paused=true;
    SDL_SetWindowRelativeMouseMode(gameWindow,false);
    const SDL_MessageBoxButtonData buttons[]={
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT|SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,0,"Done"},
        {0,1,"3D / Classic"},{0,2,"Resolution"},{0,3,"Effects on/off"},{0,4,"Reset"}};
    char text[512];
    snprintf(text,sizeof(text),"Renderer: %s\nResolution: %d%%\nEffects (emissive, fog, reflections, detail): %s\n\n"
        "All options, including field of view and sprite filtering, are in graphics.cfg next to the game.",
        settings.accelerated?"3D":"Classic",settings.scale,settings.emissive&&settings.fog?"on":"off");
    SDL_MessageBoxData box={SDL_MESSAGEBOX_INFORMATION,gameWindow,"Graphics",text,SDL_arraysize(buttons),buttons,nullptr};
    int choice=0;SDL_ShowMessageBox(&box,&choice);paused=wasPaused;
    if(choice==1)settings.accelerated=!settings.accelerated;
    else if(choice==2)settings.scale=settings.scale==100?75:settings.scale==75?50:100;
    else if(choice==3) {
        int on=!(settings.emissive&&settings.fog);
        settings.emissive=settings.fog=settings.reflections=settings.detail=settings.softLight=on;
        settings.blood=settings.bloodShine=settings.flashlightShadows=settings.softSprites=settings.heatHaze=settings.eyeAdaptation=on;
        settings.splashes=settings.dust=settings.playerShadow=settings.doorLight=settings.texelLight=on;
        settings.skyLight=settings.bakedAO=settings.thingShadows=settings.lightFlow=settings.ceilingCaustics=settings.glossyScreens=settings.sunShafts=settings.sunDisc=on;
    } else if(choice==4)settings=Settings{};
    settingsChanged();
}
#endif
