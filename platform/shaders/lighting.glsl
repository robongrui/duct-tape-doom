// Dynamic lights and fog. Expects the Camera block `c`, the Lights block and
// the Fog block; define BLOCKERS when the blocker storage buffer is bound.
float cross2(vec2 a,vec2 b) {return a.x*b.y-a.y*b.x;}
float lightFalloff(vec3 point,uint i) {
    vec3 delta=point-lights[i].position.xyz;
    float falloff=max(0.0,1.0-length(delta)/lights[i].position.w);
    if(falloff<=0.0) return 0.0;
    if(lights[i].direction.w>0.0) {
        float cosine=length(delta)>0.001?dot(normalize(delta),lights[i].direction.xyz):1.0;
        float cone=smoothstep(lights[i].direction.w,0.985,cosine);
        falloff*=cone;
    }
    return falloff;
}
float flashAtOpen(vec3 point,uint i) {
    float falloff=lightFalloff(point,i);
    if(falloff<=0.0) return 0.0;
    return lights[i].strength*falloff*falloff*(3.0-2.0*falloff);
}
#ifdef BLOCKERS
// Solid walls and closed portal spans stop the ray; matches doom_flash_at.
// The intersection tests scale by det instead of dividing per blocker.
float flashAt(vec3 point,uint i) {
    float amount=flashAtOpen(point,i);
    if(amount<=0.0) return 0.0;
    vec3 origin3=lights[i].position.xyz;
    vec3 delta=point-origin3;
    for(uint j=lights[i].first;j<lights[i].first+lights[i].count;++j) {
        vec4 line=blockers[j].line;
        vec2 edge=line.zw-line.xy,origin=line.xy-origin3.xy;
        float det=cross2(delta.xy,edge);
        if(abs(det)<0.0001) continue;
        float t=cross2(origin,edge),u=cross2(origin,delta.xy);
        if(det<0.0) {det=-det;t=-t;u=-u;}
        if(t>0.001*det&&t<0.999*det&&u>=0.0&&u<=det) {
            float hit=origin3.z+delta.z*(t/det);
            vec2 opening=blockers[j].opening.xy;
            if(hit<=opening.x+0.02||hit>=opening.y-0.02) return 0.0;
        }
    }
    return amount;
}
#endif
// Lambert facing, blended toward 1 by the light's directionality (color.w):
// near-surface emissive sources stay soft so they still light their own wall.
float flashFacing(vec3 point,vec3 normal,uint i) {
    vec3 toLight=lights[i].position.xyz-point;
    float distance=length(toLight);
    float lambert=distance>0.001?max(0.0,dot(normal,toLight)/distance):1.0;
    return mix(1.0,lambert,lights[i].color.w);
}
// Fog: analytic exponential height extinction, an analytic headlamp glow, and
// one soft ray/sphere glow per nearby light. No marching through the volume.
// Use the denser endpoint for downward rays to avoid an overflowing exp(-x).
// Density thickens exponentially below the median floor, but stops 96 units
// down: deep shafts (Freedoom has some hundreds of units deep) would
// otherwise fog solid. Below that the ray sees the capped density.
float heightFogTau(float oz,float pz,float distance,float density,float falloff,float reference) {
    float lowest=reference-96.0,lo=min(oz,pz),hi=max(oz,pz);
    float below=hi-lo<1e-3?(lo<lowest?1.0:0.0):clamp((lowest-lo)/(hi-lo),0.0,1.0);
    float start=max(lo,lowest),x=falloff*(max(hi,lowest)-start);
    float f=x<1e-3?1.0-x*0.5+x*x/6.0:(1.0-exp(-x))/x;
    return density*distance*(below*exp(falloff*96.0)+
        (1.0-below)*exp(clamp(-falloff*(start-reference),-80.0,80.0))*f);
}
float fogBeamIntegral(float distance,float radius) {
    float u=clamp(distance/max(radius,0.001),0.0,1.0);
    return radius*(u-u*u*u+0.5*u*u*u*u);
}
uint fogLight(uint n) {return n==0u?fogA.y:n==1u?fogA.z:n==2u?fogA.w:fogB.x;}
vec4 fogAlong(vec3 end) {
    float distance=length(end-c.eye.xyz);
    if(c.materials.z<=0.0||c.effects.z!=0.0||distance<0.01) return vec4(0,0,0,1);
    vec3 ray=(end-c.eye.xyz)/distance;
    float transmission=exp(-heightFogTau(c.eye.z,end.z,distance,c.fog.x,c.fog.y,c.fog.z));
    distance=min(distance,2000.0); // Bound only the approximate light-glow work.
    vec3 glow=vec3(0.045,0.055,0.065)*(1.0-transmission);
    for(uint n=0u;n<fogA.x;++n) {
        uint i=fogLight(n);
        vec3 offset=lights[i].position.xyz-c.eye.xyz;
        float radius=lights[i].position.w,energy=0.0,center=0.0;
        if(lights[i].direction.w>0.0&&dot(offset,offset)<1600.0) {
#ifdef NO_BEAM_FOG
            continue;
#endif
            // The handheld beam starts within a few dozen units of the eye, so
            // its angle is nearly constant along a view ray. Integrate its
            // radial fade directly; geometry already ends it.
            float cone=smoothstep(lights[i].direction.w,0.985,dot(ray,lights[i].direction.xyz));
            energy=lights[i].strength*cone*fogBeamIntegral(distance,radius);
            center=min(distance,radius)*0.3;
        } else {
            float along=dot(offset,ray);
            float perpendicular2=max(0.0,dot(offset,offset)-along*along);
            float chord2=radius*radius-perpendicular2;
            if(chord2<=0.0) continue;
            float chord=sqrt(chord2);
            float near=max(0.0,along-chord),far=min(distance,along+chord);
            if(far<=near) continue;
            center=clamp(along,near,far);
            // Unoccluded on purpose: a portal test per pixel and fog light
            // doubled the fog cost. Geometry still ends each ray at walls.
            float amount=flashAtOpen(c.eye.xyz+ray*center,i);
            energy=amount*(far-near)*0.5;
        }
        glow+=lights[i].color.rgb*energy*(0.00035*0.38)*exp(-heightFogTau(c.eye.z,c.eye.z+ray.z*center,center,c.fog.x,c.fog.y,c.fog.z));
    }
    return vec4(glow,transmission);
}
