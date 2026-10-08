/* Detail textures in the late-90s style: tileable grayscale grain, centred on
   0.5, that the world shader multiplies over walls and flats up close. The
   grain is generated here at startup and each texture picks a layer from its
   own palette colors, so no asset or WAD data is added or changed. */
#ifndef DOOM_DETAIL_TEXTURE_H
#define DOOM_DETAIL_TEXTURE_H
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Layers: rough mineral grain (stone, concrete, plaster and the default),
   fine brushed metal, fibres along v (wood), soft organic mottling. */
enum { DOOM_DETAIL_MINERAL, DOOM_DETAIL_METAL, DOOM_DETAIL_FIBRE, DOOM_DETAIL_ORGANIC, DOOM_DETAIL_LAYERS };
#define DOOM_DETAIL_SIZE 128
/* Mip levels down to 1x1; every level averages to the neutral 0.5. */
#define DOOM_DETAIL_LEVELS 8

static inline float doom_detail_hash(int x, int y, int seed)
{
    unsigned h=(unsigned)x*374761393u+(unsigned)y*668265263u+(unsigned)seed*2246822519u;
    h=(h^(h>>13))*1274126177u;
    return (float)((h^(h>>16))&0xffffffu)/16777215.0f;
}
/* Smooth value noise over cx by cy cells that tiles across the layer. */
static inline float doom_detail_noise(int x, int y, int cx, int cy, int seed)
{
    float u=(x+0.5f)*cx/DOOM_DETAIL_SIZE,v=(y+0.5f)*cy/DOOM_DETAIL_SIZE;
    int x0=(int)floorf(u),y0=(int)floorf(v);
    float fx=u-x0,fy=v-y0;
    fx=fx*fx*(3-2*fx);fy=fy*fy*(3-2*fy);
    int x1=(x0+1)%cx,y1=(y0+1)%cy;
    float a=doom_detail_hash(x0,y0,seed),b=doom_detail_hash(x1,y0,seed);
    float c=doom_detail_hash(x0,y1,seed),d=doom_detail_hash(x1,y1,seed);
    return (a+(b-a)*fx)+((c+(d-c)*fx)-(a+(b-a)*fx))*fy-0.5f;
}
/* Distance to the nearest of one jittered point per cell, tiling. */
static inline float doom_detail_cells(int x, int y, int cells, int seed)
{
    float u=(x+0.5f)*cells/DOOM_DETAIL_SIZE,v=(y+0.5f)*cells/DOOM_DETAIL_SIZE,best=4;
    int cu=(int)floorf(u),cv=(int)floorf(v);
    for (int j=-1;j<=1;++j) for (int i=-1;i<=1;++i) {
        int px=cu+i,py=cv+j,wx=(px%cells+cells)%cells,wy=(py%cells+cells)%cells;
        float dx=px+doom_detail_hash(wx,wy,seed)-u,dy=py+doom_detail_hash(wx,wy,seed+1)-v;
        float d=dx*dx+dy*dy;
        if (d<best) best=d;
    }
    return sqrtf(best);
}
static inline float doom_detail_sample(int layer, int x, int y)
{
    switch (layer) {
    case DOOM_DETAIL_METAL:
        /* Streaks along u with a fine speckle. */
        return doom_detail_noise(x,y,4,128,11)+0.6f*doom_detail_noise(x,y,8,64,12)+0.35f*doom_detail_noise(x,y,64,64,13);
    case DOOM_DETAIL_FIBRE:
        return doom_detail_noise(x,y,64,4,21)+0.5f*doom_detail_noise(x,y,128,8,22)+0.3f*doom_detail_noise(x,y,32,32,23);
    case DOOM_DETAIL_ORGANIC:
        return 1.2f*doom_detail_cells(x,y,8,31)+0.4f*doom_detail_noise(x,y,16,16,33)+0.3f*doom_detail_noise(x,y,64,64,34);
    default:
        /* Coarse to fine, with sparse pits. */
        return 0.35f*doom_detail_noise(x,y,8,8,1)+0.6f*doom_detail_noise(x,y,16,16,2)+doom_detail_noise(x,y,32,32,3)+
               0.8f*doom_detail_noise(x,y,64,64,4)-(doom_detail_hash(x,y,5)<0.015f?1.2f:0.0f);
    }
}
/* Fills DOOM_DETAIL_LAYERS layers of DOOM_DETAIL_SIZE squared bytes, then
   their mip chains: level by level, each level holding every layer. Returns
   the bytes written; levels must hold doom_detail_bytes(). Each layer has a
   mean of 0.5 and a standard deviation of contrast, clamped at three. */
static inline size_t doom_detail_bytes(void)
{
    size_t total=0;
    for (int s=DOOM_DETAIL_SIZE;s>=1;s/=2) total+=(size_t)s*s*DOOM_DETAIL_LAYERS;
    return total;
}
static inline size_t doom_detail_build(unsigned char *levels, float contrast)
{
    enum { N=DOOM_DETAIL_SIZE*DOOM_DETAIL_SIZE };
    float *values=(float*)malloc(sizeof(float)*N*DOOM_DETAIL_LAYERS);
    if (!values) return 0;
    for (int layer=0;layer<DOOM_DETAIL_LAYERS;++layer) {
        float *v=values+(size_t)layer*N;
        double sum=0,squares=0;
        for (int y=0;y<DOOM_DETAIL_SIZE;++y) for (int x=0;x<DOOM_DETAIL_SIZE;++x) {
            float s=doom_detail_sample(layer,x,y);
            v[y*DOOM_DETAIL_SIZE+x]=s;sum+=s;squares+=(double)s*s;
        }
        float mean=(float)(sum/N),deviation=(float)sqrt(squares/N-(sum/N)*(sum/N));
        double centred=0;
        for (int i=0;i<N;++i) {
            float z=(v[i]-mean)/(deviation>1e-6f?deviation:1.0f);
            v[i]=0.5f+contrast*(z<-3?-3:z>3?3:z);
            centred+=v[i];
        }
        /* Clamping shifts the mean slightly; pull it back to 0.5. */
        float shift=0.5f-(float)(centred/N);
        for (int i=0;i<N;++i) v[i]+=shift;
    }
    size_t offset=0;
    for (int s=DOOM_DETAIL_SIZE;s>=1;s/=2) {
        for (int layer=0;layer<DOOM_DETAIL_LAYERS;++layer) {
            float *v=values+(size_t)layer*N;
            for (int i=0;i<s*s;++i) {
                float c=v[i]<0?0:v[i]>1?1:v[i];
                levels[offset++]=(unsigned char)lrintf(c*255.0f);
            }
            /* Box filter in place for the next level. */
            if (s>1) for (int y=0;y<s/2;++y) for (int x=0;x<s/2;++x)
                v[y*(s/2)+x]=0.25f*(v[2*y*s+2*x]+v[2*y*s+2*x+1]+v[(2*y+1)*s+2*x]+v[(2*y+1)*s+2*x+1]);
        }
    }
    free(values);
    return offset;
}
/* pixels: palette index and coverage pairs; palette: PLAYPAL RGB. Returns
   the layer for a wall or flat, plus 4 when its fibres run along u (wood
   grain is found as luminance that changes much faster across one axis).
   Greys and anything unclear stay mineral. */
static inline int doom_detail_class(int w, int h, const unsigned char *pixels, const unsigned char *palette)
{
    size_t counts[5]={0},covered=0;
    double across=0,along=0;
    enum { BROWN=4 };
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        size_t i=(size_t)y*w+x;
        if (!pixels[2*i+1]) continue;
        const unsigned char *c=palette+pixels[2*i]*3;
        float r=c[0]/255.0f,g=c[1]/255.0f,b=c[2]/255.0f;
        float hi=r>g?(r>b?r:b):(g>b?g:b),lo=r<g?(r<b?r:b):(g<b?g:b),saturation=hi>0?(hi-lo)/hi:0,hue=0;
        if (hi>lo) {
            if (hi==r) hue=60*fmodf((g-b)/(hi-lo)+6,6);
            else if (hi==g) hue=60*((b-r)/(hi-lo)+2);
            else hue=60*((r-g)/(hi-lo)+4);
        }
        int kind=DOOM_DETAIL_MINERAL;
        if (saturation>=0.2f&&hi>=0.08f) {
            if (hue>=190&&hue<=260) kind=DOOM_DETAIL_METAL;
            else if (hue>=18&&hue<=50&&saturation>=0.25f) kind=BROWN;
            else if ((hue<18||hue>=320)&&saturation>=0.35f) kind=DOOM_DETAIL_ORGANIC;
        }
        ++counts[kind];++covered;
        float luma=0.299f*r+0.587f*g+0.114f*b;
        const unsigned char *right=palette+pixels[2*(y*w+(x+1)%w)]*3,*below=palette+pixels[2*(((y+1)%h)*w+x)]*3;
        float dx=(0.299f*right[0]+0.587f*right[1]+0.114f*right[2])/255.0f-luma;
        float dy=(0.299f*below[0]+0.587f*below[1]+0.114f*below[2])/255.0f-luma;
        across+=dx*dx;along+=dy*dy;
    }
    if (!covered) return DOOM_DETAIL_MINERAL;
    if (counts[DOOM_DETAIL_ORGANIC]*2>covered) return DOOM_DETAIL_ORGANIC;
    if (counts[DOOM_DETAIL_METAL]*5>covered*2) return DOOM_DETAIL_METAL;
    if (counts[BROWN]*2>covered) {
        if (across>along*3.0) return DOOM_DETAIL_FIBRE;
        if (along>across*3.0) return DOOM_DETAIL_FIBRE|4;
    }
    return DOOM_DETAIL_MINERAL;
}
#endif
