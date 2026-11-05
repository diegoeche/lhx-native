/* Rendering-only inventory upstream of the original distance/list limits.
 *
 * Original image 140CE visits world cells, 1705E follows object+4 siblings
 * and flag20 child lists at+18. 16E57 then rejects distant models and 16B07
 * reduces translations to model-unit signed16 values. This reader keeps the
 * loaded inventory/state intact and transforms source geometry in doubles.
 * No generated instructions, original draw lists, or spawning are involved.
 */
#include "scene_world.h"
#include "model_transform.h"
#include <math.h>
#include <string.h>

typedef struct { uint16_t segment; unsigned offset; } Address;
typedef struct {
    CPU *cpu;
    const LhxSceneView *view;
    LhxSceneWorldAlloc allocate;
    LhxSceneWorldFree release;
    LhxSceneWorldEmit emit;
    void *context;
    LhxSceneWorldStats stats;
    double camera[3], rotation[3][3];
} World;

static int readable(CPU *cpu, Address a, unsigned bytes)
{
    return cpu->mem && a.offset <= 65536 && bytes <= 65536-a.offset &&
           ((uint32_t)a.segment << 4) + a.offset + bytes <= MEM_SIZE;
}
static unsigned byte(World *w, Address a, unsigned add)
{ return mem_read8(w->cpu,a.segment,(uint16_t)(a.offset+add)); }
static unsigned word(World *w, Address a, unsigned add)
{ return mem_read16(w->cpu,a.segment,(uint16_t)(a.offset+add)); }
static int32_t integer(World *w, Address a, unsigned add)
{
    uint32_t value=word(w,a,add) | (uint32_t)word(w,a,add+2)<<16;
    return (int32_t)value;
}
static Address data(World *w, unsigned offset)
{ return (Address){w->cpu->ds,offset}; }

/* Far model pointers retain their relocation base even when their serialized
 * LOD block is not the model currently copied into the engine's shared slot.
 * Original 167B7 performs that copy/relocation. Read the retained block directly. */
static int model_address(World *w, Address record, unsigned pointer,
                         unsigned size, Address *out)
{
    unsigned segment=word(w,record,0x22);
    if (!segment) *out=data(w,pointer);
    else {
        unsigned relative=(uint16_t)(pointer-word(w,record,0x24));
        unsigned length=word(w,record,0x26);
        if (relative>length || size>length-relative) return 0;
        *out=(Address){(uint16_t)segment,word(w,record,0x20)+relative};
    }
    return readable(w->cpu,*out,size);
}

static void matrix_product(double out[3][3], double a[3][3], double b[3][3])
{
    double result[3][3];
    for (unsigned row=0;row<3;row++) for (unsigned col=0;col<3;col++) {
        result[row][col]=0;
        for (unsigned k=0;k<3;k++) result[row][col]+=a[row][k]*b[k][col];
    }
    memcpy(out,result,sizeof(result));
}

/* 14872 starts with Rz(-roll), left-multiplies Rx(-pitch), then
 * Ry(-heading) through 1B1DF. 18F32 applies that placement directly, before
 * the camera basis. Angles are 2880 units/turn. These are ideal native
 * rotations; the camera matrix remains the exact authored Q14 matrix. */
static void placement(World *w, Address object, unsigned flags, double out[3][3])
{
    memset(out,0,9*sizeof(double));
    out[0][0]=out[1][1]=out[2][2]=1;
    if (flags&2) return;
    double turn=6.283185307179586476925286766559/2880.0;
    double h=(int16_t)word(w,object,0x12)*turn;
    double p=(int16_t)word(w,object,0x14)*turn;
    double r=(int16_t)word(w,object,0x16)*turn;
    double ch=cos(h),sh=sin(h),cp=cos(p),sp=sin(p),cr=cos(r),sr=sin(r);
    double rz[3][3]={{cr,sr,0},{-sr,cr,0},{0,0,1}};
    double rx[3][3]={{1,0,0},{0,cp,sp},{0,-sp,cp}};
    double ry[3][3]={{ch,0,-sh},{0,1,0},{sh,0,ch}};
    matrix_product(out,rx,rz);
    matrix_product(out,ry,out);
}

static void apply(double matrix[3][3], const double point[3], double out[3])
{
    for (unsigned row=0;row<3;row++) {
        out[row]=0;
        for (unsigned col=0;col<3;col++) out[row]+=matrix[row][col]*point[col];
    }
}

void lhx_scene_world_environment(CPU *cpu, LhxSceneView *view)
{
    if (view) view->has_camera=0;
    if (!cpu || !view) return;
    World w={.cpu=cpu};
    Address basis=data(&w,0xf03b),camera=data(&w,0xf01b);
    if (!readable(cpu,basis,18) || !readable(cpu,camera,12)) return;
    double norm=0;
    for (unsigned i=0;i<3;i++) {
        view->camera[i]=integer(&w,camera,i*4)/256.0;
        for (unsigned j=0;j<3;j++) {
            double v=(int16_t)word(&w,basis,(i*3+j)*2)/16384.0;
            view->world_to_view[i][j]=v;
            norm+=v*v;
        }
    }
    double (*m)[3]=view->world_to_view;
    double det=m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])-
               m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])+
               m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
    if (fabs(det)<0.01 || norm>12) return;
    Address world=data(&w,word(&w,data(&w,0xf019),0));
    if (!readable(cpu,world,2)) return;
    view->sky_color=byte(&w,world,1);
    view->ground_color=byte(&w,world,0);
    /* Auxiliary target cameras retain their original backgrounds, but their
     * current basis/sky also lights replacement geometry consistently. */
    view->has_camera=view->x0==0 && view->x1==320;
}

void lhx_scene_world_surface(CPU *cpu, unsigned record_offset,
                             uint16_t segment, unsigned object_offset,
                             LhxSceneMesh *mesh)
{
    if (mesh) {mesh->runway=0;mesh->has_placement=0;memset(mesh->model_name,0,sizeof mesh->model_name);}
    if (!cpu || !mesh) return;
    World w={.cpu=cpu};
    Address record=data(&w,record_offset),object={segment,object_offset};
    if (!readable(cpu,record,0x44) || !readable(cpu,object,0x18)) return;
    unsigned name=word(&w,record,0x1e);
    unsigned length=0;
    for (;length<sizeof mesh->model_name;length++) {
        Address a;
        if (!name || !model_address(&w,record,name+length,1,&a)) return;
        mesh->model_name[length]=(char)byte(&w,a,0);
        if(!mesh->model_name[length])break;
    }
    if(length==sizeof mesh->model_name){mesh->model_name[0]=0;return;}
    double rotation[3][3]; placement(&w,object,word(&w,object,2),rotation);
    double basis[3][3],origin[3];
    mesh->has_placement=1;
    for(unsigned i=0;i<3;i++){
        mesh->world_origin[i]=integer(&w,object,6+i*4)/256.0;
        origin[i]=mesh->world_origin[i]-integer(&w,data(&w,0xf01b),i*4)/256.0;
        mesh->model_bounds[i*2]=integer(&w,record,0x2c+i*8)/256.0;
        mesh->model_bounds[i*2+1]=integer(&w,record,0x30+i*8)/256.0;
        if(mesh->model_bounds[i*2]>mesh->model_bounds[i*2+1])mesh->has_placement=0;
        for(unsigned j=0;j<3;j++)basis[i][j]=(int16_t)word(&w,data(&w,0xf03b),2*(i*3+j))/16384.0;
    }
    double combined[3][3];matrix_product(combined,basis,rotation);
    double determinant=basis[0][0]*(basis[1][1]*basis[2][2]-basis[1][2]*basis[2][1])-
        basis[0][1]*(basis[1][0]*basis[2][2]-basis[1][2]*basis[2][0])+
        basis[0][2]*(basis[1][0]*basis[2][1]-basis[1][1]*basis[2][0]);
    if(fabs(determinant)<.01)mesh->has_placement=0;
    for(unsigned i=0;i<3;i++){
        double offset=0;for(unsigned j=0;j<3;j++){mesh->model_to_view[i][j]=combined[i][j];offset+=basis[i][j]*origin[j];}
        mesh->model_to_view[i][3]=offset;
    }
    if(mesh->layer!=1 || strcmp(mesh->model_name,"runway"))return;
    double bounds[4]={integer(&w,record,0x2c)/256.0,
        integer(&w,record,0x30)/256.0,integer(&w,record,0x3c)/256.0,
        integer(&w,record,0x40)/256.0};
    if (bounds[0]>=bounds[1] || bounds[2]>=bounds[3]) return;
    for (unsigned i=0;i<4;i++) {
        double local[3]={bounds[i==1 || i==2],0,bounds[2+(i>=2)]};
        double position[3],point[3]; apply(rotation,local,position);
        for (unsigned k=0;k<3;k++)
            position[k]+=integer(&w,object,6+k*4)/256.0-
                         integer(&w,data(&w,0xf01b),k*4)/256.0;
        for (unsigned k=0;k<3;k++) {
            point[k]=0;
            for (unsigned j=0;j<3;j++) point[k]+=
                (int16_t)word(&w,data(&w,0xf03b),2*(k*3+j))/16384.0*position[j];
        }
        mesh->runway_corners[i]=(LhxSceneVertex){point[0],point[1],point[2]};
    }
    mesh->runway=1;
}

static int outside(World *w, const double center[3], double radius)
{
    const LhxSceneView *v=w->view;
    double focus=v->focus, vertical=focus*15.0/16.0;
    double planes[5][3]={{0,0,1},{focus,0,v->cx-v->x0},
        {-focus,0,v->x1-v->cx},{0,-vertical,v->cy-v->y0},
        {0,vertical,v->y1-v->cy}};
    for (unsigned p=0;p<5;p++) {
        double distance=0,norm=0;
        for (unsigned k=0;k<3;k++) distance+=planes[p][k]*center[k];
        for (unsigned col=0;col<3;col++) {
            double value=0;
            for (unsigned row=0;row<3;row++)
                value+=planes[p][row]*w->rotation[row][col];
            norm+=value*value;
        }
        if (distance+radius*sqrt(norm)<0) return 1;
    }
    return 0;
}

static void dispose(World *w, LhxSceneMesh *mesh)
{
    for (unsigned i=0;i<mesh->primitive_count;i++)
        w->release(w->context,mesh->primitives[i].indices);
    w->release(w->context,mesh->primitives);
    w->release(w->context,mesh->vertices);
    memset(mesh,0,sizeof(*mesh));
}

/* These three authored callbacks select vehicle/damage variants by enabling
 * leaves in the model's own BSP data. Evaluate into this host mesh only.
 * Original image 2DE16 (BRDM), 2DE46 (ZSU23), 2DE98/2E27A (Hind). */
static unsigned leaf_mode(World *w, Address object, unsigned callback,
                           unsigned lod, unsigned node, unsigned original)
{
    unsigned state=word(w,object,0x18);
    if (callback==0x466 && (node==0xbac2 || node==0xbc4c))
        return state==0x1bbe?3:1;
    if (callback==0x496) {
        if (node==0xc211 || node==0xc078) return state==0x1c5a?3:1;
        if (node==0xc21b || node==0xc082) return state==0x1c5a?1:3;
    }
    if (callback==0x4e8 && lod==0x6771) {
        if (node==0x6c47)
            return integer(w,object,0x0a)<0x6400 && byte(w,object,0x24)!=7?3:1;
        if (node==0x6bd1) return state==0x1a74?3:1;
        if (node==0x6bbf) return state==0x1a74?1:3;
        if (node==0x6c1d || node==0x6bfd) return state==0x1abc?3:1;
    }
    return original;
}

static int active_primitives(World *w, Address record, Address object,
                              Address lod, unsigned lod_pointer,
                              unsigned callback, const unsigned *pointers,
                              unsigned count, uint8_t *active)
{
    unsigned root=word(w,lod,10);
    if (!root) { memset(active,1,count); return 1; }
    uint16_t stack[512]; unsigned depth=1;
    uint8_t visited[65536/8]={0};
    stack[0]=(uint16_t)root;
    while (depth) {
        unsigned node=stack[--depth],mask=1u<<(node&7);
        if (visited[node/8]&mask) continue;
        visited[node/8]|=(uint8_t)mask;
        Address a;
        if (!model_address(w,record,node,2,&a)) return 0;
        unsigned mode=leaf_mode(w,object,callback,lod_pointer,node,byte(w,a,0));
        if (mode&1) {
            if (!(mode&2)) continue;
            unsigned length=byte(w,a,1);
            if (!model_address(w,record,node,2+2*length,&a)) return 0;
            for (unsigned j=0;j<length;j++) {
                unsigned primitive=word(w,a,2+2*j),k;
                for (k=0;k<count;k++) if (pointers[k]==primitive) break;
                if (k==count) return 0;
                active[k]=1;
            }
        } else {
            if (depth>510 || !model_address(w,record,node,7,&a)) return 0;
            stack[depth++]=(uint16_t)word(w,a,3);
            stack[depth++]=(uint16_t)word(w,a,5);
        }
    }
    return 1;
}

/* Zero means unsupported, one built, minus one allocation/data failure. */
static int build_lod(World *w, Address record, Address object, unsigned flags,
                     unsigned pointer, const double delta[3], double distance,
                     LhxSceneMesh *mesh)
{
    Address lod;
    if (!pointer || !model_address(w,record,pointer,14,&lod)) return 0;
    unsigned mode=byte(w,lod,13), count=byte(w,lod,0), vertices=byte(w,lod,1);
    if (!vertices || !count || (mode&1)) return 0;
    unsigned callback=0;
    if (word(w,lod,4)) {
        callback=word(w,lod,2);
        if (word(w,lod,4)!=(uint16_t)(g_load_seg+0x2d9b) ||
            (callback!=0x466 && callback!=0x496 && callback!=0x4e8) ||
            !readable(w->cpu,object,callback==0x4e8?0x25:0x1a)) return 0;
    }
    int32_t xyz[255*3];
    if (mode&2) {
        if (!lhx_model_source_vertices((uint16_t)word(w,lod,8),
                (uint16_t)word(w,lod,6),xyz,vertices)) return 0;
    } else {
        unsigned stride=(mode&4)?3:6;
        Address source={(uint16_t)word(w,lod,8),word(w,lod,6)};
        if (!readable(w->cpu,source,vertices*stride)) return -1;
        for (unsigned i=0;i<vertices;i++) for (unsigned k=0;k<3;k++)
            xyz[i*3+k]=(mode&4)?(int8_t)byte(w,source,i*3+k):
                                      (int16_t)word(w,source,i*6+k*2);
    }
    int shift=(int8_t)byte(w,record,12);
    if (shift < -30 || shift > 30) return -1;
    double unit=ldexp(1.0,shift), object_rotation[3][3],combined[3][3];
    placement(w,object,flags,object_rotation);
    matrix_product(combined,w->rotation,object_rotation);
    double origin[3]; apply(w->rotation,delta,origin);
    memset(mesh,0,sizeof(*mesh));
    mesh->object_id=seg_off(object.segment,(uint16_t)object.offset);
    mesh->object_distance=(float)distance;
    mesh->vertex_count=vertices;
    mesh->vertices=w->allocate(w->context,vertices,sizeof(*mesh->vertices));
    mesh->primitives=w->allocate(w->context,count,sizeof(*mesh->primitives));
    if (!mesh->vertices || !mesh->primitives) { dispose(w,mesh); return -1; }
    memset(mesh->primitives,0,count*sizeof(*mesh->primitives));
    for (unsigned i=0;i<vertices;i++) {
        double local[3]={xyz[i*3]*unit,xyz[i*3+1]*unit,xyz[i*3+2]*unit},point[3];
        apply(combined,local,point);
        mesh->vertices[i]=(LhxSceneVertex){(float)(origin[0]+point[0]),
            (float)(origin[1]+point[1]),(float)(origin[2]+point[2])};
    }
    unsigned pointers[255],cursor=pointer+14;
    uint8_t active[255]={0};
    /* Walk the authored primitive list once to index its data records; the BSP
     * leaves determine which variants are enabled for this particular object. */
    for (unsigned i=0;i<count;i++) {
        Address a;
        if (cursor>65535 || !model_address(w,record,cursor,6,&a)) goto malformed;
        pointers[i]=cursor;
        unsigned kind=byte(w,a,0)&7,bytes;
        if (kind==0) {
            if (!model_address(w,record,cursor,7,&a)) goto malformed;
            bytes=7+byte(w,a,6);
        } else if (kind==1) bytes=7;
        else if (kind==2) bytes=6;
        else if (kind==3) bytes=8;
        else if (kind==4) { dispose(w,mesh); return 0; }
        else goto malformed;
        if (!model_address(w,record,cursor,bytes,&a)) goto malformed;
        cursor+=bytes;
    }
    if (!active_primitives(w,record,object,lod,pointer,callback,pointers,count,active))
        goto malformed;
    int points_only=1;
    for (unsigned i=0;i<count;i++) {
        if (!active[i]) continue;
        cursor=pointers[i];
        Address a;
        if (cursor>65535 || !model_address(w,record,cursor,6,&a)) goto malformed;
        unsigned type=byte(w,a,0),kind=type&7,n,at,bytes;
        if (kind==0) {
            if (!model_address(w,record,cursor,7,&a)) goto malformed;
            n=byte(w,a,6);at=7;bytes=7+n;
            if (n<3) goto malformed;
        } else if (kind==1) { n=2;at=5;bytes=7; }
        else if (kind==2) { n=1;at=5;bytes=6; }
        else if (kind==3) { n=1;at=7;bytes=8; }
        else if (kind==4) { dispose(w,mesh); return 0; } /* Original 2D callbacks. */
        else goto malformed;
        if (!model_address(w,record,cursor,bytes,&a)) goto malformed;
        LhxScenePrimitive *p=mesh->primitives+mesh->primitive_count++;
        *p=(LhxScenePrimitive){.kind=kind,.count=n,.color=byte(w,a,3),
            .one_sided=kind==0&&(type&0x10),.stipple=kind==0&&byte(w,a,4)!=255};
        if (callback==0x496 && cursor+3==0xc1b9)
            p->color=word(w,object,0x18)==0x1c5a?12:9;
        p->indices=w->allocate(w->context,n,sizeof(*p->indices));
        if (!p->indices) goto malformed;
        for (unsigned j=0;j<n;j++) {
            unsigned index=byte(w,a,at+j);
            if (index>=vertices) goto malformed;
            p->indices[j]=(uint16_t)index;
        }
        if (kind==3) p->radius=(float)(word(w,a,5)*unit);
        if (kind<2) points_only=0;
    }
    mesh->layer=points_only?0:
        integer(w,record,0x34)==integer(w,record,0x38)&&integer(w,object,10)==0?1:2;
    lhx_scene_world_surface(w->cpu,record.offset,object.segment,object.offset,mesh);
    return 1;
malformed:
    dispose(w,mesh);
    return -1;
}

static int visit(World *w, Address object, unsigned flags)
{
    /* Authored 16E57:716E excludes this class for the current camera.
     * This is a semantic visibility flag, separate from range/cache limits. */
    if (byte(w,data(w,0x06ef),0) && (flags&4)) {
        w->stats.unsupported++;
        return 1;
    }
    Address record=data(w,word(w,object,0));
    if (!readable(w->cpu,record,0x44)) return 0;
    if (word(w,record,0x1a)) { w->stats.unsupported++; return 1; }
    if (!(flags&2) && !readable(w->cpu,object,0x18)) return 0;
    double delta[3], center[3], distance=0;
    for (unsigned k=0;k<3;k++) {
        delta[k]=integer(w,object,6+k*4)/256.0-w->camera[k];
        distance+=delta[k]*delta[k];
    }
    distance=sqrt(distance);
    apply(w->rotation,delta,center);
    /* Model+2 bounds local coordinates; a cube around that radius safely
     * encloses every rotated point, without trusting the original far limit. */
    int shift=(int8_t)byte(w,record,12);
    if (shift < -30 || shift > 30) return 0;
    double radius=word(w,record,2)*ldexp(1.0,shift)*1.7320508075688773+1;
    double box_radius=0;
    for (unsigned axis=0;axis<3;axis++) {
        double low=fabs(integer(w,record,0x2c+axis*8)/256.0);
        double high=fabs(integer(w,record,0x30+axis*8)/256.0);
        double bound=low>high?low:high;
        box_radius+=bound*bound;
    }
    box_radius=sqrt(box_radius)+1;
    if (box_radius>radius) radius=box_radius;
    if (outside(w,center,radius)) { w->stats.frustum_rejected++; return 1; }
    LhxSceneMesh mesh;
    unsigned high=word(w,record,0x14), low=word(w,record,0x12);
    int built=build_lod(w,record,object,flags,high,delta,distance,&mesh);
    if (!built && low!=high)
        built=build_lod(w,record,object,flags,low,delta,distance,&mesh);
    if (built<0) return 0;
    if (!built) { w->stats.unsupported++; return 1; }
    unsigned vertices=mesh.vertex_count,primitives=mesh.primitive_count;
    if (!w->emit(w->context,mesh.object_id,&mesh)) { dispose(w,&mesh); return 0; }
    w->stats.emitted++;
    w->stats.vertices+=vertices;
    w->stats.primitives+=primitives;
    return 1;
}

int lhx_scene_world_capture(CPU *cpu, const LhxSceneView *view,
                            LhxSceneWorldAlloc allocate,
                            LhxSceneWorldFree release,
                            LhxSceneWorldEmit emit, void *context,
                            LhxSceneWorldStats *stats)
{
    World w={.cpu=cpu,.view=view,.allocate=allocate,.release=release,
             .emit=emit,.context=context};
    if (stats) memset(stats,0,sizeof(*stats));
    if (!cpu || !cpu->mem || !view || !allocate || !release || !emit ||
        !isfinite(view->focus) || view->focus<=0 || view->x1<=view->x0 ||
        view->y1<=view->y0) return 0;
    Address world=data(&w,word(&w,data(&w,0xf019),0));
    if (!readable(cpu,world,16)) return 0;
    uint16_t segment=(uint16_t)word(&w,world,10);
    unsigned nx=word(&w,world,4),nz=word(&w,world,6);
    if (!nx || !nz || nx>16384 || nz>16384/nx) return 0;
    Address grid={(uint16_t)word(&w,world,14),word(&w,world,8)};
    if (!readable(cpu,grid,nx*nz*4)) return 0;
    for (unsigned k=0;k<3;k++) {
        w.camera[k]=integer(&w,data(&w,0xf01b),4*k)/256.0;
        for (unsigned j=0;j<3;j++)
            w.rotation[k][j]=(int16_t)word(&w,data(&w,0xf03b),2*(k*3+j))/16384.0;
    }
    uint8_t seen[65536/8]={0};
    uint16_t heads[16385];
    unsigned count=0;
    heads[count++]=(uint16_t)word(&w,world,12);
    for (unsigned i=0;i<nx*nz;i++) heads[count++]=(uint16_t)word(&w,grid,4*i);
    int success=1;
    while (count && success) {
        unsigned offset=heads[--count];
        while (offset) {
            unsigned mask=1u<<(offset&7);
            if (seen[offset/8]&mask) break; /* Duplicate cell/group references. */
            seen[offset/8]|=(uint8_t)mask;
            Address object={segment,offset};
            if (!readable(cpu,object,0x12)) { success=0; break; }
            unsigned flags=word(&w,object,2);
            w.stats.considered++;
            if (!(flags&1)) w.stats.inactive++;
            else if (byte(&w,data(&w,0x06ef),0) && (flags&4))
                w.stats.unsupported++;
            else if (flags&0x20) {
                w.stats.groups++;
                if (!readable(cpu,object,0x1a) || count==16385) { success=0; break; }
                Address group_record=data(&w,word(&w,object,0));
                if (!readable(cpu,group_record,0x44)) { success=0; break; }
                if (word(&w,group_record,0x1a)) w.stats.unsupported++;
                else heads[count++]=(uint16_t)word(&w,object,0x18);
            } else if (!visit(&w,object,flags)) { success=0; break; }
            offset=word(&w,object,4);
        }
    }
    if (stats) *stats=w.stats;
    return success;
}
