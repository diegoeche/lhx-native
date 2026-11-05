#include "scene_renderer.h"
#include "scene_geometry.h"
#include "scene_atmosphere.h"
#include "scene_props.h"
#include "scene_sky.h"
#include "scene_relief.h"
#include "scene_surface.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define NEAR_Z 2.0
#define Y_SCALE (15.0 / 16.0)
#define LINE_BIAS 0.0005
#define DECAL_BIAS 0.0001
#define MAX_DIMENSION 8192u
#define MAX_PIXELS 16777216u
#define MAX_VIEWS 64u
#define MAX_MESHES 65536u
#define MAX_VERTICES 65536u
#define MAX_PRIMITIVES 4096u
#define MAX_INDICES 1024u

typedef struct { double x, y, z; } Vec3;
typedef struct { double x, y; } Vec2;
typedef struct { double x, y, q, u, v; } Screen;
typedef struct { Vec3 p; double u, v; } TextureVertex;
typedef struct {
    Vec3 normal, center;
    double plane, area;
    int group;
    unsigned rank;
} Face;
typedef struct { unsigned root; } PlaneGroup;
typedef struct {
    float *depth;
    size_t depth_capacity;
    Face *faces;
    PlaneGroup *groups;
    size_t face_capacity;
    int *vertex_groups;
    size_t vertex_capacity;
    LhxSceneVertex *placed;
    unsigned short *rotor_counts;
    unsigned char *shadow_mask;size_t shadow_capacity;
    TextureVertex *contacts;size_t contact_capacity;
    Vec3 *polygon;
    Vec2 *flat;
    LhxSceneUV *uvs;
    unsigned *ring, *triangles;
    size_t polygon_capacity;
    uint32_t *antialias_pixels;
    size_t antialias_capacity;
} Scratch;
typedef struct {
    uint32_t *pixels;
    unsigned width, height;
    int x0, y0, x1, y1;
    double sx, sy, cx, cy, focus;
    /* Camera-space half planes: near, left, right, bottom, top. */
    Vec3 planes[5];
    uint32_t colors[256];
    int depth_enabled;
    const LhxSceneView *view;
    const LhxSceneRenderOptions *options;
    const LhxTexture *texture;
    double tint;
    double daylight;
    const LhxSceneGeometrySink *sink;
    void *sink_context;
    int emit_failed;
    int atmosphere;
    uint32_t haze;
    double inverse_basis[3][3];
    const LhxTexture *panorama;
    int relief,drape;
    double base_height,shadow_ground,shadow_matrix[3][4];int shadow_flat;
    unsigned subdivisions;
    int detail,detail_kind,shadow,rotors;uint32_t material_color;
    double local_inverse[3][3],local_offset[3];
} Render;

static int inverse(const double m[3][3],double out[3][3]);
static Scratch scratch;
static LhxTexture runway_texture;
static struct {LhxTexture texture;int length,width,bearing;} runway_details[8];
static unsigned runway_cursor;

static Vec3 vertex(const LhxSceneVertex *v)
{
    return (Vec3){v->x, v->y, v->z};
}

static double dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static int finite_vertex(const LhxSceneVertex *v)
{
    return isfinite(v->x) && isfinite(v->y) && isfinite(v->z);
}

void lhx_scene_renderer_reset(void)
{
    lhx_texture_free(&runway_texture);
    for(unsigned i=0;i<8;i++)lhx_texture_free(&runway_details[i].texture);
    memset(runway_details,0,sizeof runway_details);runway_cursor=0;
    free(scratch.depth);
    free(scratch.faces);
    free(scratch.groups);
    free(scratch.vertex_groups);
    free(scratch.placed);
    free(scratch.rotor_counts);
    free(scratch.shadow_mask);
    free(scratch.contacts);
    free(scratch.polygon);
    free(scratch.flat);
    free(scratch.uvs);
    free(scratch.ring);
    free(scratch.triangles);
    free(scratch.antialias_pixels);
    memset(&scratch, 0, sizeof scratch);
}

static int reserve_depth(size_t count)
{
    if (count <= scratch.depth_capacity)
        return 1;
    float *p = realloc(scratch.depth, count * sizeof *p);
    if (!p)
        return 0;
    scratch.depth = p;
    scratch.depth_capacity = count;
    return 1;
}

static int reserve_mesh(unsigned faces, unsigned vertices, unsigned polygon)
{
    if (faces > scratch.face_capacity) {
        /* Allocate pairs together so a failed reserve leaves usable storage. */
        Face *f = malloc((size_t)faces * sizeof *f);
        PlaneGroup *g = malloc((size_t)faces * sizeof *g);
        if (!f || !g) {
            free(f);
            free(g);
            return 0;
        }
        free(scratch.faces);
        free(scratch.groups);
        scratch.faces = f;
        scratch.groups = g;
        scratch.face_capacity = faces;
    }
    if (vertices > scratch.vertex_capacity) {
        unsigned short *rotors=realloc(scratch.rotor_counts,(size_t)vertices*sizeof(*rotors));
        if(!rotors)return 0;
        scratch.rotor_counts=rotors;
        LhxSceneVertex *placed=realloc(scratch.placed,(size_t)vertices*sizeof(*placed));
        if(!placed)return 0;
        scratch.placed=placed;
        int *p = realloc(scratch.vertex_groups, (size_t)vertices * sizeof *p);
        if (!p)
            return 0;
        scratch.vertex_groups = p;
        scratch.vertex_capacity = vertices;
    }
    if (polygon > scratch.polygon_capacity) {
        Vec3 *p = malloc((size_t)polygon * sizeof *p);
        Vec2 *f = malloc((size_t)polygon * sizeof *f);
        LhxSceneUV *uvs=malloc((size_t)polygon*sizeof(*uvs));
        unsigned *r = malloc((size_t)polygon * sizeof *r);
        unsigned *t = malloc((size_t)polygon * 3 * sizeof *t);
        if (!p || !f || !r || !t || !uvs) {
            free(p);
            free(f);
            free(r);
            free(t);
            free(uvs);
            return 0;
        }
        free(scratch.polygon);
        free(scratch.flat);
        free(scratch.uvs);
        free(scratch.ring);
        free(scratch.triangles);
        scratch.polygon = p;
        scratch.flat = f;
        scratch.uvs=uvs;
        scratch.ring = r;
        scratch.triangles = t;
        scratch.polygon_capacity = polygon;
    }
    return 1;
}

static int validate_frame(const LhxSceneFrame *frame, unsigned *max_faces,
                          unsigned *max_vertices, unsigned *max_polygon)
{
    if (!frame || !frame->view_count)
        return 0;
    if (!frame->views || frame->view_count > MAX_VIEWS)
        return -1;
    for (unsigned v = 0; v < frame->view_count; v++) {
        const LhxSceneView *view = &frame->views[v];
        if (view->mesh_count > MAX_MESHES || (view->mesh_count && !view->meshes))
            return -1;
        if (!isfinite(view->focus) || !isfinite(view->horizon.x) ||
            !isfinite(view->horizon.y) || !isfinite(view->horizon.nx) ||
            !isfinite(view->horizon.ny))
            return -1;
        for (unsigned m = 0; m < view->mesh_count; m++) {
            const LhxSceneMesh *mesh = &view->meshes[m];
            if(!memchr(mesh->model_name,0,sizeof mesh->model_name))return -1;
            if(mesh->has_placement)for(unsigned i=0;i<3;i++){
                if(!isfinite(mesh->world_origin[i]) || !isfinite(mesh->model_bounds[2*i]) ||
                   !isfinite(mesh->model_bounds[2*i+1]) || mesh->model_bounds[2*i]>mesh->model_bounds[2*i+1])return -1;
                for(unsigned j=0;j<4;j++)if(!isfinite(mesh->model_to_view[i][j]))return -1;
            }
            if (mesh->vertex_count > MAX_VERTICES ||
                mesh->primitive_count > MAX_PRIMITIVES ||
                (mesh->vertex_count && !mesh->vertices) ||
                (mesh->primitive_count && !mesh->primitives) ||
                mesh->layer < 0 || mesh->layer > 2)
                return -1;
            if (mesh->primitive_count > *max_faces)
                *max_faces = mesh->primitive_count;
            if (mesh->vertex_count > *max_vertices)
                *max_vertices = mesh->vertex_count;
            for (unsigned i = 0; i < mesh->vertex_count; i++)
                if (!finite_vertex(&mesh->vertices[i]))
                    return -1;
            if (mesh->texture) {
                if (!mesh->uvs || !mesh->texture->count || mesh->texture->count>13)
                    return -1;
                for (unsigned l=0;l<mesh->texture->count;l++) {
                    const LhxTextureLevel *level=&mesh->texture->levels[l];
                    if (!level->pixels || !level->width || !level->height ||
                        level->width>2048 || level->height>2048) return -1;
                }
                for (unsigned i=0;i<mesh->vertex_count;i++)
                    if (!isfinite(mesh->uvs[i].u) || !isfinite(mesh->uvs[i].v)) return -1;
            }
            for (unsigned i = 0; i < mesh->primitive_count; i++) {
                const LhxScenePrimitive *p = &mesh->primitives[i];
                if (p->count > MAX_INDICES || (p->count && !p->indices) ||
                    !isfinite(p->radius))
                    return -1;
                if (p->count > *max_polygon)
                    *max_polygon = p->count;
                for (unsigned j = 0; j < p->count; j++)
                    if (p->indices[j] >= mesh->vertex_count)
                        return -1;
            }
        }
    }
    return 1;
}

static Screen project(const Render *r, Vec3 v)
{
    double q = 1.0 / v.z;
    return (Screen){.x=r->sx * (r->cx + r->focus * v.x * q),
                    .y=r->sy * (r->cy - Y_SCALE * r->focus * v.y * q), .q=q};
}

static uint32_t tint_color(uint32_t color,double tint)
{
    uint32_t out=0xff000000u;
    for (unsigned shift=0;shift<=16;shift+=8)
        out|=(uint32_t)fmin(255,((color>>shift)&255)*tint+0.5)<<shift;
    return out;
}

static uint32_t texture_tint(uint32_t color,double tint)
{
    if (tint>=1) return tint==1?color:tint_color(color,tint);
    unsigned scale=(unsigned)(tint*256+0.5);
    return 0xff000000u|
        (((((color>>16)&255)*scale+128)>>8)<<16)|
        (((((color>>8)&255)*scale+128)>>8)<<8)|
        (((color&255)*scale+128)>>8);
}

static double plane_distance(const Render *r, unsigned plane, Vec3 v)
{
    return dot(r->planes[plane], v) - (plane == 0 ? NEAR_Z : 0.0);
}

static Vec3 between(Vec3 a, Vec3 b, double t)
{
    return (Vec3){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t};
}

static int clip_line(const Render *r, Vec3 *a, Vec3 *b)
{
    for (unsigned p = 0; p < 5; p++) {
        double da = plane_distance(r, p, *a);
        double db = plane_distance(r, p, *b);
        if (da < 0.0 && db < 0.0)
            return 0;
        if ((da < 0.0) != (db < 0.0)) {
            Vec3 v = between(*a, *b, da / (da - db));
            if (da < 0.0)
                *a = v;
            else
                *b = v;
        }
    }
    return 1;
}

static void put_pixel(Render *r, int x, int y, double q, uint32_t color,
                       int stipple, double bias)
{
    if (stipple && ((x ^ y) & 1))
        return;
    size_t index = (size_t)y * r->width + (unsigned)x;
    float depth = (float)(q * (1.0 + bias));
    if (!r->depth_enabled || depth > scratch.depth[index]) {
        r->pixels[index] = r->atmosphere?
            lhx_atmosphere_mix(color,r->haze,lhx_haze_visibility(1/q)):color;
        if (r->depth_enabled)
            scratch.depth[index] = depth;
    }
}

static uint32_t mix_color(uint32_t a,uint32_t b,unsigned weight)
{
    unsigned other=256-weight;
    uint32_t rb=(((a&0xff00ff)*other+(b&0xff00ff)*weight+0x800080)>>8)&0xff00ff;
    uint32_t g=(((a&0xff00)*other+(b&0xff00)*weight+0x8000)>>8)&0xff00;
    return 0xff000000u|rb|g;
}

static void covered_pixel(Render *r,int x,int y,double q,uint32_t color,
                           int stipple,double coverage)
{
    if (coverage<=0 || (stipple && ((x^y)&1))) return;
    size_t index=(size_t)y*r->width+(unsigned)x;
    float depth=(float)(q*(1.0+LINE_BIAS));
    if (r->depth_enabled && depth<=scratch.depth[index]) return;
    unsigned weight=(unsigned)(fmin(1,coverage)*256+0.5);
    if (!weight) return;
    if(r->atmosphere)color=lhx_atmosphere_mix(color,r->haze,lhx_haze_visibility(1/q));
    r->pixels[index]=mix_color(r->pixels[index],color,weight);
    if (r->depth_enabled) scratch.depth[index]=depth;
}

static double edge(Screen a, Screen b, double x, double y)
{
    return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}

static int top_left(Screen a, Screen b)
{
    return b.y < a.y || (b.y == a.y && b.x > a.x);
}

static int lower_pixel(double v, int low, int high)
{
    if (v <= (double)low + 0.5)
        return low;
    if (v >= (double)high + 0.5)
        return high;
    return (int)ceil(v - 0.5);
}

static void raster_triangle(Render *r, Screen a, Screen b, Screen c,
                             uint32_t color, int one_sided, int stipple,
                             double bias)
{
    double area = edge(a, b, c.x, c.y);
    /* The DOS model format's outward faces are clockwise on a y-down screen. */
    if (fabs(area) <= 1e-12 || (one_sided && area <= 0.0))
        return;
    if (area < 0.0) {
        Screen tmp = b;
        b = c;
        c = tmp;
        area = -area;
    }
    int x0 = lower_pixel(fmin(a.x, fmin(b.x, c.x)), r->x0, r->x1);
    int y0 = lower_pixel(fmin(a.y, fmin(b.y, c.y)), r->y0, r->y1);
    int x1 = lower_pixel(fmax(a.x, fmax(b.x, c.x)), r->x0, r->x1);
    int y1 = lower_pixel(fmax(a.y, fmax(b.y, c.y)), r->y0, r->y1);
    if (x0 >= x1 || y0 >= y1)
        return;
    double dx0 = b.y - c.y, dx1 = c.y - a.y, dx2 = a.y - b.y;
    double dy0 = c.x - b.x, dy1 = a.x - c.x, dy2 = b.x - a.x;
    double e0 = edge(b, c, x0 + 0.5, y0 + 0.5);
    double e1 = edge(c, a, x0 + 0.5, y0 + 0.5);
    double e2 = edge(a, b, x0 + 0.5, y0 + 0.5);
    double zscale = (1.0 + bias) / area;
    double q0 = a.q * zscale, q1 = b.q * zscale, q2 = c.q * zscale;
    double qrow = e0 * q0 + e1 * q1 + e2 * q2;
    double qdx = dx0 * q0 + dx1 * q1 + dx2 * q2;
    double qdy = dy0 * q0 + dy1 * q1 + dy2 * q2;
    double u0=a.u*q0,u1=b.u*q1,u2=c.u*q2;
    double v0=a.v*q0,v1=b.v*q1,v2=c.v*q2;
    double urow=e0*u0+e1*u1+e2*u2,vrow=e0*v0+e1*v1+e2*v2;
    double udx=dx0*u0+dx1*u1+dx2*u2,vdx=dx0*v0+dx1*v1+dx2*v2;
    double udy=dy0*u0+dy1*u1+dy2*u2,vdy=dy0*v0+dy1*v1+dy2*v2;
    int tl0 = top_left(b, c), tl1 = top_left(c, a), tl2 = top_left(a, b);
    double eps = area * 1e-12;
    for (int y = y0; y < y1; y++) {
        double w0 = e0, w1 = e1, w2 = e2, q = qrow;
        double u=urow,v=vrow;
        size_t index = (size_t)y * r->width + (unsigned)x0;
        for (int x = x0; x < x1; x++, index++) {
            if ((w0 > eps || (w0 >= -eps && tl0)) &&
                (w1 > eps || (w1 >= -eps && tl1)) &&
                (w2 > eps || (w2 >= -eps && tl2)) &&
                (stipple!=1 || !((x ^ y) & 1))) {
                float depth = (float)q;
                if (!r->depth_enabled || depth > scratch.depth[index]) {
                    uint32_t previous=r->pixels[index];
                    if (r->texture) {
                        double inv=1/q,tu=u*inv,tv=v*inv;
                        const LhxTextureLevel *base=&r->texture->levels[0];
                        double dudx=(udx-tu*qdx)*inv*base->width;
                        double dvdx=(vdx-tv*qdx)*inv*base->height;
                        double dudy=(udy-tu*qdy)*inv*base->width;
                        double dvdy=(vdy-tv*qdy)*inv*base->height;
                        double fx=dudx*dudx+dvdx*dvdx,fy=dudy*dudy+dvdy*dvdy;
                        double footprint=sqrt(fx>fy?fx:fy);
                        r->pixels[index]=texture_tint(lhx_texture_sample(r->texture,tu,tv,footprint),r->tint);
                    } else if(r->detail){
                        double inv=1/q,tu=u*inv,tv=v*inv;
                        double du=(fabs(udx-tu*qdx)+fabs(udy-tu*qdy))*inv;
                        double dv=(fabs(vdx-tv*qdx)+fabs(vdy-tv*qdy))*inv;
                        r->pixels[index]=tint_color(r->material_color,lhx_surface_detail(r->detail,tu,tv,du,dv));
                    } else r->pixels[index] = color;
                    if(r->atmosphere)r->pixels[index]=lhx_atmosphere_mix(
                        r->pixels[index],r->haze,lhx_haze_visibility(1/q));
                    if(r->shadow){
                        double alpha=r->daylight*(r->shadow==1?.5:.55);
                        if(r->shadow==2){double du=2*u/q-1,dv=2*v/q-1,d2=du*du+dv*dv;
                            alpha*=fmax(0,exp(-d2*4)-exp(-4))/(1-exp(-4));}
                        if(r->shadow==1 && scratch.shadow_mask[index])alpha=0;
                        r->pixels[index]=lhx_atmosphere_mix(r->pixels[index],previous,alpha);
                        if(r->shadow==1 && alpha>0)scratch.shadow_mask[index]=1;
                    }else if(stipple>=2){
                        r->pixels[index]=lhx_atmosphere_mix(r->pixels[index],previous,stipple==2?.2:.7);
                    }else if (r->depth_enabled)
                        scratch.depth[index] = depth;
                }
            }
            w0 += dx0;
            w1 += dx1;
            w2 += dx2;
            q += qdx;
            u+=udx;v+=vdx;
        }
        e0 += dy0;
        e1 += dy1;
        e2 += dy2;
        qrow += qdy;
        urow+=udy;vrow+=vdy;
    }
}

static void draw_textured_triangle(Render *r,TextureVertex a,TextureVertex b,
                                    TextureVertex c,int one_sided,int stipple,double bias)
{
    if (r->sink) {
        LhxSceneVertex p[3]={{a.p.x,a.p.y,a.p.z},{b.p.x,b.p.y,b.p.z},{c.p.x,c.p.y,c.p.z}};
        LhxSceneUV uv[3]={{a.u,a.v},{b.u,b.v},{c.u,c.v}};
        if (!r->sink->triangle(r->sink_context,p,uv,r->texture,r->detail?r->material_color:0xffffffffu,
                              r->tint,one_sided,stipple,bias)) r->emit_failed=1;
        return;
    }
    TextureVertex poly[2][12];
    poly[0][0]=a;poly[0][1]=b;poly[0][2]=c;
    unsigned count=3,src=0;
    for (unsigned plane=0;plane<5 && count;plane++) {
        unsigned out=0;TextureVertex last=poly[src][count-1];
        double dl=plane_distance(r,plane,last.p);
        for (unsigned i=0;i<count;i++) {
            TextureVertex next=poly[src][i];double dn=plane_distance(r,plane,next.p);
            if ((dl<0)!=(dn<0)) {
                double t=dl/(dl-dn);
                poly[!src][out++]=(TextureVertex){between(last.p,next.p,t),
                    last.u+(next.u-last.u)*t,last.v+(next.v-last.v)*t};
            }
            if (dn>=0) poly[!src][out++]=next;
            last=next;dl=dn;
        }
        count=out;src=!src;
    }
    if (count<3) return;
    Screen points[12];
    for (unsigned i=0;i<count;i++) {
        points[i]=project(r,poly[src][i].p);
        points[i].u=poly[src][i].u;points[i].v=poly[src][i].v;
    }
    for (unsigned i=2;i<count;i++)
        raster_triangle(r,points[0],points[i-1],points[i],0,one_sided,stipple,bias);
}

static void draw_triangle(Render *r, Vec3 a, Vec3 b, Vec3 c,
                           uint32_t color, int one_sided, int stipple,
                           double bias)
{
    if (r->sink) {
        LhxSceneVertex p[3]={{a.x,a.y,a.z},{b.x,b.y,b.z},{c.x,c.y,c.z}};
        if (!r->sink->triangle(r->sink_context,p,NULL,NULL,color,1,
                              one_sided,stipple,bias)) r->emit_failed=1;
        return;
    }
    /* Clipping a triangle against five planes needs at most eight vertices. */
    Vec3 polygons[2][12];
    polygons[0][0] = a;
    polygons[0][1] = b;
    polygons[0][2] = c;
    unsigned count = 3, src = 0;
    for (unsigned plane = 0; plane < 5 && count; plane++) {
        unsigned out = 0;
        Vec3 last = polygons[src][count - 1];
        double dl = plane_distance(r, plane, last);
        for (unsigned i = 0; i < count; i++) {
            Vec3 next = polygons[src][i];
            double dn = plane_distance(r, plane, next);
            if ((dl < 0.0) != (dn < 0.0))
                polygons[!src][out++] = between(last, next, dl / (dl - dn));
            if (dn >= 0.0)
                polygons[!src][out++] = next;
            last = next;
            dl = dn;
        }
        count = out;
        src = !src;
    }
    if (count < 3)
        return;
    Screen first = project(r, polygons[src][0]);
    Screen prev = project(r, polygons[src][1]);
    for (unsigned i = 2; i < count; i++) {
        Screen next = project(r, polygons[src][i]);
        raster_triangle(r, first, prev, next, color, one_sided, stipple, bias);
        prev = next;
    }
}

static double cross2(Vec2 a, Vec2 b, Vec2 c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static int in_triangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c, double sign, double eps)
{
    return sign * cross2(a, b, p) >= -eps &&
           sign * cross2(b, c, p) >= -eps &&
           sign * cross2(c, a, p) >= -eps;
}

/* Triangulate in the polygon's dominant plane before near clipping. This keeps
 * a concave contour's disconnected visible pieces separate when it crosses Z=2.
 */
static int triangulate(unsigned count, Vec3 normal, unsigned *triangle_count)
{
    unsigned axis = fabs(normal.x) > fabs(normal.y) ? 0 : 1;
    if (fabs(normal.z) > (axis == 0 ? fabs(normal.x) : fabs(normal.y)))
        axis = 2;
    double minx = DBL_MAX, maxx = -DBL_MAX, miny = DBL_MAX, maxy = -DBL_MAX;
    for (unsigned i = 0; i < count; i++) {
        Vec3 p = scratch.polygon[i];
        Vec2 q = axis == 0 ? (Vec2){p.y, p.z} :
                 axis == 1 ? (Vec2){p.x, p.z} : (Vec2){p.x, p.y};
        scratch.flat[i] = q;
        minx = fmin(minx, q.x);
        maxx = fmax(maxx, q.x);
        miny = fmin(miny, q.y);
        maxy = fmax(maxy, q.y);
        scratch.ring[i] = i;
    }
    double area = 0.0;
    for (unsigned i = 1; i + 1 < count; i++)
        area += cross2(scratch.flat[0], scratch.flat[i], scratch.flat[i + 1]);
    double span = fmax(maxx - minx, maxy - miny);
    double eps = fmax(1e-18, span * span * 1e-12);
    *triangle_count = 0;
    if (fabs(area) <= eps)
        return 1;
    double sign = area > 0.0 ? 1.0 : -1.0;
    int convex = 1;
    for (unsigned i = 0; i < count; i++)
        if (sign * cross2(scratch.flat[(i + count - 1) % count],
                           scratch.flat[i], scratch.flat[(i + 1) % count]) < -eps)
            convex = 0;
    if (convex) {
        for (unsigned i = 1; i + 1 < count; i++) {
            scratch.triangles[(*triangle_count)++] = 0;
            scratch.triangles[(*triangle_count)++] = i;
            scratch.triangles[(*triangle_count)++] = i + 1;
        }
        return 1;
    }
    unsigned remaining = count;
    while (remaining > 3) {
        int found = 0;
        for (unsigned i = 0; i < remaining; i++) {
            unsigned prev = scratch.ring[(i + remaining - 1) % remaining];
            unsigned curr = scratch.ring[i];
            unsigned next = scratch.ring[(i + 1) % remaining];
            double turn = sign * cross2(scratch.flat[prev], scratch.flat[curr],
                                        scratch.flat[next]);
            if (turn < -eps)
                continue;
            int occupied = 0;
            if (turn > eps) {
                for (unsigned j = 0; j < remaining; j++) {
                    unsigned test = scratch.ring[j];
                    if (test != prev && test != curr && test != next &&
                        in_triangle(scratch.flat[test], scratch.flat[prev],
                                     scratch.flat[curr], scratch.flat[next], sign, eps)) {
                        occupied = 1;
                        break;
                    }
                }
            }
            if (occupied)
                continue;
            if (turn > eps) {
                scratch.triangles[(*triangle_count)++] = prev;
                scratch.triangles[(*triangle_count)++] = curr;
                scratch.triangles[(*triangle_count)++] = next;
            }
            memmove(scratch.ring + i, scratch.ring + i + 1,
                    (remaining - i - 1) * sizeof *scratch.ring);
            remaining--;
            found = 1;
            break;
        }
        if (!found)
            return 0; /* Not a simple contour, or degenerate beyond tolerance. */
    }
    if (remaining == 3) {
        for (unsigned i = 0; i < 3; i++)
            scratch.triangles[(*triangle_count)++] = scratch.ring[i];
    }
    return 1;
}

static void prepare_faces(const LhxSceneMesh *mesh)
{
    unsigned group_count = 0;
    Vec3 min = {DBL_MAX, DBL_MAX, DBL_MAX}, max = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
    for (unsigned i = 0; i < mesh->vertex_count; i++) {
        Vec3 p = vertex(&mesh->vertices[i]);
        min.x = fmin(min.x, p.x);
        min.y = fmin(min.y, p.y);
        min.z = fmin(min.z, p.z);
        max.x = fmax(max.x, p.x);
        max.y = fmax(max.y, p.y);
        max.z = fmax(max.z, p.z);
        scratch.vertex_groups[i] = -1;
    }
    double extent = mesh->vertex_count ? fmax(max.x - min.x,
                     fmax(max.y - min.y, max.z - min.z)) : 0.0;
    double tolerance = 2.0 + 0.002 * extent;
    for (unsigned i = 0; i < mesh->primitive_count; i++) {
        Face *face = &scratch.faces[i];
        memset(face, 0, sizeof *face);
        face->group = -1;
        const LhxScenePrimitive *p = &mesh->primitives[i];
        if (p->kind != 0 || p->count < 3)
            continue;
        /* Sum crosses around a translated origin to avoid cancellation when a
         * tiny distant object has large absolute camera-space coordinates. */
        Vec3 base = vertex(&mesh->vertices[p->indices[0]]);
        for (unsigned j = 0; j < p->count; j++) {
            Vec3 a = vertex(&mesh->vertices[p->indices[j]]);
            Vec3 b = vertex(&mesh->vertices[p->indices[(j + 1) % p->count]]);
            face->center.x += a.x;
            face->center.y += a.y;
            face->center.z += a.z;
            a.x -= base.x;
            a.y -= base.y;
            a.z -= base.z;
            b.x -= base.x;
            b.y -= base.y;
            b.z -= base.z;
            face->normal.x += a.y * b.z - a.z * b.y;
            face->normal.y += a.z * b.x - a.x * b.z;
            face->normal.z += a.x * b.y - a.y * b.x;
        }
        double length = sqrt(dot(face->normal, face->normal));
        if (length <= 1e-18)
            continue;
        face->normal.x /= length;
        face->normal.y /= length;
        face->normal.z /= length;
        face->center.x /= p->count;
        face->center.y /= p->count;
        face->center.z /= p->count;
        face->area = length * 0.5;
        face->plane = dot(face->normal, base);
        for (unsigned g = 0; g < group_count; g++) {
            const Face *root = &scratch.faces[scratch.groups[g].root];
            if (dot(face->normal, root->normal) > 0.995 &&
                fabs(dot(root->normal, face->center) - root->plane) <= tolerance &&
                fabs(dot(face->normal, root->center) - face->plane) <= tolerance) {
                face->group = (int)g;
                if (face->area > root->area)
                    scratch.groups[g].root = i;
                break;
            }
        }
        if (face->group < 0) {
            face->group = (int)group_count;
            scratch.groups[group_count++].root = i;
        }
    }
    for (unsigned i = 0; i < mesh->primitive_count; i++) {
        Face *face = &scratch.faces[i];
        if (face->group < 0)
            continue;
        const LhxScenePrimitive *p = &mesh->primitives[i];
        for (unsigned j = 0; j < p->count; j++) {
            int *group = &scratch.vertex_groups[p->indices[j]];
            if (*group == -1)
                *group = face->group;
            else if (*group != face->group)
                *group = -2; /* Shared corners of distinct planes stay fixed. */
        }
        for (unsigned j = 0; j < mesh->primitive_count; j++) {
            const Face *other = &scratch.faces[j];
            if (other->group == face->group && other->area > face->area * 1.000001)
                face->rank++;
        }
    }
}

static LhxSceneVertex world_view(const Render *r,double x,double y,double z)
{
    double p[3]={x-r->view->camera[0],y-r->view->camera[1]-r->base_height,z-r->view->camera[2]},out[3];
    for(unsigned i=0;i<3;i++)out[i]=r->view->world_to_view[i][0]*p[0]+
        r->view->world_to_view[i][1]*p[1]+r->view->world_to_view[i][2]*p[2];
    return (LhxSceneVertex){out[0],out[1],out[2]};
}
static Vec3 draped(const Render *r,Vec3 q)
{
    double world[3];const double *camera=r->view->camera;
    for(unsigned i=0;i<3;i++)world[i]=camera[i]+r->inverse_basis[i][0]*q.x+
        r->inverse_basis[i][1]*q.y+r->inverse_basis[i][2]*q.z;
    const LhxTerrain *terrain=r->relief?r->options->terrain:NULL;
    double height=lhx_relief_height(terrain,world[0],world[2]);
    LhxSceneVertex p=world_view(r,world[0],height,world[2]);
    return (Vec3){p.x*.998,p.y*.998,p.z*.998};
}
static Vec3 shadow_point(const Render *r,Vec3 p)
{
    if(r->shadow_flat){
        const double (*m)[4]=r->shadow_matrix;
        return (Vec3){m[0][0]*p.x+m[0][1]*p.y+m[0][2]*p.z+m[0][3],
                      m[1][0]*p.x+m[1][1]*p.y+m[1][2]*p.z+m[1][3],
                      m[2][0]*p.x+m[2][1]*p.y+m[2][2]*p.z+m[2][3]};
    }
    const LhxTerrain *terrain=r->relief?r->options->terrain:NULL;
    double world[3];
    for(unsigned i=0;i<3;i++)world[i]=r->view->camera[i]+r->inverse_basis[i][0]*p.x+
        r->inverse_basis[i][1]*p.y+r->inverse_basis[i][2]*p.z;
    world[1]+=r->base_height;
    double t=(world[1]-r->shadow_ground)/1.1,x=world[0]+t,z=world[2]-t;
    LhxSceneVertex q=world_view(r,x,lhx_relief_height(terrain,x,z),z);
    return (Vec3){q.x*.988,q.y*.988,q.z*.988};
}
static void shadow_plane(Render *r)
{
    /* Reference replacement shadows use one affine sun projection at the foot.
     * Compose it once per instance, rather than resampling terrain per face. */
    const double (*v)[3]=r->view->world_to_view,(*w)[3]=r->inverse_basis;
    double gy=r->shadow_ground-r->view->camera[1]-r->base_height,t=-gy/1.1;
    for(unsigned i=0;i<3;i++){
        for(unsigned j=0;j<3;j++)r->shadow_matrix[i][j]=.988*(v[i][0]*(w[0][j]+w[1][j]/1.1)+v[i][2]*(w[2][j]-w[1][j]/1.1));
        r->shadow_matrix[i][3]=.988*(v[i][0]*t+v[i][1]*gy-v[i][2]*t);
    }
}
static void ground_triangle(Render *r,TextureVertex a,TextureVertex b,TextureVertex c,
                              uint32_t color,int one_sided,int stipple,double bias,unsigned depth)
{
    if(r->drape){
        Vec3 edges[3]={{b.p.x-a.p.x,b.p.y-a.p.y,b.p.z-a.p.z},
                       {c.p.x-b.p.x,c.p.y-b.p.y,c.p.z-b.p.z},
                       {a.p.x-c.p.x,a.p.y-c.p.y,a.p.z-c.p.z}};
        double lengths[3]={dot(edges[0],edges[0]),dot(edges[1],edges[1]),dot(edges[2],edges[2])};
        unsigned edge=lengths[1]>lengths[0]?1:0;if(lengths[2]>lengths[edge])edge=2;
        if(lengths[edge]>400.0*400 && depth<20 && r->subdivisions++<20000){
            TextureVertex v[3]={a,b,c};unsigned j=(edge+1)%3,k=(edge+2)%3;
            TextureVertex mid={between(v[edge].p,v[j].p,.5),(v[edge].u+v[j].u)*.5,(v[edge].v+v[j].v)*.5};
            ground_triangle(r,v[edge],mid,v[k],color,one_sided,stipple,bias,depth+1);
            ground_triangle(r,mid,v[j],v[k],color,one_sided,stipple,bias,depth+1);return;
        }
        if(r->sink && r->sink->draped_triangle){
            LhxSceneVertex p[3]={{a.p.x,a.p.y,a.p.z},{b.p.x,b.p.y,b.p.z},{c.p.x,c.p.y,c.p.z}};
            LhxSceneUV uv[3]={{a.u,a.v},{b.u,b.v},{c.u,c.v}};
            if(!r->sink->draped_triangle(r->sink_context,p,uv,r->texture,
                    r->texture?0xffffffffu:color,r->texture?r->tint:1,one_sided,stipple,bias))r->emit_failed=1;
            return;
        }
        a.p=draped(r,a.p);b.p=draped(r,b.p);c.p=draped(r,c.p);
    }
    if(r->shadow==1){a.p=shadow_point(r,a.p);b.p=shadow_point(r,b.p);c.p=shadow_point(r,c.p);}
    if(r->shadow){
        if(r->sink){
            LhxSceneVertex v[3]={{a.p.x,a.p.y,a.p.z},{b.p.x,b.p.y,b.p.z},{c.p.x,c.p.y,c.p.z}};
            LhxSceneUV uv[3]={{a.u,a.v},{b.u,b.v},{c.u,c.v}};
            uint32_t alpha=(uint32_t)(255*r->daylight*(r->shadow==1?.5:.55));
            if(!r->sink->triangle(r->sink_context,v,uv,NULL,alpha<<24,1,0,0,0))r->emit_failed=1;
        }else{
            /* Reuse perspective-correct UV clipping for the soft contact patch. */
            r->material_color=0xff000000u;r->detail=0;
            TextureVertex poly[2][12]={{a,b,c}};unsigned count=3,src=0;
            for(unsigned plane=0;plane<5 && count;plane++){
                unsigned out=0;TextureVertex last=poly[src][count-1];double dl=plane_distance(r,plane,last.p);
                for(unsigned i=0;i<count;i++){
                    TextureVertex next=poly[src][i];double dn=plane_distance(r,plane,next.p);
                    if((dl<0)!=(dn<0)){double t=dl/(dl-dn);poly[!src][out++]=(TextureVertex){between(last.p,next.p,t),last.u+(next.u-last.u)*t,last.v+(next.v-last.v)*t};}
                    if(dn>=0)poly[!src][out++]=next;
                    last=next;dl=dn;
                }count=out;src=!src;
            }
            for(unsigned i=2;i<count;i++){
                TextureVertex v[3]={poly[src][0],poly[src][i-1],poly[src][i]};Screen s[3];
                for(unsigned j=0;j<3;j++){s[j]=project(r,v[j].p);s[j].u=v[j].u;s[j].v=v[j].v;}
                raster_triangle(r,s[0],s[1],s[2],0xff000000,0,0,0);
            }
        }
    }else if(r->texture || r->detail)draw_textured_triangle(r,a,b,c,one_sided,stipple,bias);
    else draw_triangle(r,a.p,b.p,c.p,color,one_sided,stipple,bias);
}

static int draw_polygon(Render *r, const LhxSceneMesh *mesh, unsigned number)
{
    const LhxScenePrimitive *p = &mesh->primitives[number];
    const Face *face = &scratch.faces[number];
    int stipple=p->stipple;
    if(stipple && r->rotors){
        int rotor=0;for(unsigned i=0;i<p->count;i++)if(scratch.rotor_counts[p->indices[i]]>=3)rotor=1;
        stipple=rotor?2:3;
    }
    if (face->group < 0 || p->count < 3)
        return 1;
    const Face *root = &scratch.faces[scratch.groups[face->group].root];
    unsigned count = 0;
    for (unsigned i = 0; i < p->count; i++) {
        unsigned index = p->indices[i];
        Vec3 v = vertex(&mesh->vertices[index]);
        if (face->rank && scratch.vertex_groups[index] != -2) {
            double distance = dot(root->normal, v) - root->plane;
            v.x -= root->normal.x * distance;
            v.y -= root->normal.y * distance;
            v.z -= root->normal.z * distance;
        }
        if (!count || v.x != scratch.polygon[count - 1].x ||
                      v.y != scratch.polygon[count - 1].y ||
                      v.z != scratch.polygon[count - 1].z) {
            if (mesh->texture) scratch.uvs[count]=mesh->uvs[index];
            scratch.polygon[count++] = v;
        }
    }
    if (count > 1 && scratch.polygon[0].x == scratch.polygon[count - 1].x &&
                     scratch.polygon[0].y == scratch.polygon[count - 1].y &&
                     scratch.polygon[0].z == scratch.polygon[count - 1].z)
        count--;
    if (count < 3)
        return 1;
    unsigned triangles;
    if (!triangulate(count, face->normal, &triangles))
        return 0;
    /* Keep the bias bounded even for unusually dense, nearly coplanar meshes. */
    double bias = fmin(face->rank * DECAL_BIAS, 0.02);
    double shade=1;
    if (r->options && r->options->lighting && r->view->has_camera && mesh->layer==2) {
        /* A normal transforms with R transpose back to world space. This also
         * handles the authored camera's nonuniform scaling without headlight drift. */
        const double (*m)[3]=r->view->world_to_view;
        Vec3 n={m[0][0]*face->normal.x+m[1][0]*face->normal.y+m[2][0]*face->normal.z,
                m[0][1]*face->normal.x+m[1][1]*face->normal.y+m[2][1]*face->normal.z,
                m[0][2]*face->normal.x+m[1][2]*face->normal.y+m[2][2]*face->normal.z};
        double length=sqrt(dot(n,n));
        if (length>1e-12) {
            double sign=dot(face->normal,face->center)>0?-1:1;
            double sun=fmax(0,sign*(-n.x+1.1*n.y+n.z)/(length*sqrt(3.21)));
            shade=fmax(0.4,fmin(1.25,0.55+0.6*sun+0.1*sign*n.y/length));
        }
    }
    uint32_t color=r->colors[p->color&255];
    if(r->detail_kind && r->options->military_paint)color=lhx_military_paint(color);
    r->detail=r->detail_kind && r->options->surface_detail && !p->stipple?r->detail_kind:0;
    if(r->detail){
        Vec3 local[3];
        for(unsigned i=0;i<3;i++){
            Vec3 q=scratch.polygon[i];double v[3]={q.x-r->local_offset[0],q.y-r->local_offset[1],q.z-r->local_offset[2]};
            local[i]=(Vec3){dot((Vec3){r->local_inverse[0][0],r->local_inverse[0][1],r->local_inverse[0][2]},(Vec3){v[0],v[1],v[2]}),
                dot((Vec3){r->local_inverse[1][0],r->local_inverse[1][1],r->local_inverse[1][2]},(Vec3){v[0],v[1],v[2]}),
                dot((Vec3){r->local_inverse[2][0],r->local_inverse[2][1],r->local_inverse[2][2]},(Vec3){v[0],v[1],v[2]})};
        }
        Vec3 a={local[1].x-local[0].x,local[1].y-local[0].y,local[1].z-local[0].z};
        Vec3 b={local[2].x-local[0].x,local[2].y-local[0].y,local[2].z-local[0].z};
        Vec3 n={fabs(a.y*b.z-a.z*b.y),fabs(a.z*b.x-a.x*b.z),fabs(a.x*b.y-a.y*b.x)};
        unsigned axis=n.x>n.y && n.x>n.z?0:n.y>n.z?1:2;
        for(unsigned i=0;i<count;i++){
            Vec3 q=scratch.polygon[i];double v[3]={q.x-r->local_offset[0],q.y-r->local_offset[1],q.z-r->local_offset[2]},l[3];
            for(unsigned j=0;j<3;j++)l[j]=r->local_inverse[j][0]*v[0]+r->local_inverse[j][1]*v[1]+r->local_inverse[j][2]*v[2];
            scratch.uvs[i]=(LhxSceneUV){axis==0?l[2]:l[0],axis==1?l[2]:l[1]};
        }
    }
    if(r->sink && r->sink->material && !r->sink->material(r->sink_context,r->detail,0))r->emit_failed=1;
    if (shade!=1) color=tint_color(color,shade);
    r->texture=mesh->texture;r->tint=r->detail?1:shade;r->material_color=color;
    for (unsigned i = 0; i < triangles; i += 3) {
        if (mesh->texture || r->detail) {
            TextureVertex v[3];
            for (unsigned j=0;j<3;j++) {
                unsigned k=scratch.triangles[i+j];
                v[j]=(TextureVertex){scratch.polygon[k],scratch.uvs[k].u,scratch.uvs[k].v};
            }
            ground_triangle(r,v[0],v[1],v[2],color,r->relief?0:p->one_sided,stipple,bias,0);
        } else {
            TextureVertex v[3];for(unsigned j=0;j<3;j++)v[j]=(TextureVertex){scratch.polygon[scratch.triangles[i+j]],0,0};
            ground_triangle(r,v[0],v[1],v[2],color,r->relief?0:p->one_sided,stipple,bias,0);
        }
    }
    r->texture=NULL;r->detail=0;
    if(r->sink && r->sink->material && !r->sink->material(r->sink_context,0,0))r->emit_failed=1;
    return 1;
}

static int draw_runway(Render *r,const LhxSceneMesh *mesh)
{
    if (!runway_texture.count && !lhx_texture_runway(&runway_texture)) return 0;
    TextureVertex v[4];
    for (unsigned i=0;i<4;i++)
        v[i]=(TextureVertex){vertex(&mesh->runway_corners[i]),i==1 || i==2,i>=2};
    r->texture=&runway_texture;
    if(r->options && r->options->runway_detail && mesh->has_placement){
        int length=(int)round(mesh->model_bounds[5]-mesh->model_bounds[4]),width=(int)round(mesh->model_bounds[1]-mesh->model_bounds[0]);
        double x=0,z=0;for(unsigned i=0;i<3;i++){
            x+=r->inverse_basis[0][i]*mesh->model_to_view[i][2];z+=r->inverse_basis[2][i]*mesh->model_to_view[i][2];}
        int bearing=(int)round(atan2(x,z)*180/3.14159265358979323846);
        unsigned slot=0;for(;slot<8;slot++)if(runway_details[slot].texture.count && runway_details[slot].length==length &&
            runway_details[slot].width==width && runway_details[slot].bearing==bearing)break;
        if(slot==8){slot=runway_cursor++%8;if(!lhx_texture_runway_detail(&runway_details[slot].texture,length,width,bearing))return 0;
            runway_details[slot].length=length;runway_details[slot].width=width;runway_details[slot].bearing=bearing;}
        r->texture=&runway_details[slot].texture;
    }
    r->tint=r->daylight;
    ground_triangle(r,v[0],v[1],v[2],0,0,0,0.001,0);
    ground_triangle(r,v[0],v[2],v[3],0,0,0,0.001,0);
    r->texture=NULL;
    return 1;
}

static void line_sample(Render *r, int x, int y, Screen from, double dx,
                          double dy, double length2, double qdelta,
                          double radius2, uint32_t color, int stipple)
{
    double px = x + 0.5 - from.x, py = y + 0.5 - from.y;
    double t = length2 > 1e-18 ? (px * dx + py * dy) / length2 : 0.0;
    t = fmax(0.0, fmin(1.0, t));
    double ex = px - t * dx, ey = py - t * dy;
    if (r->options && r->options->antialiasing) {
        double distance=sqrt(ex*ex+ey*ey);
        covered_pixel(r,x,y,from.q+qdelta*t,color,stipple,sqrt(radius2)+0.5-distance);
    } else if (ex * ex + ey * ey <= radius2)
        put_pixel(r, x, y, from.q + qdelta * t, color, stipple, LINE_BIAS);
}

static void draw_line(Render *r, Vec3 a, Vec3 b, uint32_t color, int stipple)
{
    if(r->drape){
        unsigned n=(unsigned)fmin(4096,fmax(1,ceil(hypot(hypot(b.x-a.x,b.y-a.y),b.z-a.z)/400)));
        r->drape=0;for(unsigned i=0;i<n;i++)draw_line(r,draped(r,between(a,b,(double)i/n)),
            draped(r,between(a,b,(double)(i+1)/n)),color,stipple);r->drape=1;return;
    }
    if (r->sink) {
        if (!r->sink->line(r->sink_context,(LhxSceneVertex){a.x,a.y,a.z},
                           (LhxSceneVertex){b.x,b.y,b.z},color,stipple)) r->emit_failed=1;
        return;
    }
    if (!clip_line(r, &a, &b))
        return;
    Screen from = project(r, a), to = project(r, b);
    double dx = to.x - from.x, dy = to.y - from.y;
    double length2 = dx * dx + dy * dy;
    double radius = fmax(0.5, 0.4 * fmin(r->sx, r->sy));
    int smooth=r->options && r->options->antialiasing;
    if (smooth) radius=0.5;
    double extent=radius+(smooth?0.5:0);
    int x0 = lower_pixel(fmin(from.x, to.x) - extent, r->x0, r->x1);
    int x1 = lower_pixel(fmax(from.x, to.x) + extent, r->x0, r->x1);
    int y0 = lower_pixel(fmin(from.y, to.y) - extent, r->y0, r->y1);
    int y1 = lower_pixel(fmax(from.y, to.y) + extent, r->y0, r->y1);
    /* Scan a narrow capsule rather than the diagonal's full bounding box. */
    if (length2 > 1e-18 && fabs(dx) >= fabs(dy)) {
        double reach = extent * sqrt(length2) / fabs(dx);
        for (int x = x0; x < x1; x++) {
            double t = fmax(0.0, fmin(1.0, (x + 0.5 - from.x) / dx));
            double center = from.y + t * dy;
            int first = lower_pixel(center - reach, y0, y1);
            int last = lower_pixel(center + reach, y0, y1);
            for (int y = first; y < last; y++)
                line_sample(r, x, y, from, dx, dy, length2, to.q - from.q,
                              radius * radius, color, stipple);
        }
    } else if (length2 > 1e-18) {
        double reach = extent * sqrt(length2) / fabs(dy);
        for (int y = y0; y < y1; y++) {
            double t = fmax(0.0, fmin(1.0, (y + 0.5 - from.y) / dy));
            double center = from.x + t * dx;
            int first = lower_pixel(center - reach, x0, x1);
            int last = lower_pixel(center + reach, x0, x1);
            for (int x = first; x < last; x++)
                line_sample(r, x, y, from, dx, dy, length2, to.q - from.q,
                              radius * radius, color, stipple);
        }
    } else {
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
                line_sample(r, x, y, from, dx, dy, length2, 0.0,
                              radius * radius, color, stipple);
    }
}

static void smooth_point(Render *r,Screen p,uint32_t color,int stipple,double energy)
{
    if (p.x<r->x0-1 || p.x>r->x1+1 || p.y<r->y0-1 || p.y>r->y1+1) return;
    int x0=(int)floor(p.x-0.5),y0=(int)floor(p.y-0.5);
    double fx=p.x-0.5-x0,fy=p.y-0.5-y0;
    for (int j=0;j<2;j++) for (int i=0;i<2;i++) {
        int x=x0+i,y=y0+j;
        if (x>=r->x0 && x<r->x1 && y>=r->y0 && y<r->y1)
            covered_pixel(r,x,y,p.q,color,stipple,energy*(i?fx:1-fx)*(j?fy:1-fy));
    }
}

static void draw_point(Render *r, Vec3 center, uint32_t color, int stipple)
{
    if(r->drape)center=draped(r,center);
    if (r->sink) {
        if (!r->sink->particle(r->sink_context,
            (LhxSceneVertex){center.x,center.y,center.z},0,color,stipple)) r->emit_failed=1;
        return;
    }
    if (center.z < NEAR_Z)
        return;
    Screen p = project(r, center);
    if (r->options && r->options->antialiasing) {
        /* One output-pixel of energy, independent of scale and subpixel phase. */
        smooth_point(r,p,color,stipple,1);
        return;
    }
    double radius = fmax(0.5, 0.5 * fmin(r->sx, r->sy));
    int x0 = lower_pixel(p.x - radius, r->x0, r->x1);
    int x1 = lower_pixel(p.x + radius, r->x0, r->x1);
    int y0 = lower_pixel(p.y - radius, r->y0, r->y1);
    int y1 = lower_pixel(p.y + radius, r->y0, r->y1);
    int covered = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            double dx = x + 0.5 - p.x, dy = y + 0.5 - p.y;
            if (dx * dx + dy * dy <= radius * radius) {
                put_pixel(r, x, y, p.q, color, stipple, LINE_BIAS);
                covered = 1;
            }
        }
    /* A one-pixel dot centered on four pixel corners still needs one sample. */
    if (!covered && p.x >= r->x0 && p.x < r->x1 && p.y >= r->y0 && p.y < r->y1)
        put_pixel(r, (int)floor(p.x), (int)floor(p.y), p.q, color, stipple, LINE_BIAS);
}

static void draw_sphere(Render *r, Vec3 center, double radius, uint32_t color,
                         int stipple)
{
    if (r->sink) {
        if (!r->sink->particle(r->sink_context,
            (LhxSceneVertex){center.x,center.y,center.z},radius,color,stipple)) r->emit_failed=1;
        return;
    }
    if (radius <= 0.0) {
        draw_point(r, center, color, stipple);
        return;
    }
    if (center.z + radius < NEAR_Z)
        return;
    if (r->options && r->options->antialiasing && center.z-radius>NEAR_Z) {
        double rx=r->sx*r->focus*radius/center.z;
        double ry=r->sy*Y_SCALE*r->focus*radius*0.86/center.z;
        double area=3.14159265358979323846*rx*ry;
        if (area<1) {
            /* Finite distant geometry fades with projected area, instead of
             * alternating between an oversized dot and no pixel at all. */
            smooth_point(r,project(r,center),color,stipple,area);
            return;
        }
    }
    int x0 = r->x0, x1 = r->x1, y0 = r->y0, y1 = r->y1;
    if (center.z - radius > NEAR_Z) {
        double xmin = DBL_MAX, xmax = -DBL_MAX, ymin = DBL_MAX, ymax = -DBL_MAX;
        for (int zsign = -1; zsign <= 1; zsign += 2) {
            for (int sign = -1; sign <= 1; sign += 2) {
                double z = center.z + zsign * radius;
                double x = r->sx * (r->cx + r->focus * (center.x + sign * radius) / z);
                double y = r->sy * (r->cy - Y_SCALE * r->focus *
                                     (center.y + sign * radius * 0.86) / z);
                xmin = fmin(xmin, x);
                xmax = fmax(xmax, x);
                ymin = fmin(ymin, y);
                ymax = fmax(ymax, y);
            }
        }
        x0 = lower_pixel(xmin, r->x0, r->x1);
        x1 = lower_pixel(xmax, r->x0, r->x1);
        y0 = lower_pixel(ymin, r->y0, r->y1);
        y1 = lower_pixel(ymax, r->y0, r->y1);
    }
    const double ys = 1.0 / (0.86 * 0.86);
    double constant = center.x * center.x + center.y * center.y * ys +
                       center.z * center.z - radius * radius;
    double dx = 1.0 / (r->sx * r->focus);
    double rayx0 = ((x0 + 0.5) / r->sx - r->cx) / r->focus;
    for (int y = y0; y < y1; y++) {
        double rayy = (r->cy - (y + 0.5) / r->sy) / (Y_SCALE * r->focus);
        double rayx = rayx0;
        for (int x = x0; x < x1; x++, rayx += dx) {
            if (stipple && ((x ^ y) & 1))
                continue;
            double a = rayx * rayx + rayy * rayy * ys + 1.0;
            double b = rayx * center.x + rayy * center.y * ys + center.z;
            double discriminant = b * b - a * constant;
            if (discriminant < 0.0)
                continue;
            double root = sqrt(discriminant);
            double z = (b - root) / a;
            if (z < NEAR_Z)
                z = (b + root) / a;
            if (z >= NEAR_Z)
                put_pixel(r, x, y, 1.0 / z, color, 0, 0.0);
        }
    }
}

static int mesh_layer(const LhxSceneMesh *mesh,const LhxSceneRenderOptions *options)
{
    LhxPropFit fit;
    if(options && lhx_props_select(options->props,mesh,&fit))return 2;
    /* Capture retains the original background layer for point/sphere-only
     * models. Finite smoke spheres occupy space and must share solid geometry's
     * depth buffer; ordinary point markers and stars keep their background pass. */
    if (mesh->layer==0 && options && options->sphere_depth)
        for (unsigned i=0;i<mesh->primitive_count;i++)
            if (mesh->primitives[i].kind==3 && mesh->primitives[i].radius>0)
                return 2;
    return mesh->layer;
}

static int helicopter(const char *name){
    const char *names[]={"apache","black","lhx","osprey","hind","hip","havoc","hokum"};
    for(unsigned i=0;i<8;i++)if(!strcmp(name,names[i]))return 1;
    return 0;
}
static Vec3 cross(Vec3 a,Vec3 b){return (Vec3){a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static Vec3 subtract(Vec3 a,Vec3 b){return (Vec3){a.x-b.x,a.y-b.y,a.z-b.z};}
static void rotor_blades(Render *r,const LhxSceneMesh *mesh)
{
    if(!r->rotors)return;
    unsigned most=0;for(unsigned i=0;i<mesh->vertex_count;i++)if(scratch.rotor_counts[i]>most)most=scratch.rotor_counts[i];
    unsigned discs=0;
    for(unsigned hub=0;hub<mesh->vertex_count && discs<16;hub++){
        if(scratch.rotor_counts[hub]<3)continue;
        Vec3 h=vertex(mesh->vertices+hub),normal={0},axis={0};double radius=0;int have_axis=0;
        for(unsigned i=0;i<mesh->primitive_count;i++){
            const LhxScenePrimitive *p=mesh->primitives+i;if(p->kind || !p->stipple)continue;
            int shares=0;for(unsigned j=0;j<p->count;j++)if(p->indices[j]==hub)shares=1;
            if(!shares)continue;
            for(unsigned j=0;j<p->count;j++){
                Vec3 a=subtract(vertex(mesh->vertices+p->indices[j]),h);
                if(dot(a,a)>radius*radius)radius=sqrt(dot(a,a));
                if(!have_axis && dot(a,a)>1e-6){axis=a;have_axis=1;}
                Vec3 b=subtract(vertex(mesh->vertices+p->indices[(j+1)%p->count]),h),n=cross(a,b);
                double sign=dot(n,normal)<0?-1:1;normal.x+=sign*n.x;normal.y+=sign*n.y;normal.z+=sign*n.z;
            }
        }
        double nl=sqrt(dot(normal,normal));if(radius<1 || nl<1e-6)continue;
        normal.x/=nl;normal.y/=nl;normal.z/=nl;
        double d=dot(axis,normal);axis.x-=d*normal.x;axis.y-=d*normal.y;axis.z-=d*normal.z;
        double al=sqrt(dot(axis,axis));if(al<1e-6)continue;
        axis.x/=al;axis.y/=al;axis.z/=al;Vec3 side=cross(normal,axis);
        int main=scratch.rotor_counts[hub]==most;double width=radius*(main?.045:.09);
        double time=r->options->time_seconds;if(!isfinite(time))time=0;
        double turn=fmod(time,1000)*(main?3.1:6.2)*6.283185307179586;
        for(unsigned k=0;k<4;k++){
            double t=turn+k*1.570796326794897,c=cos(t),s=sin(t);
            Vec3 along={c*axis.x+s*side.x,c*axis.y+s*side.y,c*axis.z+s*side.z};
            Vec3 across={-s*axis.x+c*side.x,-s*axis.y+c*side.y,-s*axis.z+c*side.z};
            Vec3 q[4];for(unsigned j=0;j<4;j++){
                double a=j==1 || j==2?radius:radius*.06,b=(j>=2?1:-1)*width*.5;
                q[j]=(Vec3){h.x+along.x*a+across.x*b,h.y+along.y*a+across.y*b,h.z+along.z*a+across.z*b};}
            draw_triangle(r,q[0],q[1],q[2],0xff1f1f1c,0,0,0.0002);
            draw_triangle(r,q[0],q[2],q[3],0xff1f1f1c,0,0,0.0002);
        }
        discs++;
    }
}

static int draw_placed_mesh(Render *r, const LhxSceneMesh *mesh)
{
    if(r->view->has_camera && r->options && r->options->sky &&
       r->options->sky->night.count && lhx_haze_color(r->colors[r->view->sky_color&255])==0xff0d0f17u &&
       !strcmp(mesh->model_name,"stars"))return 1;
    LhxPropFit fit;
    const LhxPropModel *replacement=r->options?
        lhx_props_select(r->options->props,mesh,&fit):NULL;
    if(replacement){
        if(r->sink && r->sink->prop)
            return r->sink->prop(r->sink_context,replacement,mesh,&fit,
                                  r->daylight,r->options->lighting,NULL);
        for(unsigned g=0;g<replacement->count;g++){
            const LhxPropGroup *group=replacement->groups+g;r->texture=&group->texture;
            for(unsigned i=0;i<group->count;i+=3){
                TextureVertex triangle[3];
                for(unsigned j=0;j<3;j++){
                    const LhxPropVertex *v=group->vertices+i+j;
                    LhxSceneVertex p=lhx_props_vertex(mesh,&fit,v->position);
                    triangle[j]=(TextureVertex){{p.x,p.y,p.z},v->uv.u,v->uv.v};
                }
                double shade=1;
                if(r->options->lighting){
                    Vec3 a=triangle[0].p,b=triangle[1].p,c=triangle[2].p;
                    Vec3 u={b.x-a.x,b.y-a.y,b.z-a.z},v={c.x-a.x,c.y-a.y,c.z-a.z};
                    Vec3 n={u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
                    double sign=dot(n,a)>0?-1:1;const double (*m)[3]=r->view->world_to_view;
                    Vec3 world={m[0][0]*n.x+m[1][0]*n.y+m[2][0]*n.z,
                                m[0][1]*n.x+m[1][1]*n.y+m[2][1]*n.z,
                                m[0][2]*n.x+m[1][2]*n.y+m[2][2]*n.z};
                    double length=sqrt(dot(world,world));
                    if(length>1e-12)shade=fmax(.4,fmin(1.25,.55+.6*fmax(0,sign*
                        (-world.x+1.1*world.y+world.z)/(length*sqrt(3.21)))+.1*sign*world.y/length));
                }
                r->tint=r->daylight*shade;
                draw_textured_triangle(r,triangle[0],triangle[1],triangle[2],group->one_sided,0,0);
            }
        }
        r->texture=NULL;return !r->emit_failed;
    }
    prepare_faces(mesh);
    r->rotors=r->options && r->options->rotor_blades && helicopter(mesh->model_name);
    if(r->rotors){
        memset(scratch.rotor_counts,0,mesh->vertex_count*sizeof(*scratch.rotor_counts));
        for(unsigned i=0;i<mesh->primitive_count;i++){
            const LhxScenePrimitive *p=mesh->primitives+i;if(p->kind || !p->stipple)continue;
            for(unsigned j=0;j<p->count;j++){
                int duplicate=0;for(unsigned k=0;k<j;k++)if(p->indices[k]==p->indices[j])duplicate=1;
                if(!duplicate)scratch.rotor_counts[p->indices[j]]++;
            }
        }
    }
    for (unsigned i = 0; i < mesh->primitive_count; i++) {
        const LhxScenePrimitive *p = &mesh->primitives[i];
        uint32_t color = r->colors[p->color & 255];
        switch (p->kind) {
        case 0:
            if (!draw_polygon(r, mesh, i))
                return 0;
            break;
        case 1:
            for (unsigned j = 1; j < p->count; j++)
                draw_line(r, vertex(&mesh->vertices[p->indices[j - 1]]),
                             vertex(&mesh->vertices[p->indices[j]]), color, p->stipple);
            break;
        case 2:
            for (unsigned j = 0; j < p->count; j++)
                draw_point(r, vertex(&mesh->vertices[p->indices[j]]), color, p->stipple);
            break;
        case 3:
            if (p->count)
                draw_sphere(r, vertex(&mesh->vertices[p->indices[0]]),
                               p->radius, color, p->stipple);
            break;
        default:
            /* Custom callbacks keep their original 2D overlay in the compositor. */
            break;
        }
    }
    rotor_blades(r,mesh);r->rotors=0;
    return 1;
}

static int model_kind(const char *name)
{
    const char *air[]={"apache","black","lhx","osprey","hind","hip","havoc","hokum","b2","mig27","su25"};
    const char *vehicle[]={"t80","m1","m2","m113","bmp2","brdm","btr70","zsu23","zsu57","sa6","sa8","sa11","sa13","s60","s23","truck","radar"};
    for(unsigned i=0;i<sizeof air/sizeof *air;i++)if(!strcmp(name,air[i]))return 1;
    for(unsigned i=0;i<sizeof vehicle/sizeof *vehicle;i++)if(!strcmp(name,vehicle[i]))return 2;
    return 0;
}
static int hill(const char *name){return !strcmp(name,"hill1") || !strcmp(name,"hill2") ||
    !strcmp(name,"hill3") || !strcmp(name,"hill4") || !strcmp(name,"mountain");}
static const LhxSceneMesh *place_mesh(Render *r,const LhxSceneMesh *original,LhxSceneMesh *placed)
{
    r->drape=0;r->subdivisions=0;
    if(!r->relief)return original;
    if(!strcmp(original->model_name,"stars")){
        *placed=*original;for(unsigned i=0;i<original->vertex_count;i++)scratch.placed[i]=(LhxSceneVertex){
            original->vertices[i].x*10000,original->vertices[i].y*10000,original->vertices[i].z*10000};
        placed->vertices=scratch.placed;return placed;
    }
    if(!original->has_placement)return original;
    int finite_sphere=0;for(unsigned i=0;i<original->primitive_count;i++)
        if(original->primitives[i].kind==3 && original->primitives[i].radius>0)finite_sphere=1;
    if(original->layer<2 && !finite_sphere && strcmp(original->model_name,"stars")){
        r->drape=1;return original;
    }
    if(!strcmp(original->model_name,"stars"))return original;
    *placed=*original;
    double d=lhx_relief_height(r->options->terrain,original->world_origin[0],original->world_origin[2])-
        lhx_relief_height(r->options->terrain,r->view->camera[0],r->view->camera[2]);
    LhxPropFit fit;
    if(!lhx_props_select(r->options->props,original,&fit))
        d+=fmax(0,-original->world_origin[1]-original->model_bounds[2]);
    double shift[3];for(unsigned i=0;i<3;i++){
        shift[i]=r->view->world_to_view[i][1]*d;placed->model_to_view[i][3]+=shift[i];
    }
    for(unsigned i=0;i<original->vertex_count;i++)scratch.placed[i]=(LhxSceneVertex){
        original->vertices[i].x+shift[0],original->vertices[i].y+shift[1],original->vertices[i].z+shift[2]};
    placed->vertices=scratch.placed;return placed;
}
static int draw_mesh(Render *r,const LhxSceneMesh *mesh)
{
    if(r->relief && hill(mesh->model_name))return 1;
    LhxSceneMesh placed;mesh=place_mesh(r,mesh,&placed);
    r->detail_kind=r->options && mesh->has_placement?model_kind(mesh->model_name):0;
    if(r->detail_kind){
        double matrix[3][3];for(unsigned i=0;i<3;i++){
            r->local_offset[i]=mesh->model_to_view[i][3];
            for(unsigned j=0;j<3;j++)matrix[i][j]=mesh->model_to_view[i][j];
        }
        if(!inverse(matrix,r->local_inverse))r->detail_kind=0;
    }
    if(mesh->runway && r->view->has_camera && r->options && r->options->runway_textures)
        return draw_runway(r,mesh);
    return draw_placed_mesh(r,mesh);
}

static int draw_shadows(Render *r)
{
    if(!r->options || !r->options->shadows || !r->view->has_camera ||
       !inverse(r->view->world_to_view,r->inverse_basis))return 1;
    if(!r->sink){
        size_t size=(size_t)r->width*r->height;
        if(size>scratch.shadow_capacity){void *p=realloc(scratch.shadow_mask,size);if(!p)return 0;
            scratch.shadow_mask=p;scratch.shadow_capacity=size;}
        memset(scratch.shadow_mask,0,size);
    }
    r->depth_enabled=1;
    size_t need=(size_t)r->view->mesh_count*4;
    if(need>scratch.contact_capacity){void *p=realloc(scratch.contacts,need*sizeof(*scratch.contacts));if(!p)return 0;
        scratch.contacts=p;scratch.contact_capacity=need;}
    unsigned contacts=0;
    for(unsigned i=0;i<r->view->mesh_count;i++){
        const LhxSceneMesh *original=r->view->meshes+i;
        if(!original->has_placement || original->layer!=2 || model_kind(original->model_name)==1 ||
           hill(original->model_name) || original->world_origin[1]+original->model_bounds[2]>=24)continue;
        LhxSceneMesh placed;const LhxSceneMesh *mesh=place_mesh(r,original,&placed);
        r->local_offset[0]=mesh->world_origin[0];r->local_offset[2]=mesh->world_origin[2];
        r->shadow=1;r->texture=NULL;r->detail=0;r->drape=0;
        r->shadow_ground=lhx_relief_height(r->relief?r->options->terrain:NULL,mesh->world_origin[0],mesh->world_origin[2]);
        if(r->sink && r->sink->material && !r->sink->material(r->sink_context,0,1))return 0;
        LhxPropFit fit;const LhxPropModel *prop=lhx_props_select(r->options->props,mesh,&fit);
        r->shadow_flat=prop!=NULL;if(prop)shadow_plane(r);
        if(prop && r->sink && r->sink->prop){
            if(!r->sink->prop(r->sink_context,prop,mesh,&fit,r->daylight,0,r->shadow_matrix))return 0;
        }else if(prop){
            for(unsigned g=0;g<prop->count;g++)for(unsigned k=0;k<prop->groups[g].count;k+=3){
                TextureVertex v[3];for(unsigned j=0;j<3;j++){
                    LhxSceneVertex p=lhx_props_vertex(mesh,&fit,prop->groups[g].vertices[k+j].position);
                    v[j]=(TextureVertex){{p.x,p.y,p.z},0,0};}
                ground_triangle(r,v[0],v[1],v[2],0,0,0,0,0);
            }
        }else{
            prepare_faces(mesh);
            for(unsigned k=0;k<mesh->primitive_count;k++){
                const LhxScenePrimitive *p=mesh->primitives+k;
                if(p->kind || p->stipple || p->count<3)continue;
                for(unsigned j=0;j<p->count;j++)scratch.polygon[j]=vertex(mesh->vertices+p->indices[j]);
                unsigned count;if(!triangulate(p->count,scratch.faces[k].normal,&count))continue;
                for(unsigned j=0;j<count;j+=3){TextureVertex v[3];for(unsigned n=0;n<3;n++)v[n]=(TextureVertex){scratch.polygon[scratch.triangles[j+n]],0,0};
                    ground_triangle(r,v[0],v[1],v[2],0,0,0,0,0);}
            }
        }
        if(prop){
            /* Footprint contact shade: original authored footprint and heading. */
            int tree=!strcmp(mesh->model_name,"palm") || !strcmp(mesh->model_name,"tree2");
            double scale=tree?.6:1.35,cx=(mesh->model_bounds[0]+mesh->model_bounds[1])*.5,
                cz=(mesh->model_bounds[4]+mesh->model_bounds[5])*.5;
            TextureVertex v[4];
            for(unsigned j=0;j<4;j++){
                double u=j==1 || j==2,w=j>=2;
                double x=cx+(u-.5)*(mesh->model_bounds[1]-mesh->model_bounds[0])*scale;
                double z=cz+(w-.5)*(mesh->model_bounds[5]-mesh->model_bounds[4])*scale;
                Vec3 q={mesh->model_to_view[0][3]+mesh->model_to_view[0][0]*x+mesh->model_to_view[0][2]*z,
                        mesh->model_to_view[1][3]+mesh->model_to_view[1][0]*x+mesh->model_to_view[1][2]*z,
                        mesh->model_to_view[2][3]+mesh->model_to_view[2][0]*x+mesh->model_to_view[2][2]*z};
                q=draped(r,q);v[j]=(TextureVertex){{q.x*.99,q.y*.99,q.z*.99},u,w};
            }
            memcpy(scratch.contacts+contacts*4,v,sizeof v);contacts++;
        }
    }
    if(contacts){
        r->shadow=2;if(r->sink && r->sink->material && !r->sink->material(r->sink_context,0,2))return 0;
        for(unsigned i=0;i<contacts;i++){
            TextureVertex *v=scratch.contacts+i*4;
            ground_triangle(r,v[0],v[1],v[2],0,0,0,0,0);ground_triangle(r,v[0],v[2],v[3],0,0,0,0,0);
        }
    }
    r->shadow=0;r->shadow_flat=0;
    if(r->sink && r->sink->material && !r->sink->material(r->sink_context,0,0))return 0;
    return !r->emit_failed;
}

static void clear_depth(const Render *r)
{
    for (int y = r->y0; y < r->y1; y++)
        memset(scratch.depth + (size_t)y * r->width + r->x0, 0,
                (size_t)(r->x1 - r->x0) * sizeof *scratch.depth);
}

static void draw_horizon(Render *r, const LhxSceneHorizon *horizon)
{
    uint32_t a = r->colors[horizon->colors[0] & 255];
    uint32_t b = r->colors[horizon->colors[1] & 255];
    for (int y = r->y0; y < r->y1; y++) {
        double side = ((r->x0 + 0.5) / r->sx - horizon->x) * horizon->nx +
                      ((y + 0.5) / r->sy - horizon->y) * horizon->ny;
        double step = horizon->nx / r->sx;
        size_t index = (size_t)y * r->width + (unsigned)r->x0;
        for (int x = r->x0; x < r->x1; x++, index++, side += step) {
            if(r->atmosphere || r->panorama){
                double vx=((x+0.5)/r->sx-r->cx)/r->focus;
                double vy=(r->cy-(y+0.5)/r->sy)/(Y_SCALE*r->focus),ray[3];
                for(unsigned i=0;i<3;i++)ray[i]=r->inverse_basis[i][0]*vx+
                    r->inverse_basis[i][1]*vy+r->inverse_basis[i][2];
                double elevation=asin(fmax(-1,fmin(1,ray[1]/hypot(hypot(ray[0],ray[1]),ray[2]))));
                uint32_t base=ray[1]>=0?r->colors[r->view->sky_color&255]:
                    r->options->terrain && r->atmosphere?r->haze:r->colors[r->view->ground_color&255];
                if(ray[1]>=0 && r->panorama){
                    double u=.5+atan2(ray[0],ray[2])/6.2831853071795864769;
                    double v=.5-elevation/3.14159265358979323846;
                    double footprint=fmax(r->panorama->levels[0].width/(6.2831853*r->focus*r->sx),
                        r->panorama->levels[0].height/(3.14159265*Y_SCALE*r->focus*r->sy));
                    base=tint_color(lhx_texture_sample(r->panorama,u,v,footprint),.78);
                }
                r->pixels[index]=r->atmosphere?
                    lhx_atmosphere_mix(base,r->haze,1-exp(-fabs(elevation)*40)):base;
            }else r->pixels[index] = !horizon->two_colors || side >= 0.0 ? a : b;
        }
    }
}

static int inverse(const double m[3][3],double out[3][3])
{
    double det=0;
    for (unsigned i=0;i<3;i++) {
        unsigned a=(i+1)%3,b=(i+2)%3;
        double c=m[1][a]*m[2][b]-m[1][b]*m[2][a];
        det+=m[0][i]*c;
    }
    if (!isfinite(det) || fabs(det)<0.01) return 0;
    for (unsigned i=0;i<3;i++) for (unsigned j=0;j<3;j++)
        out[j][i]=(m[(i+1)%3][(j+1)%3]*m[(i+2)%3][(j+2)%3]-
                   m[(i+1)%3][(j+2)%3]*m[(i+2)%3][(j+1)%3])/det;
    return 1;
}

static const LhxTerrainGrid *terrain_grid(const LhxTerrain *t,double x,double z)
{
    const LhxTerrainGrid *grids[2]={&t->near,&t->far};
    for (unsigned i=0;i<2;i++) {
        const LhxTerrainGrid *g=grids[i];
        if (g->texture.count && x>=g->bounds[0] && x<=g->bounds[1] &&
            z>=g->bounds[2] && z<=g->bounds[3]) return g;
    }
    return NULL;
}

static void draw_satellite_ground(Render *r)
{
    if (!r->options || !r->options->terrain || !r->view->has_camera) return;
    double inv[3][3];
    if (!inverse(r->view->world_to_view,inv) || r->view->camera[1]<=0) return;
    const double *cam=r->view->camera;
    const LhxTerrain *terrain=r->options->terrain;
    const LhxTerrainGrid *grids[2]={&terrain->near,&terrain->far};
    double iu[2],iv[2];
    for (unsigned i=0;i<2;i++) {
        iu[i]=1/(grids[i]->bounds[1]-grids[i]->bounds[0]);
        iv[i]=1/(grids[i]->bounds[3]-grids[i]->bounds[2]);
    }
    double dx[3],dy[3];
    for (unsigned i=0;i<3;i++) {
        dx[i]=inv[i][0]/(r->sx*r->focus);
        dy[i]=-inv[i][1]/(r->sy*r->focus*Y_SCALE);
    }
    for (int y=r->y0;y<r->y1;y++) {
        double vx=((r->x0+0.5)/r->sx-r->cx)/r->focus;
        double vy=(r->cy-(y+0.5)/r->sy)/(r->focus*Y_SCALE),ray[3];
        for (unsigned i=0;i<3;i++) ray[i]=inv[i][0]*vx+inv[i][1]*vy+inv[i][2];
        for (int x=r->x0;x<r->x1;x++) {
            size_t index=(size_t)y*r->width+(unsigned)x;
            if (ray[1]<-1e-9) {
                double inv_y=1/ray[1],distance=-cam[1]*inv_y;
                double wx=cam[0]+ray[0]*distance,wz=cam[2]+ray[2]*distance;
                const LhxTerrainGrid *g=terrain_grid(r->options->terrain,wx,wz);
                if (g) {
                    unsigned k=g==grids[0]?0:1;
                    double su=g->texture.levels[0].width*iu[k];
                    double sv=g->texture.levels[0].height*iv[k];
                    double ux=distance*(dx[0]-ray[0]*dx[1]*inv_y)*su;
                    double vx1=distance*(dx[2]-ray[2]*dx[1]*inv_y)*sv;
                    double uy=distance*(dy[0]-ray[0]*dy[1]*inv_y)*su;
                    double vy1=distance*(dy[2]-ray[2]*dy[1]*inv_y)*sv;
                    double fx=ux*ux+vx1*vx1,fy=uy*uy+vy1*vy1;
                    uint32_t color=lhx_texture_sample(&g->texture,
                        (wx-g->bounds[0])*iu[k],(g->bounds[3]-wz)*iv[k],sqrt(fx>fy?fx:fy));
                    color=texture_tint(color,r->daylight);
                    r->pixels[index]=r->atmosphere?lhx_atmosphere_mix(
                        color,r->haze,lhx_haze_visibility(distance)):color;
                } else r->pixels[index]=r->atmosphere?r->haze:r->colors[r->view->ground_color&255];
            } else if(!r->atmosphere && !r->panorama)r->pixels[index]=r->colors[r->view->sky_color&255];
            for (unsigned i=0;i<3;i++) ray[i]+=dx[i];
        }
    }
}

static void draw_relief(Render *r)
{
    const LhxTerrainGrid *grids[]={&r->options->terrain->far,&r->options->terrain->near};
    for(unsigned g=0;g<2;g++){
        const LhxTerrainGrid *grid=grids[g];r->texture=&grid->texture;
        for(unsigned i=0;i<grid->index_count;i+=3){
            TextureVertex t[3];double shade=0;
            for(unsigned j=0;j<3;j++){
                const LhxTerrainVertex *v=grid->vertices+grid->indices[i+j];
                LhxSceneVertex p=world_view(r,v->x,v->y,v->z);
                t[j]=(TextureVertex){{p.x,p.y,p.z},v->u,v->v};shade+=v->shade;
            }
            r->tint=r->daylight*shade/3;draw_textured_triangle(r,t[0],t[1],t[2],0,0,0);
        }
    }
    r->texture=NULL;
}

static int luminance(uint32_t color)
{
    return (77*((color>>16)&255)+150*((color>>8)&255)+29*(color&255))>>8;
}

static uint32_t edge_sample(const Render *r,double x,double y)
{
    int x0=(int)floor(x),y0=(int)floor(y);
    unsigned fx=(unsigned)((x-x0)*256+0.5),fy=(unsigned)((y-y0)*256+0.5);
    const uint32_t *p=scratch.antialias_pixels+(size_t)y0*r->width+(unsigned)x0;
    return mix_color(mix_color(p[0],p[1],fx),
                     mix_color(p[r->width],p[r->width+1],fx),fy);
}

/* A small spatial edge filter: sample along the local contour's tangent.
 * Flat/low-contrast regions and view borders remain unchanged. It operates on
 * native pixels before original cockpit/HUD/effect composition, with no extra
 * geometry passes or higher-resolution render target. */
static void smooth_edges(Render *r)
{
    if (r->x1-r->x0<3 || r->y1-r->y0<3) return;
    for (int y=r->y0;y<r->y1;y++)
        memcpy(scratch.antialias_pixels+(size_t)y*r->width+r->x0,
               r->pixels+(size_t)y*r->width+r->x0,
               (size_t)(r->x1-r->x0)*sizeof(*r->pixels));
    for (int y=r->y0+1;y<r->y1-1;y++) for (int x=r->x0+1;x<r->x1-1;x++) {
        size_t at=(size_t)y*r->width+(unsigned)x;
        const uint32_t *p=scratch.antialias_pixels+at;
        int m=luminance(p[0]),n=luminance(p[-(int)r->width]),s=luminance(p[r->width]);
        int e=luminance(p[1]),w=luminance(p[-1]),low=m,high=m;
        const int values[4]={n,s,e,w};
        for (unsigned i=0;i<4;i++) {
            if (values[i]<low) low=values[i];
            if (values[i]>high) high=values[i];
        }
        if (high-low<24 || (high-low)*8<high) continue;
        int gx=e-w,gy=s-n;
        uint32_t filtered;
        int magnitude=abs(gx)>abs(gy)?abs(gx):abs(gy);
        if (!magnitude) {
            uint32_t horizontal=mix_color(p[-1],p[1],128);
            uint32_t vertical=mix_color(p[-(int)r->width],p[r->width],128);
            filtered=mix_color(p[0],mix_color(horizontal,vertical,128),64);
        } else {
            double dx=-0.5*gy/magnitude,dy=0.5*gx/magnitude;
            uint32_t average=mix_color(edge_sample(r,x+dx,y+dy),
                                      edge_sample(r,x-dx,y-dy),128);
            filtered=mix_color(p[0],average,192);
        }
        r->pixels[at]=filtered;
    }
}

static LhxSceneView enhanced_view(const LhxSceneView *view,const LhxSceneRenderOptions *options)
{
    LhxSceneView result=*view;
    /* Target-display windows contain captured world models; the attitude ball
     * has no models and must retain its original independently cached horizon. */
    if(!view->has_camera && view->mesh_count && options &&
       (options->props || options->sky || options->atmosphere || options->shadows ||
        options->military_paint || options->surface_detail || options->rotor_blades ||
        options->runway_detail || (options->terrain_relief && lhx_relief_ready(options->terrain)))){
        int world=0;for(unsigned i=0;i<view->mesh_count;i++)
            if(view->meshes[i].has_placement && view->meshes[i].model_name[0]){world=1;break;}
        double inv[3][3];if(world && inverse(view->world_to_view,inv))result.has_camera=1;
    }
    return result;
}

int lhx_scene_render_with_options(const LhxSceneFrame *frame, const uint8_t palette[768],
                     uint32_t *pixels, unsigned width, unsigned height,
                     const LhxSceneRenderOptions *options)
{
    if (!palette || !pixels || !width || !height || width > MAX_DIMENSION ||
        height > MAX_DIMENSION || (size_t)width * height > MAX_PIXELS)
        return -1;
    unsigned faces = 0, vertices = 0, polygon = 0;
    int valid = validate_frame(frame, &faces, &vertices, &polygon);
    if (valid <= 0)
        return valid;
    if (!reserve_depth((size_t)width * height) || !reserve_mesh(faces, vertices, polygon))
        return -1;
    if (options && options->antialiasing && (size_t)width*height>scratch.antialias_capacity) {
        uint32_t *p=realloc(scratch.antialias_pixels,(size_t)width*height*sizeof(*p));
        if (!p) return -1;
        scratch.antialias_pixels=p;scratch.antialias_capacity=(size_t)width*height;
    }
    Render r;
    memset(&r, 0, sizeof r);
    r.pixels = pixels;
    r.width = width;
    r.height = height;
    r.sx = width / 320.0;
    r.sy = height / 200.0;
    r.options=options;
    for (unsigned i = 0; i < 256; i++) {
        unsigned red = (palette[i * 3] & 63) * 255 / 63;
        unsigned green = (palette[i * 3 + 1] & 63) * 255 / 63;
        unsigned blue = (palette[i * 3 + 2] & 63) * 255 / 63;
        r.colors[i] = 0xff000000u | red << 16 | green << 8 | blue;
    }
    int drawn = 0;
    for (unsigned i = 0; i < frame->view_count; i++) {
        LhxSceneView current=enhanced_view(frame->views+i,options);
        const LhxSceneView *view=&current;
        r.view=view;
        int camera_inverse=view->has_camera && inverse(view->world_to_view,r.inverse_basis);
        r.atmosphere=options && options->atmosphere && camera_inverse;
        r.haze=lhx_haze_color(r.colors[view->sky_color&255]);
        r.panorama=options && options->sky && camera_inverse?
            (r.haze==0xff0d0f17u?&options->sky->night:&options->sky->day):NULL;
        if(r.panorama && (!r.panorama->count || r.panorama->count>13 || !r.panorama->levels[0].pixels))r.panorama=NULL;
        uint32_t sky=r.colors[view->sky_color&255];
        r.daylight=fmin(1,0.12+1.6*(0.30*((sky>>16)&255)+
                      0.59*((sky>>8)&255)+0.11*(sky&255))/255.0);
        if (view->focus <= 0.0f)
            continue;
        int x0 = view->x0 < 0 ? 0 : view->x0;
        int x1 = view->x1 > 320 ? 320 : view->x1;
        int y0 = view->y0 < 0 ? 0 : view->y0;
        int y1 = view->y1 > 200 ? 200 : view->y1;
        if (x0 >= x1 || y0 >= y1)
            continue;
        r.x0 = lower_pixel(x0 * r.sx, 0, (int)width);
        r.x1 = lower_pixel(x1 * r.sx, 0, (int)width);
        r.y0 = lower_pixel(y0 * r.sy, 0, (int)height);
        r.y1 = lower_pixel(y1 * r.sy, 0, (int)height);
        if (r.x0 >= r.x1 || r.y0 >= r.y1)
            continue;
        r.cx = view->cx;
        r.cy = view->cy;
        r.focus = view->focus;
        double left = (x0 - r.cx) / r.focus;
        double right = (x1 - r.cx) / r.focus;
        double top = (r.cy - y0) / (r.focus * Y_SCALE);
        double bottom = (r.cy - y1) / (r.focus * Y_SCALE);
        r.planes[0] = (Vec3){0.0, 0.0, 1.0};
        r.planes[1] = (Vec3){1.0, 0.0, -left};
        r.planes[2] = (Vec3){-1.0, 0.0, right};
        r.planes[3] = (Vec3){0.0, 1.0, -bottom};
        r.planes[4] = (Vec3){0.0, -1.0, top};
        draw_horizon(&r, &view->horizon);
        r.relief=options && options->terrain_relief && camera_inverse && lhx_relief_ready(options->terrain);
        r.base_height=r.relief?lhx_relief_height(options->terrain,r.view->camera[0],r.view->camera[2]):0;
        if(r.relief){r.depth_enabled=1;clear_depth(&r);draw_relief(&r);}
        else draw_satellite_ground(&r);
        for (int layer = 0; layer < 3; layer++) {
            r.depth_enabled = layer != 0 || r.relief;
            if (r.depth_enabled && !r.relief)
                clear_depth(&r);
            for (unsigned m = 0; m < view->mesh_count; m++) {
                const LhxSceneMesh *mesh=&view->meshes[m];
                if (mesh_layer(mesh,options)!=layer) continue;
                if (!draw_mesh(&r,mesh)) return -1;
            }
            if(layer==1 && !draw_shadows(&r))return -1;
        }
        if (options && options->antialiasing) smooth_edges(&r);
        drawn = 1;
    }
    return drawn;
}

int lhx_scene_render(const LhxSceneFrame *frame,const uint8_t palette[768],
                     uint32_t *pixels,unsigned width,unsigned height)
{
    return lhx_scene_render_with_options(frame,palette,pixels,width,height,NULL);
}

int lhx_scene_emit_geometry(const LhxSceneFrame *frame,const uint8_t palette[768],
                            const LhxSceneRenderOptions *options,
                            const LhxSceneGeometrySink *sink,void *context)
{
    if (!palette || !sink || !sink->view || !sink->layer || !sink->triangle ||
        !sink->line || !sink->particle || !sink->end_view) return -1;
    unsigned faces=0,vertices=0,polygon=0;
    int valid=validate_frame(frame,&faces,&vertices,&polygon);
    if (valid<=0) return valid;
    if (!reserve_mesh(faces,vertices,polygon)) return -1;
    Render r={.options=options,.sink=sink,.sink_context=context};
    for (unsigned i=0;i<256;i++)
        r.colors[i]=0xff000000u|((palette[i*3]&63)*255/63<<16)|
                    ((palette[i*3+1]&63)*255/63<<8)|((palette[i*3+2]&63)*255/63);
    for (unsigned i=0;i<frame->view_count;i++) {
        LhxSceneView current=enhanced_view(frame->views+i,options);r.view=&current;
        if (r.view->focus<=0 || r.view->x0>=r.view->x1 || r.view->y0>=r.view->y1)
            continue;
        uint32_t sky=r.colors[r.view->sky_color&255];
        r.daylight=fmin(1,0.12+1.6*(0.30*((sky>>16)&255)+
                      0.59*((sky>>8)&255)+0.11*(sky&255))/255.0);
        int camera_inverse=r.view->has_camera && inverse(r.view->world_to_view,r.inverse_basis);
        r.relief=options && options->terrain_relief && camera_inverse && lhx_relief_ready(options->terrain);
        r.base_height=r.relief?lhx_relief_height(options->terrain,r.view->camera[0],r.view->camera[2]):0;
        if (!sink->view(context,r.view,r.colors,r.daylight)) return -1;
        for (int layer=0;layer<3;layer++) {
            if (!sink->layer(context,layer)) return -1;
            for (unsigned j=0;j<r.view->mesh_count;j++) {
                const LhxSceneMesh *mesh=r.view->meshes+j;
                if (mesh_layer(mesh,options)!=layer) continue;
                if (!draw_mesh(&r,mesh)) return -1;
                if (r.emit_failed) return -1;
            }
            if(layer==1 && !draw_shadows(&r))return -1;
        }
        if (!sink->end_view(context)) return -1;
    }
    return 1;
}
