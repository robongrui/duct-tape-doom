#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Shadows use only WAD alpha coverage, independent of palette and sprite upscale.
// DECAL: bullet marks reuse the puff sprite's coverage as a dark, fading stain;
// flashlight silhouettes reuse a thing's sprite.
layout(set=2,binding=0) uniform sampler2D image;
layout(location=0) out vec4 outColor;
float shadowCoverage(vec2 uv) {
    vec2 size=vec2(textureSize(image,0));
    if(any(lessThan(uv,vec2(0)))||any(greaterThanEqual(uv,size))) return 0.0;
    return textureLod(image,uv/size,0.0).g;
}
void main() {
#ifdef DECAL
    float alpha=shadowCoverage(vUV);
    // Mode 512: hard-edged silhouettes keep the sprite's pixel outline.
    if((vMode&512u)!=0u) alpha=step(0.5,alpha);
    if(alpha<0.02) discard;
    outColor=vec4(0,0,0,alpha*vLight);
#else
    float softness=1.0;
    float alpha=shadowCoverage(vUV)*0.4;
    alpha+=shadowCoverage(vUV+vec2(softness,0))*0.15;
    alpha+=shadowCoverage(vUV-vec2(softness,0))*0.15;
    alpha+=shadowCoverage(vUV+vec2(0,softness))*0.15;
    alpha+=shadowCoverage(vUV-vec2(0,softness))*0.15;
    float along=clamp(vUV.y/float(textureSize(image,0).y),0.0,1.0);
    outColor=vec4(0,0,0,alpha*vLight*(0.35+0.65*along));
#endif
}
