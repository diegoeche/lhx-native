#pragma once
/* These shaders are original host-renderer code. Camera projection follows the
 * existing native scene convention (+Z forward, 15/16 vertical pixel aspect).
 * C stringification keeps GLSL embedded, independent of the runtime directory. */
#define LHX_GLSL(...) #__VA_ARGS__
#define LHX_CAMERA LHX_GLSL( \
 uniform vec3 camera_projection; uniform vec2 canvas; uniform int antialiasing; \
 vec2 project_point(vec3 p) { return vec2(camera_projection.x+camera_projection.z*p.x/p.z, \
 camera_projection.y-0.9375*camera_projection.z*p.y/p.z)*canvas/vec2(320,200); } \
 vec4 clip_point(vec3 p,float bias) { return vec4( \
 (2.0*camera_projection.x/320.0-1.0)*p.z+2.0*camera_projection.z*p.x/320.0, \
 (1.0-2.0*camera_projection.y/200.0)*p.z+2.0*0.9375*camera_projection.z*p.y/200.0, \
 4.0*(1.0+bias)-p.z,p.z); } )
#define LHX_ATMOSPHERE LHX_GLSL( \
 uniform vec3 haze_color; uniform float haze_density; \
 vec3 apply_haze(vec3 value,float forward) { \
 float d=max(0.0,forward)*haze_density;return mix(haze_color,value,exp(-d*d)); } )

#define LHX_DRAPE LHX_GLSL( \
 uniform mat3 ground_basis,ground_inverse;uniform vec3 ground_camera;uniform float camera_ground; \
 uniform sampler2D near_heights,far_heights;uniform vec4 near_height_bounds,far_height_bounds; \
 uniform ivec3 near_grid,far_grid; \
 float grid_height(sampler2D image,vec4 bounds,ivec3 grid,vec2 p){ \
   vec2 g=(vec2((p.x-bounds.x)/(bounds.y-bounds.x),(bounds.w-p.y)/(bounds.w-bounds.z))*float(grid.x)-0.5)/float(grid.y); \
   if(grid.z<2 || any(lessThan(g,vec2(0))) || any(greaterThan(g,vec2(grid.z-1))))return -1.0; \
   ivec2 i=min(ivec2(floor(g)),ivec2(grid.z-2));vec2 f=g-vec2(i); \
   float a=texelFetch(image,i,0).r,b=texelFetch(image,i+ivec2(1,0),0).r; \
   float c=texelFetch(image,i+ivec2(0,1),0).r,d=texelFetch(image,i+ivec2(1,1),0).r; \
   return f.x+f.y<=1.0?a+(b-a)*f.x+(c-a)*f.y:d+(c-d)*(1.0-f.x)+(b-d)*(1.0-f.y); } \
 vec3 draped_position(vec3 p){ \
   vec3 world=ground_camera+ground_inverse*p; \
   float h=grid_height(near_heights,near_height_bounds,near_grid,world.xz); \
   if(h<0)h=grid_height(far_heights,far_height_bounds,far_grid,world.xz); \
   world.y=max(0.0,h);return 0.998*(ground_basis*(world-ground_camera-vec3(0,camera_ground,0))); } )

static const char *gpu_vertex_source="#version 330 core\n" LHX_CAMERA LHX_DRAPE LHX_GLSL(
 layout(location=0) in vec3 position; layout(location=1) in vec2 texcoord;
 layout(location=2) in vec4 color; layout(location=3) in float bias;
 layout(location=4) in float radius; layout(location=5) in float tint;layout(location=6) in float detail;
 out vec2 uv; out vec4 rgba;out float camera_z;flat out int surface_kind;
 void main(){ vec3 p=radius<0.0?draped_position(position):position;gl_Position=clip_point(p,bias);uv=texcoord;rgba=vec4(color.bgr*tint,color.a);camera_z=p.z;surface_kind=int(detail); }
);
static const char *gpu_fragment_source="#version 330 core\n" LHX_ATMOSPHERE LHX_GLSL(
 uniform sampler2D image;uniform int textured;uniform int stipple;uniform vec2 canvas;
 in vec2 uv;in vec4 rgba;in float camera_z;flat in int surface_kind;out vec4 result;
 float detail_hash(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
 float detail_noise(vec2 p){vec2 i=floor(p),f=fract(p),u=f*f*(3.0-2.0*f);
 return mix(mix(detail_hash(i),detail_hash(i+vec2(1,0)),u.x),mix(detail_hash(i+vec2(0,1)),detail_hash(i+vec2(1,1)),u.x),u.y);}
 float surface_detail(vec2 p,int kind){
 float k=(0.93+0.14*detail_noise(p*0.3))*(0.97+0.06*detail_noise(p*1.7));
 if(kind==2){float c=detail_noise(p*0.05+7.3)+0.5*detail_noise(p*0.13+2.1);return k*mix(1.0,0.68,smoothstep(0.88,0.95,c));}
 vec2 g=p/vec2(9,6),w=max(fwidth(g),vec2(0.00000001));
 vec2 line=1.0-smoothstep(vec2(0),w*1.2,abs(fract(g-0.5)-0.5));
 return k*(1.0-0.2*max(line.x,line.y)*clamp(1.0-max(w.x,w.y)*5.0,0,1));}

 void main(){
   if(stipple==1 && ((int(gl_FragCoord.x)^int(canvas.y-gl_FragCoord.y))&1)!=0)discard;
   result=textured!=0?texture(image,uv)*rgba:rgba;
   if(stipple>=2)result.a*=stipple==2?0.2:0.7;
   if(surface_kind>0)result.rgb*=surface_detail(uv,surface_kind);
   if(surface_kind==-2){vec2 d=uv*2.0-1.0;result.a*=max(0.0,exp(-dot(d,d)*4.0)-exp(-4.0))/(1.0-exp(-4.0));}
   result.rgb=apply_haze(result.rgb,camera_z);
 }
);
static const char *gpu_relief_vertex="#version 330 core\n" LHX_CAMERA LHX_GLSL(
 layout(location=0) in vec3 position;layout(location=1) in vec2 texcoord;
 layout(location=2) in float shade;
 uniform mat3 world_basis;uniform vec3 raised_camera;uniform float daylight;
 out vec2 uv;out vec4 rgba;out float camera_z;flat out int surface_kind;
 void main(){vec3 p=world_basis*(position-raised_camera);gl_Position=clip_point(p,0);
 uv=texcoord;rgba=vec4(vec3(shade*daylight),1);camera_z=p.z;surface_kind=0;}
);
static const char *gpu_prop_vertex="#version 330 core\n" LHX_CAMERA LHX_GLSL(
 layout(location=0) in vec3 position;layout(location=1) in vec2 texcoord;
 layout(location=2) in vec3 face_normal;
 uniform mat4 instance_view;uniform mat3 instance_normal,world_basis;
 uniform float daylight,shadow_opacity;uniform int light_model,is_shadow;
 out vec2 uv;out vec4 rgba;out float camera_z;flat out int surface_kind;
 void main(){
   vec3 p=(instance_view*vec4(position,1)).xyz;
   float shade=1.0;
   if(light_model!=0){
     vec3 n=instance_normal*face_normal;
     float side=dot(n,p)>0.0?-1.0:1.0;
     vec3 world=transpose(world_basis)*n;float len=length(world);
     if(len>0.000000000001){
       float sun=max(0.0,side*dot(world,vec3(-1,1.1,1))/(len*sqrt(3.21)));
       shade=clamp(0.55+0.6*sun+0.1*side*world.y/len,0.4,1.25);
     }
   }
   gl_Position=clip_point(p,0);uv=texcoord;camera_z=p.z;surface_kind=0;
   rgba=is_shadow!=0?vec4(0,0,0,shadow_opacity):vec4(vec3(daylight*shade),1);
 }
);
static const char *gpu_particle_vertex="#version 330 core\n" LHX_GLSL(
 layout(location=0) in vec3 position;layout(location=2) in vec4 color;
 layout(location=4) in float radius;
 out vec4 particle_color;out float particle_radius;
 void main(){gl_Position=vec4(position,1);particle_color=color.bgra;particle_radius=radius;}
);
static const char *gpu_particle_geometry="#version 330 core\n" LHX_CAMERA LHX_GLSL(
 layout(points) in;layout(triangle_strip,max_vertices=4) out;
 in vec4 particle_color[];in float particle_radius[];
 flat out vec3 center;flat out float size;flat out vec4 rgba;
 void corner(vec2 p){
   center=gl_in[0].gl_Position.xyz;size=max(0,particle_radius[0]);rgba=particle_color[0];
   gl_Position=vec4(2.0*p.x/canvas.x-1.0,1.0-2.0*p.y/canvas.y,0,1);EmitVertex();
 }
 void main(){
   center=gl_in[0].gl_Position.xyz;size=max(0,particle_radius[0]);rgba=particle_color[0];
   if(center.z+size<2.0)return;
   vec2 lo=vec2(0),hi=canvas;
   if(center.z-size>2.0){
     lo=canvas;hi=vec2(0);
     for(int z=-1;z<=1;z+=2)for(int x=-1;x<=1;x+=2)for(int y=-1;y<=1;y+=2){
       vec2 p=project_point(center+vec3(x*size,y*size*0.86,z*size));lo=min(lo,p);hi=max(hi,p);
     }
     float minimum=antialiasing!=0?1.0:max(0.5,0.5*min(canvas.x/320.0,canvas.y/200.0));
     vec2 midpoint=project_point(center);lo=min(lo,midpoint-minimum);hi=max(hi,midpoint+minimum);
   }
   corner(lo);corner(vec2(lo.x,hi.y));corner(vec2(hi.x,lo.y));corner(hi);EndPrimitive();
 }
);
static const char *gpu_particle_fragment="#version 330 core\n" LHX_CAMERA LHX_ATMOSPHERE LHX_GLSL(
 uniform int stipple;
 flat in vec3 center;flat in float size;flat in vec4 rgba;out vec4 result;
 void main(){
   vec2 pixel=vec2(gl_FragCoord.x,canvas.y-gl_FragCoord.y);
   if(stipple!=0 && ((int(pixel.x)^int(pixel.y))&1)!=0)discard;
   float area=3.14159265*(canvas.x/320.0)*(canvas.y/200.0)*0.9375*0.86*
                    pow(camera_projection.z*size/max(center.z,2.0),2.0);
   if(size==0.0 || (antialiasing!=0 && area<1.0 && center.z-size>2.0)){
     if(center.z<2.0)discard;
     vec2 delta=abs(pixel-project_point(center));float coverage;
     if(antialiasing!=0)coverage=max(0,1.0-delta.x)*max(0,1.0-delta.y)*(size>0?area:1.0);
     else coverage=length(delta)<=max(0.5,0.5*min(canvas.x/320.0,canvas.y/200.0))?1.0:0.0;
     if(coverage<0.001)discard;result=vec4(apply_haze(rgba.rgb,center.z),coverage);gl_FragDepth=2.001/center.z;return;
   }
   vec2 source=pixel*vec2(320,200)/canvas;
   vec3 ray=vec3((source.x-camera_projection.x)/camera_projection.z,
     (camera_projection.y-source.y)/(camera_projection.z*0.9375),1);
   vec3 scale=vec3(1,1.0/(0.86*0.86),1);
   float a=dot(ray*scale,ray),b=dot(ray*scale,center);
   float constant=dot(center*scale,center)-size*size;
   float d=b*b-a*constant;
   float coverage=antialiasing!=0?clamp(0.5+d/max(fwidth(d),0.000001),0,1):float(d>=0);
   if(coverage<=0.001)discard;
   float root=sqrt(max(d,0));float z=(b-root)/a;if(z<2.0)z=(b+root)/a;
   if(z<2.0)discard;gl_FragDepth=2.0/z;result=vec4(apply_haze(rgba.rgb,z),coverage);
 }
);
static const char *gpu_fullscreen_vertex="#version 330 core\n" LHX_GLSL(
 out vec2 uv;
 void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2.0-1.0,0,1);}
);
static const char *gpu_overlay_fragment="#version 330 core\n" LHX_GLSL(
 uniform sampler2D image;in vec2 uv;out vec4 result;
 void main(){result=texture(image,vec2(uv.x,1.0-uv.y));if(result.a<0.5)discard;}
);
static const char *gpu_horizon_fragment="#version 330 core\n" LHX_CAMERA LHX_ATMOSPHERE LHX_GLSL(
 uniform vec4 horizon;uniform vec4 first_color,second_color;
 uniform int two_colors;uniform int enhanced_horizon,terrain_loaded,camera_horizon,photo_sky;
 uniform sampler2D sky_image;
 uniform mat3 inverse_basis;uniform vec4 sky_color,ground_color;out vec4 result;
 void main(){vec2 source=vec2(gl_FragCoord.x,canvas.y-gl_FragCoord.y)*vec2(320,200)/canvas;
 if(camera_horizon!=0){
   vec3 ray=normalize(inverse_basis*vec3((source.x-camera_projection.x)/camera_projection.z,
          (camera_projection.y-source.y)/(camera_projection.z*0.9375),1));
   float elevation=asin(clamp(ray.y,-1,1));
   vec3 base=ray.y>=0?sky_color.rgb:terrain_loaded!=0 && enhanced_horizon!=0?haze_color:ground_color.rgb;
   if(photo_sky!=0){
     /* This branch is uniform. Evaluate derivatives on both sides of the
      * horizon; the longitude discontinuity must not select a global mip. */
     float horizontal=length(ray.xz);
     float longitude=horizontal>0.00000001?atan(ray.x,ray.z):0.0;
     vec2 uv=vec2(0.5+longitude/6.283185307,0.5-elevation/3.141592654);
     vec2 dx=dFdx(uv),dy=dFdy(uv);
     dx.x-=floor(dx.x+0.5);dy.x-=floor(dy.x+0.5);
     if(ray.y>=0)base=textureGrad(sky_image,uv,dx,dy).rgb*0.78;
   }
   result=vec4(enhanced_horizon!=0?mix(base,haze_color,exp(-abs(elevation)*40.0)):base,1);return;
 }
 float side=dot(source-horizon.xy,horizon.zw);
 if(two_colors==0)result=first_color;
 else if(antialiasing!=0)result=mix(second_color,first_color,clamp(0.5+side/max(fwidth(side),0.000001),0,1));
 else result=side>=0?first_color:second_color;}
);
static const char *gpu_terrain_fragment="#version 330 core\n" LHX_CAMERA LHX_ATMOSPHERE LHX_GLSL(
 uniform mat3 inverse_basis;uniform vec3 world_camera;uniform vec4 near_bounds,far_bounds;
 uniform sampler2D near_image,far_image;uniform int near_ready,far_ready;uniform float daylight;
 uniform int enhanced_horizon;
 out vec4 result;
 bool covers(vec4 bounds,vec2 p){return p.x>=bounds.x && p.x<=bounds.y && p.y>=bounds.z && p.y<=bounds.w;}
 vec2 coordinates(vec4 bounds,vec2 p){return vec2((p.x-bounds.x)/(bounds.y-bounds.x),(bounds.w-p.y)/(bounds.w-bounds.z));}
 void main(){
   vec2 pixel=vec2(gl_FragCoord.x,canvas.y-gl_FragCoord.y)*vec2(320,200)/canvas;
   vec3 ray=inverse_basis*vec3((pixel.x-camera_projection.x)/camera_projection.z,
                   (camera_projection.y-pixel.y)/(camera_projection.z*0.9375),1);
   if(ray.y>=-0.000000001 || world_camera.y<=0)discard;
   float distance=-world_camera.y/ray.y;
   vec2 p=(world_camera+ray*distance).xz;
   /* Calculate derivatives before divergent coverage selection near tile edges. */
   vec2 nuv=coordinates(near_bounds,p),fuv=coordinates(far_bounds,p);
   vec4 n=textureGrad(near_image,nuv,dFdx(nuv),dFdy(nuv));
   vec4 f=textureGrad(far_image,fuv,dFdx(fuv),dFdy(fuv));
   if(near_ready!=0 && covers(near_bounds,p))result=vec4(n.rgb*daylight,1);
   else if(far_ready!=0 && covers(far_bounds,p))result=vec4(f.rgb*daylight,1);
   else if(enhanced_horizon!=0)result=vec4(haze_color,1);
   else discard;
   result.rgb=apply_haze(result.rgb,distance);
 }
);
#undef LHX_DRAPE
#undef LHX_CAMERA
#undef LHX_ATMOSPHERE
#undef LHX_GLSL
