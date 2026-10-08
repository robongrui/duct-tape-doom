/* Shared CPU flash sampling, also exercised by the data-free checks.
 * Keep flashAt in platform/shaders/lighting.glsl equivalent. */
#ifndef DOOM_FLASH_LIGHTING_H
#define DOOM_FLASH_LIGHTING_H
#include <math.h>
#include <stdint.h>
#include <stddef.h>
/* baked: the part of the light already in the bake maps (0: dynamic only). */
typedef struct { float position[4],strength; uint32_t first,count; float baked; float color[4],direction[4]; } doom_flash_t;
typedef struct { float line[4],opening[4]; } doom_light_blocker_t;
/* Pick the strongest bright, saturated hue cluster, ignoring transparent,
 * dark and white core pixels. This preserves a projectile's colored halo. */
static inline float doom_sprite_glow(const uint8_t *pixels,size_t count,const uint8_t *palette,float out[3]) {
    float weights[12]={0},colors[12][3]={{0}};
    size_t opaque=0;
    for(size_t i=0;i<count;++i) {
        if(!pixels[i*2+1]) continue;
        ++opaque;
        const uint8_t *rgb=palette+pixels[i*2]*3;
        float r=rgb[0]/255.0f,g=rgb[1]/255.0f,b=rgb[2]/255.0f;
        float hi=fmaxf(r,fmaxf(g,b)),lo=fminf(r,fminf(g,b)),chroma=hi-lo;
        if(hi<0.5f||chroma<hi*0.2f) continue;
        float hue=hi==r?(g-b)/chroma:hi==g?2+(b-r)/chroma:4+(r-g)/chroma;
        if(hue<0) hue+=6;
        int bin=(int)(hue*2)%12;
        float weight=hi*hi*hi*hi*chroma*chroma;
        weights[bin]+=weight;
        colors[bin][0]+=r*weight;colors[bin][1]+=g*weight;colors[bin][2]+=b*weight;
    }
    int best=0;float score=0;
    for(int i=0;i<12;++i) {
        float weight=weights[i]+0.5f*(weights[(i+11)%12]+weights[(i+1)%12]);
        if(weight>score) {score=weight;best=i;}
    }
    if(score<=0) {out[0]=out[1]=out[2]=1;return 0;}
    for(int c=0;c<3;++c)
        out[c]=colors[best][c]+0.5f*(colors[(best+11)%12][c]+colors[(best+1)%12][c]);
    float peak=fmaxf(out[0],fmaxf(out[1],out[2]));
    for(int c=0;c<3;++c) out[c]=fmaxf(0.06f,out[c]/peak);
    return opaque?score/(float)opaque:0;
}
/* Conservative sphere/AABB rejection: retain tangent bounds and never drop
 * illumination inside the light radius, including very large triangles. */
static inline int doom_flash_reaches_bounds(const doom_flash_t *light,const float low[3],const float high[3]) {
    float distance2=0;
    for(int axis=0;axis<3;++axis) {
        float p=light->position[axis];
        float d=p-fminf(high[axis],fmaxf(low[axis],p));
        distance2+=d*d;
    }
    return distance2<=light->position[3]*light->position[3];
}
static inline float doom_flash_cross(float ax,float ay,float bx,float by) {return ax*by-ay*bx;}
static inline float doom_flash_fade(int elapsed,float fraction) {
    float age=fmaxf(0.0f,(elapsed-2+fraction)/5.0f);
    return fmaxf(0.0f,1.0f-age);
}
static inline float doom_flash_at(float x,float y,float z,const doom_flash_t *f,
                                const doom_light_blocker_t *blockers) {
    float dx=x-f->position[0],dy=y-f->position[1],dz=z-f->position[2];
    float falloff=fmaxf(0.0f,1.0f-sqrtf(dx*dx+dy*dy+dz*dz)/f->position[3]);
    if(falloff<=0) return 0;
    if(f->direction[3]>0) {
        float distance=sqrtf(dx*dx+dy*dy+dz*dz);
        float cosine=distance>0.001f?(dx*f->direction[0]+dy*f->direction[1]+dz*f->direction[2])/distance:1;
        float cone=fminf(1,fmaxf(0,(cosine-f->direction[3])/(0.985f-f->direction[3])));
        falloff*=cone*cone*(3-2*cone);
    }
    for(uint32_t i=f->first;i<f->first+f->count;++i) {
        const doom_light_blocker_t *b=&blockers[i];
        float ex=b->line[2]-b->line[0],ey=b->line[3]-b->line[1];
        float ox=b->line[0]-f->position[0],oy=b->line[1]-f->position[1];
        float det=doom_flash_cross(dx,dy,ex,ey);
        if(fabsf(det)<0.0001f) continue;
        float t=doom_flash_cross(ox,oy,ex,ey)/det,u=doom_flash_cross(ox,oy,dx,dy)/det;
        if(t>0.001f&&t<0.999f&&u>=0&&u<=1) {
            float hit=f->position[2]+dz*t;
            if(hit<=b->opening[0]+0.02f||hit>=b->opening[1]-0.02f) return 0;
        }
    }
    return f->strength*falloff*falloff*(3.0f-2.0f*falloff);
}
/* Exact blocker culling; dropping a line never changes doom_flash_at. Lines
 * beyond the radius cannot be crossed, nor can openings spanning every reachable
 * height. A light never sits in the void, so a ray meeting a one-sided wall
 * from behind already left the map through a wall that faces the light. */
static inline int doom_flash_blocker_relevant(const doom_flash_t *f,const doom_light_blocker_t *b,int one_sided) {
    float x=f->position[0],y=f->position[1],z=f->position[2],r=f->position[3];
    float ex=b->line[2]-b->line[0],ey=b->line[3]-b->line[1];
    float px=x-b->line[0],py=y-b->line[1],length2=ex*ex+ey*ey;
    float t=length2>0?fminf(1.0f,fmaxf(0.0f,(px*ex+py*ey)/length2)):0;
    float dx=px-ex*t,dy=py-ey*t;
    if(dx*dx+dy*dy>r*r) return 0;
    if(one_sided) return doom_flash_cross(ex,ey,px,py)<=0; /* Light on the front (right) side. */
    return !(b->opening[0]<=z-r-0.02f&&b->opening[1]>=z+r+0.02f);
}
/* Exact occlusion culling of one light's relevant blockers, in place; returns
 * the new count. Around the light, angular bins hold how far walls that stop
 * every ray (one-sided, closed, or open only beyond the light's reach) cover
 * the whole bin. A line nearer to none of its bins than that is hidden: any
 * ray through it already crossed such a wall, so dropping it never changes
 * doom_flash_at. A spotlight also drops lines outside its cone's footprint.
 * Dense maps keep hundreds of lines in a large radius; most are hidden. */
#define DOOM_FLASH_BINS 1024
static inline float doom_flash_segment_distance(float x,float y,const doom_light_blocker_t *b) {
    float ex=b->line[2]-b->line[0],ey=b->line[3]-b->line[1],px=x-b->line[0],py=y-b->line[1],length2=ex*ex+ey*ey;
    float t=length2>0?fminf(1.0f,fmaxf(0.0f,(px*ex+py*ey)/length2)):0;
    return sqrtf((px-ex*t)*(px-ex*t)+(py-ey*t)*(py-ey*t));
}
static inline uint32_t doom_flash_cull_hidden(const doom_flash_t *f,doom_light_blocker_t *blockers,uint32_t count) {
    const float pi=3.14159265f,width=2*pi/DOOM_FLASH_BINS;
    float x=f->position[0],y=f->position[1],z=f->position[2],r=f->position[3];
    float depth[DOOM_FLASH_BINS];
    for(int k=0;k<DOOM_FLASH_BINS;++k) depth[k]=1e30f;
    if(f->direction[3]>0) {
        /* Azimuths a cone of half-angle a around an axis at elevation e
         * reaches: within asin(sin a/cos e) of the axis, or all around. */
        float h=sqrtf(f->direction[0]*f->direction[0]+f->direction[1]*f->direction[1]);
        float s=sqrtf(fmaxf(0.0f,1-f->direction[3]*f->direction[3]));
        if(h>s+0.001f) {
            float center=atan2f(f->direction[1],f->direction[0]),reach=asinf(s/h)+2.5f*width;
            for(int k=0;k<DOOM_FLASH_BINS;++k) {
                float d=remainderf(-pi+(k+0.5f)*width-center,2*pi);
                if(fabsf(d)>reach) depth[k]=-1e30f;
            }
        }
    }
    for(uint32_t i=0;i<count;++i) {
        const doom_light_blocker_t *b=&blockers[i];
        int closed=b->opening[0]+0.02f>=b->opening[1]-0.02f;
        if(!(closed||b->opening[0]+0.01f>=z+r||b->opening[1]-0.01f<=z-r)) continue;
        if(doom_flash_segment_distance(x,y,b)<1+0.002f*r) continue; /* t>0.001 */
        float ax=b->line[0]-x,ay=b->line[1]-y,ex=b->line[2]-b->line[0],ey=b->line[3]-b->line[1];
        float a0=atan2f(ay,ax),d=remainderf(atan2f(b->line[3]-y,b->line[2]-x)-a0,2*pi);
        float start=fmodf((d>=0?a0:a0+d)+3*pi,2*pi)+1e-4f,end=start+fabsf(d)-2e-4f;
        float numerator=doom_flash_cross(ax,ay,ex,ey);
        for(int k=(int)ceilf(start/width);(k+1)*width<=end;++k) {
            float far=0;
            for(int edge=0;edge<2;++edge) {
                float angle=-pi+(k+edge)*width,along=doom_flash_cross(cosf(angle),sinf(angle),ex,ey);
                float distance=fabsf(along)>1e-6f?numerator/along:1e30f;
                far=fmaxf(far,distance>0?distance:1e30f);
            }
            int bin=k%DOOM_FLASH_BINS;
            depth[bin]=fminf(depth[bin],far);
        }
    }
    uint32_t kept=0;
    for(uint32_t i=0;i<count;++i) {
        const doom_light_blocker_t *b=&blockers[i];
        float nearest=doom_flash_segment_distance(x,y,b);
        int hidden=nearest>=1;
        if(hidden) {
            float a0=atan2f(b->line[1]-y,b->line[0]-x),d=remainderf(atan2f(b->line[3]-y,b->line[2]-x)-a0,2*pi);
            float start=fmodf((d>=0?a0:a0+d)+3*pi,2*pi);
            int first=(int)floorf(start/width)-1,last=(int)floorf((start+fabsf(d))/width)+1;
            for(int k=first;k<=last&&hidden;++k)
                hidden=depth[(k%DOOM_FLASH_BINS+DOOM_FLASH_BINS)%DOOM_FLASH_BINS]<nearest-0.25f;
        }
        if(!hidden) blockers[kept++]=*b;
    }
    return kept;
}
/* Keep equivalent to flashFacing in platform/shaders/lighting.glsl. The unit normal faces
 * the viewer; color[3] blends from omnidirectional (0) to Lambert (1). */
static inline float doom_flash_facing(float x,float y,float z,const float normal[3],const doom_flash_t *f) {
    float lx=f->position[0]-x,ly=f->position[1]-y,lz=f->position[2]-z;
    float distance=sqrtf(lx*lx+ly*ly+lz*lz);
    float lambert=distance>0.001f?fmaxf(0.0f,(normal[0]*lx+normal[1]*ly+normal[2]*lz)/distance):1.0f;
    return 1.0f+(lambert-1.0f)*f->color[3];
}
#endif
