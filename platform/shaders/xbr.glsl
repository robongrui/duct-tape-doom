/* Adaptation of Hyllian's xBR-lv2-noblend shader (license: third_party/xbr).
 * Changes: direct indexed/palette reads, RGBA color selection, alpha-aware
 * edge classification, and no color blending on enlarged sprites.
 * Upstream: https://github.com/libretro/glsl-shaders/blob/master/xbr/shaders/xbr-lv2-noblend.glsl
 * Copyright (C) 2011-2016 Hyllian - sergiogdb@gmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 * Upstream incorporates ideas from Joshua Street's SABR shader.
 */
vec4 xbrTexel(sampler2D image,sampler2D palette,ivec2 p) {
    if(any(lessThan(p,ivec2(0)))||any(greaterThanEqual(p,textureSize(image,0)))) return vec4(0);
    return texel(image,palette,p,false);
}
float xbrLuma(vec4 color) {
    // Transparent pixels form a separate region, including opaque black edges.
    return dot(color.rgb,vec3(0.2126,0.7152,0.0722))*color.a-4.0*(1.0-color.a);
}
vec4 xbrDifferent(vec4 a,vec4 b) { return vec4(notEqual(a,b)); }
vec4 xbrDistance(vec4 a,vec4 b,vec4 c,vec4 d,vec4 e,vec4 f,vec4 g,vec4 h) {
    return abs(a-b)+abs(a-c)+abs(d-e)+abs(d-f)+4.0*abs(g-h);
}
vec4 xbrSprite(sampler2D image,sampler2D palette,vec2 uv) {
    ivec2 p=ivec2(floor(uv)); vec2 fp=fract(uv);
    vec4 A1=xbrTexel(image,palette,p+ivec2(-1,-2));
    vec4 B1=xbrTexel(image,palette,p+ivec2( 0,-2));
    vec4 C1=xbrTexel(image,palette,p+ivec2( 1,-2));
    vec4 A =xbrTexel(image,palette,p+ivec2(-1,-1));
    vec4 B =xbrTexel(image,palette,p+ivec2( 0,-1));
    vec4 C =xbrTexel(image,palette,p+ivec2( 1,-1));
    vec4 D =xbrTexel(image,palette,p+ivec2(-1, 0));
    vec4 E =xbrTexel(image,palette,p);
    vec4 F =xbrTexel(image,palette,p+ivec2( 1, 0));
    vec4 G =xbrTexel(image,palette,p+ivec2(-1, 1));
    vec4 H =xbrTexel(image,palette,p+ivec2( 0, 1));
    vec4 I =xbrTexel(image,palette,p+ivec2( 1, 1));
    vec4 G5=xbrTexel(image,palette,p+ivec2(-1, 2));
    vec4 H5=xbrTexel(image,palette,p+ivec2( 0, 2));
    vec4 I5=xbrTexel(image,palette,p+ivec2( 1, 2));
    vec4 A0=xbrTexel(image,palette,p+ivec2(-2,-1));
    vec4 D0=xbrTexel(image,palette,p+ivec2(-2, 0));
    vec4 G0=xbrTexel(image,palette,p+ivec2(-2, 1));
    vec4 C4=xbrTexel(image,palette,p+ivec2( 2,-1));
    vec4 F4=xbrTexel(image,palette,p+ivec2( 2, 0));
    vec4 I4=xbrTexel(image,palette,p+ivec2( 2, 1));
    vec4 b=vec4(xbrLuma(B),xbrLuma(D),xbrLuma(H),xbrLuma(F));
    vec4 c=vec4(xbrLuma(C),xbrLuma(A),xbrLuma(G),xbrLuma(I));
    vec4 e=vec4(xbrLuma(E)),d=b.yzwx,f=b.wxyz,g=c.zwxy,h=b.zwxy,i=c.wxyz;
    vec4 i4=vec4(xbrLuma(I4),xbrLuma(C1),xbrLuma(A0),xbrLuma(G5));
    vec4 i5=vec4(xbrLuma(I5),xbrLuma(C4),xbrLuma(A1),xbrLuma(G0));
    vec4 h5=vec4(xbrLuma(H5),xbrLuma(F4),xbrLuma(B1),xbrLuma(D0)),f4=h5.yzwx;
    vec4 a=vec4(1,-1,-1,1);
    vec4 fx=vec4(greaterThan(a*fp.y+vec4(1,1,-1,-1)*fp.x,vec4(1.5,0.5,-0.5,0.5)));
    vec4 fxl=vec4(greaterThan(a*fp.y+vec4(0.5,2,-0.5,-2)*fp.x,vec4(1,1,-0.5,0)));
    vec4 fxu=vec4(greaterThan(a*fp.y+vec4(2,0.5,-2,-0.5)*fp.x,vec4(2,0,-1,0.5)));
    vec4 restriction=xbrDifferent(e,f)*xbrDifferent(e,h);
    vec4 restrictionL=xbrDifferent(e,g)*xbrDifferent(d,g);
    vec4 restrictionU=xbrDifferent(e,c)*xbrDifferent(b,c);
    vec4 distance1=xbrDistance(e,c,g,i,h5,f4,h,f);
    vec4 distance2=xbrDistance(h,d,i5,f,i4,b,e,i);
    vec4 edge=step(distance1+0.1,distance2)*step(vec4(0.5),restriction);
    vec4 edgeL=step(2.0*abs(f-g),abs(h-c))*restrictionL*edge;
    vec4 edgeU=step(2.0*abs(h-c),abs(f-g))*restrictionU*edge;
    bvec4 corner=greaterThan(edge*(fx+edgeL*fxl)+edgeU*fxu,vec4(0));
    bvec4 pick=lessThanEqual(abs(e-f),abs(e-h));
    vec4 res1=corner.x?(pick.x?F:H):corner.y?(pick.y?B:F):corner.z?(pick.z?D:B):E;
    vec4 res2=corner.w?(pick.w?H:D):corner.z?(pick.z?D:B):corner.y?(pick.y?B:F):E;
    return abs(xbrLuma(res1)-e.x)<=abs(xbrLuma(res2)-e.y)?res2:res1;
}
