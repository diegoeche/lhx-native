#include "scene_gpu.h"
#include "scene_geometry.h"
#include "scene_atmosphere.h"
#include "scene_sky.h"
#include "scene_relief.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef LHX_HAVE_OPENGL
#define GL_GLEXT_PROTOTYPES
#include <SDL_opengl.h>
#include "scene_gpu_shaders.h"

enum { MAX_GPU_VERTICES=1048576,MAX_GPU_COMMANDS=65536,TEXTURES=128,QUERIES=8,PROP_CACHE=64,PROP_GROUPS=16 };
typedef struct {
    float position[3],uv[2];uint32_t color;float bias,radius,tint,detail;
} GpuVertex;
typedef struct { unsigned first,count,kind;GLuint texture;int cull,stipple;float line_width; } Command;
typedef struct {
    const LhxTexture *source;const uint32_t *pixels;uint64_t revision,last_used;
    GLuint name;
} Texture;
typedef struct {GLuint array,buffer,index,height;uint64_t revision,height_revision;const LhxTerrainGrid *source,*height_source;} ReliefBuffer;
typedef struct {float position[3],uv[2],normal[3];} PropVertex;
typedef struct {GLuint array,buffer;unsigned count;} PropGroup;
typedef struct {
    const LhxPropModel *source;const LhxPropVertex *vertices;uint64_t revision,last_used;
    unsigned count;PropGroup groups[PROP_GROUPS];
} PropBuffer;
struct LhxGpu {
    SDL_Window *window;SDL_GLContext context;
    unsigned width,height,samples;int antialiasing,timings;
    float max_line_width;
    GLuint scene_fbo,scene_color,scene_depth,final_fbo,final_color,overlay;
    GLuint vertex_array,buffer,geometry,particles,horizon,terrain,composite,relief_program;
    ReliefBuffer relief_buffers[2];int relief;
    GLuint prop_program;PropBuffer props[PROP_CACHE];
    uint64_t prop_instances,prop_uploaded_vertices,prop_drawn_vertices,streamed_vertices,gpu_draped_vertices;
    GpuVertex *vertices;Command *commands;unsigned vertex_count,command_count;
    size_t vertex_capacity,command_capacity;
    LhxSceneView view_state;
    const LhxSceneView *view;const LhxSceneRenderOptions *options;
    Texture textures[TEXTURES];uint64_t frame;
    unsigned last_texture;
    GLuint queries[QUERIES][2];unsigned query_cursor;int query_pending[QUERIES];
    uint64_t timed_draws,total_ns,max_ns,submitted_draws,readbacks,invalid_timestamps;
    char device[256],vendor[128],version[128];int software;
    int atmosphere,detail,shadow,shadow_started;
    uint32_t haze;
};
static void flush(LhxGpu *);
static int inverse(const double m[3][3],float out[9]);

static GLuint shader(GLenum kind,const char *source)
{
    GLuint s=glCreateShader(kind);glShaderSource(s,1,&source,NULL);glCompileShader(s);
    GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[4096];glGetShaderInfoLog(s,sizeof log,NULL,log);fprintf(stderr,"[gpu] shader: %s\n",log);glDeleteShader(s);return 0;}
    return s;
}
static GLuint program(const char *vs,const char *fs,const char *gs)
{
    GLuint a=shader(GL_VERTEX_SHADER,vs),b=shader(GL_FRAGMENT_SHADER,fs);
    GLuint c=gs?shader(GL_GEOMETRY_SHADER,gs):0,p=0;
    if(a && b && (!gs || c)){
        p=glCreateProgram();glAttachShader(p,a);glAttachShader(p,b);if(c)glAttachShader(p,c);
        glLinkProgram(p);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);
        if(!ok){char log[4096];glGetProgramInfoLog(p,sizeof log,NULL,log);fprintf(stderr,"[gpu] link: %s\n",log);glDeleteProgram(p);p=0;}
    }
    glDeleteShader(a);glDeleteShader(b);if(c)glDeleteShader(c);return p;
}
static void integer(GLuint p,const char *name,int v){glUniform1i(glGetUniformLocation(p,name),v);}
static void scalar(GLuint p,const char *name,float v){glUniform1f(glGetUniformLocation(p,name),v);}
static void vector2(GLuint p,const char *name,float x,float y){glUniform2f(glGetUniformLocation(p,name),x,y);}
static void color(GLuint p,const char *name,uint32_t c){glUniform4f(glGetUniformLocation(p,name),((c>>16)&255)/255.f,((c>>8)&255)/255.f,(c&255)/255.f,1);}
static void camera(LhxGpu *g,GLuint p)
{
    vector2(p,"canvas",g->width,g->height);
    glUniform3f(glGetUniformLocation(p,"camera_projection"),g->view->cx,g->view->cy,g->view->focus);
    integer(p,"antialiasing",g->options && g->options->antialiasing);
    integer(p,"enhanced_horizon",g->atmosphere);
    scalar(p,"haze_density",g->atmosphere?LHX_HAZE_DENSITY:0);
    glUniform3f(glGetUniformLocation(p,"haze_color"),((g->haze>>16)&255)/255.f,
                 ((g->haze>>8)&255)/255.f,(g->haze&255)/255.f);
}
static GLuint texture(LhxGpu *g,const LhxTexture *source)
{
    if(!source || !source->count || source->count>13 || !source->levels[0].pixels ||
       !source->levels[0].width || !source->levels[0].height ||
       source->levels[0].width>4096 || source->levels[0].height>4096)return 0;
    Texture *last=g->textures+g->last_texture;
    if(last->source==source && last->pixels==source->levels[0].pixels && last->revision==source->revision){
        last->last_used=g->frame;return last->name;
    }
    unsigned slot=0;
    for(unsigned i=0;i<TEXTURES;i++){
        Texture *t=g->textures+i;
        if(t->source==source){slot=i;break;}
        if(!t->source || t->last_used<g->textures[slot].last_used)slot=i;
    }
    Texture *t=g->textures+slot;
    if(t->source!=source || t->pixels!=source->levels[0].pixels || t->revision!=source->revision){
        flush(g); /* Pending commands may still reference this texture slot. */
        if(!t->name)glGenTextures(1,&t->name);
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,t->name);
        const LhxTextureLevel *l=source->levels;
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,l->width,l->height,0,GL_BGRA,GL_UNSIGNED_BYTE,l->pixels);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glGenerateMipmap(GL_TEXTURE_2D);
        t->source=source;t->pixels=l->pixels;t->revision=source->revision;
    }
    t->last_used=g->frame;g->last_texture=slot;return t->name;
}
static int reserve(LhxGpu *g,unsigned count)
{
    if(count>MAX_GPU_VERTICES)return 0;
    if(count>MAX_GPU_VERTICES-g->vertex_count || g->command_count==MAX_GPU_COMMANDS)flush(g);
    size_t need=g->vertex_count+count;
    if(need>g->vertex_capacity){
        size_t n=g->vertex_capacity?g->vertex_capacity*2:8192;
        while(n<need)n*=2;
        if(n>MAX_GPU_VERTICES)n=MAX_GPU_VERTICES;
        void *v=realloc(g->vertices,n*sizeof(*g->vertices));if(!v)return 0;
        g->vertices=v;g->vertex_capacity=n;
    }
    if(g->command_count==g->command_capacity){
        size_t n=g->command_capacity?g->command_capacity*2:1024;
        if(n>MAX_GPU_COMMANDS)n=MAX_GPU_COMMANDS;
        void *v=realloc(g->commands,n*sizeof(*g->commands));if(!v)return 0;
        g->commands=v;g->command_capacity=n;
    }
    return 1;
}
static int append(LhxGpu *g,const GpuVertex *v,unsigned n,unsigned kind,GLuint tex,int cull,int stipple,float line_width)
{
    if(!reserve(g,n))return 0;
    Command *c=g->command_count?g->commands+g->command_count-1:NULL;
    if(!c || c->kind!=kind || c->texture!=tex || c->cull!=cull || c->stipple!=stipple || c->line_width!=line_width){
        c=g->commands+g->command_count++;*c=(Command){g->vertex_count,0,kind,tex,cull,stipple,line_width};
    }
    memcpy(g->vertices+g->vertex_count,v,n*sizeof(*v));g->vertex_count+=n;c->count+=n;return 1;
}
static void ground_uniforms(LhxGpu *g,GLuint program)
{
    if(!g->relief)return;
    const LhxTerrain *terrain=g->options->terrain;
    const LhxTerrainGrid *grids[]={&terrain->near,&terrain->far};
    const char *bounds[]={"near_height_bounds","far_height_bounds"},*meta[]={"near_grid","far_grid"};
    float basis[9],inv[9];if(!inverse(g->view->world_to_view,inv))return;
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)basis[i*3+j]=g->view->world_to_view[i][j];
    glUniformMatrix3fv(glGetUniformLocation(program,"ground_basis"),1,GL_TRUE,basis);
    glUniformMatrix3fv(glGetUniformLocation(program,"ground_inverse"),1,GL_TRUE,inv);
    const double *camera=g->view->camera;
    glUniform3f(glGetUniformLocation(program,"ground_camera"),camera[0],camera[1],camera[2]);
    scalar(program,"camera_ground",lhx_relief_height(terrain,camera[0],camera[2]));
    for(unsigned i=0;i<2;i++){
        const LhxTerrainGrid *grid=grids[i];ReliefBuffer *buffer=g->relief_buffers+i;
        if(!buffer->height)glGenTextures(1,&buffer->height);
        glActiveTexture(GL_TEXTURE2+i);glBindTexture(GL_TEXTURE_2D,buffer->height);
        if(buffer->height_source!=grid || buffer->height_revision!=grid->relief_revision){
            glTexImage2D(GL_TEXTURE_2D,0,GL_R32F,grid->n,grid->n,0,GL_RED,GL_FLOAT,grid->heights);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            buffer->height_source=grid;buffer->height_revision=grid->relief_revision;
        }
        glUniform4f(glGetUniformLocation(program,bounds[i]),grid->bounds[0],grid->bounds[1],grid->bounds[2],grid->bounds[3]);
        glUniform3i(glGetUniformLocation(program,meta[i]),grid->source_n,grid->step,grid->n);
    }
    integer(program,"near_heights",2);integer(program,"far_heights",3);glActiveTexture(GL_TEXTURE0);
}
static void flush(LhxGpu *g)
{
    if(!g->vertex_count)return;
    glBindVertexArray(g->vertex_array);glBindBuffer(GL_ARRAY_BUFFER,g->buffer);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(g->vertex_count*sizeof(*g->vertices)),g->vertices,GL_STREAM_DRAW);
    g->streamed_vertices+=g->vertex_count;
    GLuint active=0;
    for(unsigned i=0;i<g->command_count;i++){
        const Command *c=g->commands+i;GLuint p=c->kind==2?g->particles:g->geometry;
        if(p!=active){glUseProgram(p);camera(g,p);if(p==g->geometry)ground_uniforms(g,p);active=p;}
        glDepthMask(!g->shadow && c->stipple<2);
        integer(p,"stipple",c->stipple);integer(p,"textured",c->texture!=0);
        integer(p,"image",0);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,c->texture);
        if(c->cull)glEnable(GL_CULL_FACE);else glDisable(GL_CULL_FACE);
        if(c->kind==1){
            float width=g->options && g->options->antialiasing?
                c->line_width*fminf(g->width/320.f,g->height/200.f):0.8f*g->width/320.f;
            glLineWidth(fminf(g->max_line_width,fmaxf(1.f,width)));
        }
        glDrawArrays(c->kind==0?GL_TRIANGLES:c->kind==1?GL_LINES:GL_POINTS,c->first,c->count);
    }
    g->vertex_count=g->command_count=0;glDisable(GL_CULL_FACE);
}
static int inverse(const double m[3][3],float out[9])
{
    double det=m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])-
               m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])+
               m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
    if(!isfinite(det) || fabs(det)<0.01)return 0;
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)
        out[j*3+i]=(m[(i+1)%3][(j+1)%3]*m[(i+2)%3][(j+2)%3]-
                    m[(i+1)%3][(j+2)%3]*m[(i+2)%3][(j+1)%3])/det;
    return 1;
}
static void draw_relief(LhxGpu *g,float daylight)
{
    GLuint p=g->relief_program;glUseProgram(p);camera(g,p);scalar(p,"daylight",daylight);
    integer(p,"textured",1);integer(p,"image",0);integer(p,"stipple",0);
    float basis[9];for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)basis[i*3+j]=g->view->world_to_view[i][j];
    glUniformMatrix3fv(glGetUniformLocation(p,"world_basis"),1,GL_TRUE,basis);
    const double *c=g->view->camera;const LhxTerrain *terrain=g->options->terrain;
    glUniform3f(glGetUniformLocation(p,"raised_camera"),c[0],c[1]+lhx_relief_height(terrain,c[0],c[2]),c[2]);
    const LhxTerrainGrid *grids[]={&terrain->far,&terrain->near};
    glEnable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);glClear(GL_DEPTH_BUFFER_BIT);
    for(unsigned i=0;i<2;i++){
        const LhxTerrainGrid *grid=grids[i];ReliefBuffer *b=g->relief_buffers+i;
        GLuint image=texture(g,&grid->texture);if(!image)continue;
        if(!b->array){glGenVertexArrays(1,&b->array);glGenBuffers(1,&b->buffer);glGenBuffers(1,&b->index);}
        glBindVertexArray(b->array);glBindBuffer(GL_ARRAY_BUFFER,b->buffer);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,b->index);
        if(b->source!=grid || b->revision!=grid->relief_revision){
            glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)grid->vertex_count*sizeof(*grid->vertices),grid->vertices,GL_STATIC_DRAW);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)grid->index_count*sizeof(*grid->indices),grid->indices,GL_STATIC_DRAW);
            b->source=grid;b->revision=grid->relief_revision;
            glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(LhxTerrainVertex),(void*)offsetof(LhxTerrainVertex,x));
            glEnableVertexAttribArray(1);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(LhxTerrainVertex),(void*)offsetof(LhxTerrainVertex,u));
            glEnableVertexAttribArray(2);glVertexAttribPointer(2,1,GL_FLOAT,GL_FALSE,sizeof(LhxTerrainVertex),(void*)offsetof(LhxTerrainVertex,shade));
        }
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,image);
        glDrawElements(GL_TRIANGLES,grid->index_count,GL_UNSIGNED_INT,0);
    }
    glBindVertexArray(g->vertex_array);
}
static void terrain(LhxGpu *g,float daylight)
{
    const LhxTerrain *t=g->options?g->options->terrain:NULL;
    if(!t || !g->view->has_camera)return;
    if(g->relief){draw_relief(g,daylight);return;}
    if(g->view->camera[1]<=0)return;
    float inv[9];if(!inverse(g->view->world_to_view,inv))return;
    GLuint near=texture(g,&t->near.texture),far=texture(g,&t->far.texture);
    if(!near && !far)return;
    GLuint p=g->terrain;glUseProgram(p);camera(g,p);scalar(p,"daylight",daylight);
    glUniformMatrix3fv(glGetUniformLocation(p,"inverse_basis"),1,GL_TRUE,inv);
    glUniform3f(glGetUniformLocation(p,"world_camera"),g->view->camera[0],g->view->camera[1],g->view->camera[2]);
    const LhxTerrainGrid *grids[]={&t->near,&t->far};const char *names[]={"near_bounds","far_bounds"};
    for(unsigned i=0;i<2;i++){
        const double *b=grids[i]->texture.count?grids[i]->bounds:grids[1-i]->bounds;
        glUniform4f(glGetUniformLocation(p,names[i]),b[0],b[1],b[2],b[3]);
    }
    integer(p,"near_ready",near!=0);integer(p,"far_ready",far!=0);
    integer(p,"near_image",0);integer(p,"far_image",1);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,near?near:far);
    glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,far?far:near);
    glDrawArrays(GL_TRIANGLES,0,3);glActiveTexture(GL_TEXTURE0);
}
static int begin_view(void *context,const LhxSceneView *view,const uint32_t colors[256],float daylight)
{
    LhxGpu *g=context;g->view_state=*view;g->view=&g->view_state;
    float inv[9];int has_inverse=view->has_camera && inverse(view->world_to_view,inv);
    g->atmosphere=g->options && g->options->atmosphere && has_inverse;
    g->shadow_started=0;g->shadow=0;g->detail=0;glDisable(GL_STENCIL_TEST);
    g->relief=g->options && g->options->terrain_relief && has_inverse && lhx_relief_ready(g->options->terrain);
    g->haze=lhx_haze_color(colors[view->sky_color&255]);
    const LhxSky *sky=g->options && view->has_camera && has_inverse?g->options->sky:NULL;
    GLuint panorama=sky?texture(g,g->haze==0xff0d0f17u?&sky->night:&sky->day):0;
    int x0=view->x0<0?0:view->x0,x1=view->x1>320?320:view->x1;
    int y0=view->y0<0?0:view->y0,y1=view->y1>200?200:view->y1;
    x0=(int)ceil(x0*g->width/320.0-0.5);x1=(int)ceil(x1*g->width/320.0-0.5);
    y0=(int)ceil(y0*g->height/200.0-0.5);y1=(int)ceil(y1*g->height/200.0-0.5);
    glScissor(x0,(int)g->height-y1,x1>x0?x1-x0:0,y1>y0?y1-y0:0);
    glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glDisable(GL_CULL_FACE);
    GLuint p=g->horizon;glUseProgram(p);camera(g,p);
    if(has_inverse)glUniformMatrix3fv(glGetUniformLocation(p,"inverse_basis"),1,GL_TRUE,inv);
    integer(p,"terrain_loaded",g->options && g->options->terrain);
    integer(p,"camera_horizon",g->atmosphere || panorama);
    integer(p,"photo_sky",panorama!=0);integer(p,"sky_image",0);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,panorama);
    if(panorama)glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    color(p,"sky_color",colors[view->sky_color&255]);color(p,"ground_color",colors[view->ground_color&255]);
    const LhxSceneHorizon *h=&view->horizon;
    glUniform4f(glGetUniformLocation(p,"horizon"),h->x,h->y,h->nx,h->ny);
    color(p,"first_color",colors[h->colors[0]&255]);color(p,"second_color",colors[h->colors[1]&255]);
    integer(p,"two_colors",h->two_colors);
    integer(p,"antialiasing",g->options && g->options->antialiasing);
    glDrawArrays(GL_TRIANGLES,0,3);
    terrain(g,daylight);return 1;
}
static int begin_layer(void *context,int layer)
{
    LhxGpu *g=context;flush(g);
    if(layer || g->relief){glEnable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);if(!g->relief)glClear(GL_DEPTH_BUFFER_BIT);}
    else{glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);}
    return 1;
}
static int triangle(void *context,const LhxSceneVertex p[3],const LhxSceneUV *uv,
                      const LhxTexture *tex,uint32_t color_value,float tint,int cull,int stipple,float bias)
{
    LhxGpu *g=context;GpuVertex v[3]={0};GLuint name=tex?texture(g,tex):0;if(tex && !name)return 0;
    for(unsigned i=0;i<3;i++){
        v[i]=(GpuVertex){{p[i].x,p[i].y,p[i].z},{uv?uv[i].u:0,uv?uv[i].v:0},color_value,bias,0,tint,g->shadow?-g->shadow:g->detail};
    }
    return append(g,v,3,0,name,cull,stipple,0);
}
static int draped_triangle(void *context,const LhxSceneVertex p[3],const LhxSceneUV *uv,
                      const LhxTexture *tex,uint32_t color_value,float tint,int cull,int stipple,float bias)
{
    LhxGpu *g=context;GpuVertex v[3]={0};GLuint name=tex?texture(g,tex):0;if(tex && !name)return 0;
    for(unsigned i=0;i<3;i++)v[i]=(GpuVertex){{p[i].x,p[i].y,p[i].z},{uv?uv[i].u:0,uv?uv[i].v:0},color_value,bias,-1,tint,0};
    g->gpu_draped_vertices+=3;return append(g,v,3,0,name,cull,stipple,0);
}
static int line(void *context,LhxSceneVertex a,LhxSceneVertex b,uint32_t color_value,int stipple,float authored_width)
{
    GpuVertex v[2]={{{a.x,a.y,a.z},{0},color_value,0.0005f,0,1,0},
                    {{b.x,b.y,b.z},{0},color_value,0.0005f,0,1,0}};
    return append(context,v,2,1,0,0,stipple,authored_width);
}
static int particle(void *context,LhxSceneVertex p,float radius,uint32_t color_value,int stipple)
{
    GpuVertex v={{p.x,p.y,p.z},{0},color_value,0,radius,1,0};
    return append(context,&v,1,2,0,0,stipple,0);
}
static int end_view(void *context){flush(context);return 1;}
static int material(void *context,int detail,int shadow){
    LhxGpu *g=context;
    if(shadow!=g->shadow){
        flush(g);
        if(shadow){
            if(!g->shadow_started){glStencilMask(0xff);glClear(GL_STENCIL_BUFFER_BIT);g->shadow_started=1;}
            glEnable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);
            if(shadow==1){glEnable(GL_STENCIL_TEST);glStencilMask(0xff);glStencilFunc(GL_NOTEQUAL,1,0xff);glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);}
            else glDisable(GL_STENCIL_TEST);
        }else{glDisable(GL_STENCIL_TEST);glDepthMask(GL_TRUE);}
        g->shadow=shadow;
    }
    g->detail=detail;return 1;
}
static void free_prop(PropBuffer *p)
{
    for(unsigned i=0;i<p->count;i++){
        glDeleteVertexArrays(1,&p->groups[i].array);glDeleteBuffers(1,&p->groups[i].buffer);
    }
    memset(p,0,sizeof(*p));
}
static PropBuffer *cached_prop(LhxGpu *g,const LhxPropModel *source)
{
    if(!source || !source->count || source->count>PROP_GROUPS)return NULL;
    unsigned slot=0;
    for(unsigned i=0;i<PROP_CACHE;i++){
        PropBuffer *p=g->props+i;
        if(p->source==source){slot=i;break;}
        if(!p->source || p->last_used<g->props[slot].last_used)slot=i;
    }
    PropBuffer *p=g->props+slot;
    if(p->source!=source || p->vertices!=source->groups[0].vertices || p->revision!=source->groups[0].texture.revision){
        free_prop(p);p->source=source;p->vertices=source->groups[0].vertices;
        p->revision=source->groups[0].texture.revision;p->count=source->count;
        for(unsigned i=0;i<p->count;i++){
            const LhxPropGroup *group=source->groups+i;PropGroup *b=p->groups+i;
            PropVertex *vertices=calloc(group->count,sizeof(*vertices));if(!vertices){free_prop(p);return NULL;}
            for(unsigned j=0;j<group->count;j+=3){
                LhxSceneVertex a=group->vertices[j].position,c=group->vertices[j+1].position,d=group->vertices[j+2].position;
                double ux=c.x-a.x,uy=c.y-a.y,uz=c.z-a.z,vx=d.x-a.x,vy=d.y-a.y,vz=d.z-a.z;
                double nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx,len=sqrt(nx*nx+ny*ny+nz*nz);
                if(len<1e-30)len=1;
                for(unsigned k=0;k<3;k++){
                    const LhxPropVertex *v=group->vertices+j+k;
                    vertices[j+k]=(PropVertex){{v->position.x,v->position.y,v->position.z},{v->uv.u,v->uv.v},{nx/len,ny/len,nz/len}};
                }
            }
            glGenVertexArrays(1,&b->array);glBindVertexArray(b->array);
            glGenBuffers(1,&b->buffer);glBindBuffer(GL_ARRAY_BUFFER,b->buffer);
            glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)group->count*sizeof(*vertices),vertices,GL_STATIC_DRAW);
            free(vertices);b->count=group->count;g->prop_uploaded_vertices+=group->count;
            const size_t offsets[]={offsetof(PropVertex,position),offsetof(PropVertex,uv),offsetof(PropVertex,normal)};
            for(unsigned j=0;j<3;j++){
                glEnableVertexAttribArray(j);glVertexAttribPointer(j,j==1?2:3,GL_FLOAT,GL_FALSE,sizeof(PropVertex),(void*)offsets[j]);
            }
        }
    }
    p->last_used=g->frame;return p;
}
static int prop(void *context,const LhxPropModel *source,const LhxSceneMesh *mesh,const LhxPropFit *fit,
                    float daylight,int lighting,const double shadow[3][4])
{
    LhxGpu *g=context;flush(g);PropBuffer *buffer=cached_prop(g,source);if(!buffer)return 0;
    /* Fit source art once per instance. Immutable positions/normals stay in VBOs. */
    double fit_matrix[3][3]={{-fit->scale[0],0,0},{0,fit->scale[1],0},{0,0,fit->scale[2]}};
    if(fit->turn){fit_matrix[0][0]=fit_matrix[2][2]=0;fit_matrix[0][2]=fit->scale[0];fit_matrix[2][0]=fit->scale[2];}
    double offset[3],matrix[3][4]={{0}};
    for(unsigned i=0;i<3;i++){
        offset[i]=fit->offset[i];for(unsigned j=0;j<3;j++)offset[i]-=fit_matrix[i][j]*fit->center[j];
    }
    for(unsigned i=0;i<3;i++){
        matrix[i][3]=mesh->model_to_view[i][3];
        for(unsigned j=0;j<3;j++){
            matrix[i][3]+=mesh->model_to_view[i][j]*offset[j];
            for(unsigned k=0;k<3;k++)matrix[i][k]+=mesh->model_to_view[i][j]*fit_matrix[j][k];
        }
    }
    if(shadow){
        double projected[3][4]={{0}};
        for(unsigned i=0;i<3;i++){
            projected[i][3]=shadow[i][3];
            for(unsigned j=0;j<3;j++)for(unsigned k=0;k<4;k++)projected[i][k]+=shadow[i][j]*matrix[j][k];
        }
        memcpy(matrix,projected,sizeof matrix);
    }
    float transform[16]={0},normal[9]={0},basis[9];transform[15]=1;
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<4;j++)transform[i*4+j]=matrix[i][j];
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)basis[i*3+j]=g->view->world_to_view[i][j];
    if(lighting && !shadow){
        double det=matrix[0][0]*(matrix[1][1]*matrix[2][2]-matrix[1][2]*matrix[2][1])-
            matrix[0][1]*(matrix[1][0]*matrix[2][2]-matrix[1][2]*matrix[2][0])+
            matrix[0][2]*(matrix[1][0]*matrix[2][1]-matrix[1][1]*matrix[2][0]);
        if(fabs(det)>1e-30)for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)
            normal[i*3+j]=(matrix[(i+1)%3][(j+1)%3]*matrix[(i+2)%3][(j+2)%3]-matrix[(i+1)%3][(j+2)%3]*matrix[(i+2)%3][(j+1)%3])/det;
        else lighting=0;
    }
    GLuint program=g->prop_program;glUseProgram(program);camera(g,program);
    glUniformMatrix4fv(glGetUniformLocation(program,"instance_view"),1,GL_TRUE,transform);
    glUniformMatrix3fv(glGetUniformLocation(program,"instance_normal"),1,GL_TRUE,normal);
    glUniformMatrix3fv(glGetUniformLocation(program,"world_basis"),1,GL_TRUE,basis);
    scalar(program,"daylight",daylight);integer(program,"light_model",lighting);
    integer(program,"is_shadow",shadow!=NULL);scalar(program,"shadow_opacity",floorf(255*daylight*.5f)/255.f);
    integer(program,"stipple",0);integer(program,"image",0);integer(program,"textured",shadow==NULL);
    glDepthMask(shadow==NULL);g->prop_instances++;
    for(unsigned i=0;i<buffer->count;i++){
        const LhxPropGroup *group=source->groups+i;
        if(!shadow){GLuint name=texture(g,&group->texture);if(!name)return 0;
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,name);}
        if(group->one_sided && !shadow)glEnable(GL_CULL_FACE);else glDisable(GL_CULL_FACE);
        glBindVertexArray(buffer->groups[i].array);
        glDrawArrays(GL_TRIANGLES,0,buffer->groups[i].count);g->prop_drawn_vertices+=buffer->groups[i].count;
    }
    glBindVertexArray(g->vertex_array);glDisable(GL_CULL_FACE);return 1;
}
static void collect_query(LhxGpu *g,unsigned i,int wait)
{
    if(!g->query_pending[i])return;
    GLint available=0;glGetQueryObjectiv(g->queries[i][1],GL_QUERY_RESULT_AVAILABLE,&available);
    if(!available && !wait)return;
    GLuint64 start=0,end=0;
    glGetQueryObjectui64v(g->queries[i][0],GL_QUERY_RESULT,&start);
    glGetQueryObjectui64v(g->queries[i][1],GL_QUERY_RESULT,&end);
    g->query_pending[i]=0;
    if(!start || end<start){g->invalid_timestamps++;return;}
    GLuint64 ns=end-start;
    g->timed_draws++;g->total_ns+=ns;if(ns>g->max_ns)g->max_ns=ns;
}
void lhx_gpu_attributes(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,0);
}
LhxGpu *lhx_gpu_create(SDL_Window *window,unsigned width,unsigned height,int antialiasing,int timings)
{
    if(!window || !width || !height || width>8192 || height>8192 || (size_t)width*height>16777216)return NULL;
    LhxGpu *g=calloc(1,sizeof(*g));if(!g)return NULL;
    g->window=window;g->width=width;g->height=height;g->antialiasing=antialiasing;
    g->timings=timings;
    g->context=SDL_GL_CreateContext(window);if(!g->context)goto fail;
    GLint major=0,minor=0;glGetIntegerv(GL_MAJOR_VERSION,&major);glGetIntegerv(GL_MINOR_VERSION,&minor);
    if(major<3 || (major==3 && minor<3))goto fail;
    snprintf(g->device,sizeof g->device,"%s",glGetString(GL_RENDERER));
    snprintf(g->vendor,sizeof g->vendor,"%s",glGetString(GL_VENDOR));
    snprintf(g->version,sizeof g->version,"%s",glGetString(GL_VERSION));
    g->software=strstr(g->device,"llvmpipe") || strstr(g->device,"softpipe") ||
                strstr(g->device,"Software") || strstr(g->device,"SwiftShader") || strstr(g->device,"swrast");
    fprintf(stderr,"[gpu] OpenGL %s; %s%s\n",g->version,g->device,g->software?" (software device)":"");
    SDL_GL_SetSwapInterval(0); /* Original frame clock owns pacing. */
    g->geometry=program(gpu_vertex_source,gpu_fragment_source,NULL);
    g->particles=program(gpu_particle_vertex,gpu_particle_fragment,gpu_particle_geometry);
    g->horizon=program(gpu_fullscreen_vertex,gpu_horizon_fragment,NULL);
    g->terrain=program(gpu_fullscreen_vertex,gpu_terrain_fragment,NULL);
    g->relief_program=program(gpu_relief_vertex,gpu_fragment_source,NULL);
    g->prop_program=program(gpu_prop_vertex,gpu_fragment_source,NULL);
    g->composite=program(gpu_fullscreen_vertex,gpu_overlay_fragment,NULL);
    if(!g->geometry || !g->particles || !g->horizon || !g->terrain || !g->composite || !g->relief_program || !g->prop_program)goto fail;
    GLint max_samples=1;glGetIntegerv(GL_MAX_SAMPLES,&max_samples);
    g->samples=antialiasing?(max_samples>=4?4:max_samples>=2?2:1):1;
    GLfloat line_range[2]={1,1};glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE,line_range);
    g->max_line_width=fmaxf(1,line_range[1]);
    glGenFramebuffers(1,&g->scene_fbo);glBindFramebuffer(GL_FRAMEBUFFER,g->scene_fbo);
    glGenRenderbuffers(1,&g->scene_color);glBindRenderbuffer(GL_RENDERBUFFER,g->scene_color);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER,g->samples,GL_RGBA8,width,height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,g->scene_color);
    glGenRenderbuffers(1,&g->scene_depth);glBindRenderbuffer(GL_RENDERBUFFER,g->scene_depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER,g->samples,GL_DEPTH32F_STENCIL8,width,height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,g->scene_depth);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)goto fail;
    glGenFramebuffers(1,&g->final_fbo);glBindFramebuffer(GL_FRAMEBUFFER,g->final_fbo);
    glGenTextures(1,&g->final_color);glBindTexture(GL_TEXTURE_2D,g->final_color);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,g->final_color,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)goto fail;
    glGenTextures(1,&g->overlay);glBindTexture(GL_TEXTURE_2D,g->overlay);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,320,200,0,GL_BGRA,GL_UNSIGNED_BYTE,NULL);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glGenVertexArrays(1,&g->vertex_array);glBindVertexArray(g->vertex_array);
    glGenBuffers(1,&g->buffer);glBindBuffer(GL_ARRAY_BUFFER,g->buffer);
    const GLint sizes[]={3,2,4,1,1,1,1};
    const size_t offsets[]={offsetof(GpuVertex,position),offsetof(GpuVertex,uv),offsetof(GpuVertex,color),offsetof(GpuVertex,bias),offsetof(GpuVertex,radius),offsetof(GpuVertex,tint),offsetof(GpuVertex,detail)};
    for(unsigned i=0;i<7;i++){
        glEnableVertexAttribArray(i);glVertexAttribPointer(i,sizes[i],i==2?GL_UNSIGNED_BYTE:GL_FLOAT,i==2, sizeof(GpuVertex),(const void*)offsets[i]);
    }
    if(timings)glGenQueries(QUERIES*2,&g->queries[0][0]);
    glDisable(GL_DITHER);glDisable(GL_FRAMEBUFFER_SRGB);glFrontFace(GL_CW);glCullFace(GL_BACK);
    glDepthFunc(GL_GREATER);glClearDepth(0);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    if(glGetError()!=GL_NO_ERROR)goto fail;
    fprintf(stderr,"[gpu] %ux%u, %u samples; readback only for screenshots\n",width,height,g->samples);
    return g;
fail:
    fprintf(stderr,"[gpu] initialization failed: %s\n",SDL_GetError());lhx_gpu_destroy(g);return NULL;
}
void lhx_gpu_destroy(LhxGpu *g)
{
    if(!g)return;
    if(g->context){
        SDL_GL_MakeCurrent(g->window,g->context);
        glDeleteProgram(g->geometry);glDeleteProgram(g->particles);glDeleteProgram(g->horizon);glDeleteProgram(g->terrain);glDeleteProgram(g->composite);
        glDeleteProgram(g->relief_program);glDeleteProgram(g->prop_program);
        for(unsigned i=0;i<PROP_CACHE;i++)free_prop(g->props+i);
        for(unsigned i=0;i<2;i++){ReliefBuffer *b=g->relief_buffers+i;glDeleteBuffers(1,&b->buffer);glDeleteBuffers(1,&b->index);glDeleteVertexArrays(1,&b->array);glDeleteTextures(1,&b->height);}
        glDeleteFramebuffers(1,&g->scene_fbo);glDeleteFramebuffers(1,&g->final_fbo);
        glDeleteRenderbuffers(1,&g->scene_color);glDeleteRenderbuffers(1,&g->scene_depth);
        glDeleteTextures(1,&g->final_color);glDeleteTextures(1,&g->overlay);
        for(unsigned i=0;i<TEXTURES;i++)glDeleteTextures(1,&g->textures[i].name);
        glDeleteBuffers(1,&g->buffer);glDeleteVertexArrays(1,&g->vertex_array);
        if(g->timings)glDeleteQueries(QUERIES*2,&g->queries[0][0]);
        SDL_GL_DeleteContext(g->context);
    }
    free(g->vertices);free(g->commands);free(g);
}
int lhx_gpu_draw(LhxGpu *g,const LhxSceneFrame *frame,const uint8_t palette[768],
                  const uint32_t overlay[64000],const LhxSceneRenderOptions *options)
{
    if(!g || !palette || !overlay)return -1;
    g->options=options;g->frame++;g->vertex_count=g->command_count=0;
    if(g->timings)for(unsigned i=0;i<QUERIES;i++)collect_query(g,i,0);
    unsigned q=g->query_cursor;int timed=g->timings && !g->query_pending[q];
    if(timed)glQueryCounter(g->queries[q][0],GL_TIMESTAMP);
    glBindFramebuffer(GL_FRAMEBUFFER,g->scene_fbo);glViewport(0,0,g->width,g->height);
    glBindVertexArray(g->vertex_array);glDisable(GL_SCISSOR_TEST);glDepthMask(GL_TRUE);
    glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);glEnable(GL_BLEND);glEnable(GL_MULTISAMPLE);
    static const LhxSceneGeometrySink sink={begin_view,begin_layer,triangle,line,particle,end_view,material,prop,draped_triangle};
    int result=lhx_scene_emit_geometry(frame,palette,options,&sink,g);
    glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,g->scene_fbo);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,g->final_fbo);
    glBlitFramebuffer(0,0,g->width,g->height,0,0,g->width,g->height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER,g->final_fbo);glUseProgram(g->composite);integer(g->composite,"image",0);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,g->overlay);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,320,200,GL_BGRA,GL_UNSIGNED_BYTE,overlay);
    glDrawArrays(GL_TRIANGLES,0,3);
    if(timed){glQueryCounter(g->queries[q][1],GL_TIMESTAMP);g->query_pending[q]=1;g->query_cursor=(q+1)%QUERIES;}
    g->submitted_draws++;
    glFlush();
    GLenum error=glGetError();if(error!=GL_NO_ERROR){fprintf(stderr,"[gpu] draw error 0x%x\n",error);return -1;}
    return result;
}
void lhx_gpu_present(LhxGpu *g)
{
    if(!g)return;
    int width,height;SDL_GL_GetDrawableSize(g->window,&width,&height);
    int w=width,h=(int)((int64_t)width*g->height/g->width);
    if(h>height){h=height;w=(int)((int64_t)height*g->width/g->height);}
    int x=(width-w)/2,y=(height-h)/2;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,g->final_fbo);
    glBlitFramebuffer(0,0,g->width,g->height,x,y,x+w,y+h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    SDL_GL_SwapWindow(g->window);
}
int lhx_gpu_read_pixels(LhxGpu *g,uint32_t *pixels)
{
    if(!g || !pixels)return 0;
    uint32_t *row=malloc(g->width*sizeof(*row));if(!row)return 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,g->final_fbo);glReadPixels(0,0,g->width,g->height,GL_BGRA,GL_UNSIGNED_BYTE,pixels);
    for(unsigned y=0;y<g->height/2;y++){
        uint32_t *a=pixels+(size_t)y*g->width,*b=pixels+(size_t)(g->height-1-y)*g->width;
        memcpy(row,a,g->width*4);memcpy(a,b,g->width*4);memcpy(b,row,g->width*4);
    }
    free(row);g->readbacks++;return glGetError()==GL_NO_ERROR;
}
static void json_string(FILE *f,const char *s)
{
    fputc('"',f);for(;*s;s++){if(*s=='"' || *s=='\\')fputc('\\',f);if((unsigned char)*s>=32)fputc(*s,f);}fputc('"',f);
}
void lhx_gpu_report(LhxGpu *g,FILE *f)
{
    if(!g || !f)return;
    if(g->timings)for(unsigned i=0;i<QUERIES;i++)collect_query(g,i,1);
    fputs("{\"device\":",f);json_string(f,g->device);fputs(",\"vendor\":",f);json_string(f,g->vendor);
    fprintf(f,",\"gpu_draped_vertices\":%llu",(unsigned long long)g->gpu_draped_vertices);
    fprintf(f,",\"cached_prop_instances\":%llu,\"prop_uploaded_vertices\":%llu,\"prop_drawn_vertices\":%llu,\"streamed_vertices\":%llu",
        (unsigned long long)g->prop_instances,(unsigned long long)g->prop_uploaded_vertices,(unsigned long long)g->prop_drawn_vertices,(unsigned long long)g->streamed_vertices);
    fprintf(f,",\"software_device\":%s,\"samples\":%u,\"submitted_draws\":%llu,\"framebuffer_readbacks\":%llu,\"timed_draws\":%llu,\"invalid_timestamps\":%llu,\"device_draw_mean_us\":%.3f,\"device_draw_max_us\":%.3f}",
        g->software?"true":"false",g->samples,(unsigned long long)g->submitted_draws,(unsigned long long)g->readbacks,(unsigned long long)g->timed_draws,(unsigned long long)g->invalid_timestamps,
        g->timed_draws?g->total_ns/(1000.0*g->timed_draws):0,g->max_ns/1000.0);
}
#else
struct LhxGpu{int unused;};
void lhx_gpu_attributes(void){}
LhxGpu *lhx_gpu_create(SDL_Window *w,unsigned width,unsigned height,int aa,int timings){(void)w;(void)width;(void)height;(void)aa;(void)timings;fprintf(stderr,"[gpu] OpenGL support was not built; using CPU\n");return NULL;}
void lhx_gpu_destroy(LhxGpu *g){(void)g;}
int lhx_gpu_draw(LhxGpu *g,const LhxSceneFrame *f,const uint8_t p[768],const uint32_t o[64000],const LhxSceneRenderOptions *s){(void)g;(void)f;(void)p;(void)o;(void)s;return -1;}
void lhx_gpu_present(LhxGpu *g){(void)g;}
int lhx_gpu_read_pixels(LhxGpu *g,uint32_t *p){(void)g;(void)p;return 0;}
void lhx_gpu_report(LhxGpu *g,FILE *f){(void)g;fputs("null",f);}
#endif
