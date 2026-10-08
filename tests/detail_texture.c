/* Data-free checks for detail textures (platform/detail_texture.h). */
#include "../platform/detail_texture.h"
#include <stdio.h>

static void check(int condition, const char *message)
{
    if (!condition) { fprintf(stderr, "%s\n", message); exit(1); }
}
int main(void)
{
    enum { W=64, H=64, N=DOOM_DETAIL_SIZE*DOOM_DETAIL_SIZE };
    unsigned char *levels=(unsigned char*)malloc(doom_detail_bytes());
    check(levels && doom_detail_build(levels,0.16f)==doom_detail_bytes(), "Every mip level must be written");
    for (int layer=0;layer<DOOM_DETAIL_LAYERS;++layer) {
        double sum=0;
        for (int i=0;i<N;++i) sum+=levels[(size_t)layer*N+i];
        check(sum/N>126.5 && sum/N<128.5, "Each layer must average to the neutral 0.5");
    }
    const unsigned char *last=levels+doom_detail_bytes()-DOOM_DETAIL_LAYERS;
    for (int layer=0;layer<DOOM_DETAIL_LAYERS;++layer)
        check(last[layer]>=126 && last[layer]<=129, "The smallest mip must be neutral grey");
    free(levels);

    unsigned char palette[768]={0};
    palette[3]=palette[4]=palette[5]=120;                  /* 1: grey */
    palette[6]=60; palette[7]=70; palette[8]=160;          /* 2: blue */
    palette[9]=120; palette[10]=80; palette[11]=40;        /* 3: brown */
    palette[12]=70; palette[13]=45; palette[14]=20;        /* 4: dark brown */
    palette[15]=180; palette[16]=40; palette[17]=40;       /* 5: red */
    static unsigned char pixels[W*H*2];
    for (int i=0;i<W*H;++i) { pixels[2*i]=1; pixels[2*i+1]=255; }
    check(doom_detail_class(W,H,pixels,palette)==DOOM_DETAIL_MINERAL, "Greys must stay mineral");
    for (int i=0;i<W*H;++i) pixels[2*i]=2;
    check(doom_detail_class(W,H,pixels,palette)==DOOM_DETAIL_METAL, "Blues must read as metal");
    for (int i=0;i<W*H;++i) pixels[2*i]=5;
    check(doom_detail_class(W,H,pixels,palette)==DOOM_DETAIL_ORGANIC, "Reds must read as organic");
    /* Planks: grain lines down the texture, then across it; bricks stay mineral. */
    for (int y=0;y<H;++y) for (int x=0;x<W;++x) pixels[2*(y*W+x)]=x%3?3:4;
    check(doom_detail_class(W,H,pixels,palette)==DOOM_DETAIL_FIBRE, "Vertical grain must be fibre along v");
    for (int y=0;y<H;++y) for (int x=0;x<W;++x) pixels[2*(y*W+x)]=y%3?3:4;
    check(doom_detail_class(W,H,pixels,palette)==(DOOM_DETAIL_FIBRE|4), "Horizontal grain must be fibre along u");
    for (int y=0;y<H;++y) for (int x=0;x<W;++x) pixels[2*(y*W+x)]=(y%8==0||(x+(y/8)*8)%16==0)?4:3;
    check(doom_detail_class(W,H,pixels,palette)==DOOM_DETAIL_MINERAL, "Brown bricks must stay mineral");
    puts("detail texture checks passed");
    return 0;
}
