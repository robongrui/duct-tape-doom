#version 450
// World, sprite and screen-space geometry; REFLECT adds the liquid clip plane.
#extension GL_GOOGLE_include_directive : require
#define VERTEX
#include "common.glsl"
layout(location=0) in vec3 inPosition;
layout(location=1) in vec2 inUV;
layout(location=2) in float inLight;
layout(location=3) in uint inMode;
layout(location=4) in vec3 inTint;
layout(location=5) in uvec2 inLightMask;
layout(location=6) in vec2 inSun;
layout(location=7) in uint inStatic;
layout(std140,set=1,binding=0) uniform Camera CAMERA_BLOCK c;
#ifdef REFLECT
// Mirrored-camera pass: geometry below the surface never appears in the reflection.
layout(std140,set=1,binding=1) uniform Plane { float plane; };
out float gl_ClipDistance[1];
#endif
void main() {
    vUV=inUV;vLight=inLight;vMode=inMode;vWorld=inPosition;vTint=inTint;vLightMask=inLightMask;vSun=inSun;vStatic=inStatic;
    if((inMode&1u)!=0u) {gl_Position=vec4(inPosition.xy,0,1);vDistance=0;}
    else {
        vec3 d=inPosition-c.eye.xyz;
        float z=dot(d,c.forward.xyz);vDistance=z;
        float n=c.projection.z,f=c.projection.w;
        gl_Position=vec4(dot(d,c.right.xyz)*c.projection.x,
                         dot(d,c.up.xyz)*c.projection.y,z*f/(f-n)-n*f/(f-n),z);
    }
#ifdef REFLECT
    gl_ClipDistance[0]=inPosition.z-plane-0.5;
#endif
}
