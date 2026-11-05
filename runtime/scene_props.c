#include "scene_props.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MODELS=64,TYPES=32,VARIANTS=32,GROUPS=16 };
#define PROP_BUDGET ((size_t)128*1024*1024)
typedef struct {char name[14];unsigned count;int height;uint16_t models[VARIANTS];} Type;
struct LhxProps {unsigned models,types;LhxPropModel model[MODELS];Type type[TYPES];size_t bytes;};
static int bytes(FILE *f,void *out,size_t n){return fread(out,1,n,f)==n;}
static int word(FILE *f,uint32_t *out)
{
    unsigned char b[4];if(!bytes(f,b,4))return 0;
    *out=(uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;return 1;
}
static int number(FILE *f,float *out)
{uint32_t n;if(!word(f,&n))return 0;memcpy(out,&n,4);return isfinite(*out)&&fabsf(*out)<1e7f;}
static void free_model(LhxPropModel *m)
{
    for(unsigned i=0;i<m->count;i++){free(m->groups[i].vertices);lhx_texture_free(&m->groups[i].texture);}
    free(m->groups);memset(m,0,sizeof(*m));
}
void lhx_props_free(LhxProps *p)
{if(p){for(unsigned i=0;i<p->models;i++)free_model(p->model+i);free(p);}}
static int model(LhxProps *p,LhxPropModel *m,const char *path)
{
    FILE *f=fopen(path,"rb");if(!f)return 0;
    char magic[8];uint32_t count;size_t before=p->bytes;
    int ok=bytes(f,magic,8)&&!memcmp(magic,"LHXPROP1",8)&&word(f,&count)&&count>0&&count<=GROUPS;
    for(unsigned i=0;ok&&i<3;i++)ok=number(f,m->minimum+i);
    for(unsigned i=0;ok&&i<3;i++)ok=number(f,m->maximum+i)&&m->maximum[i]>m->minimum[i];
    if(ok){m->groups=calloc(count,sizeof(*m->groups));ok=m->groups!=NULL;if(ok)m->count=count;}
    for(unsigned i=0;ok&&i<m->count;i++){
        LhxPropGroup *g=m->groups+i;uint32_t n,w,h,two;
        ok=word(f,&n)&&word(f,&w)&&word(f,&h)&&word(f,&two)&&n>0&&n<=65535&&n%3==0&&
           w>0&&w<=1024&&h>0&&h<=1024&&two<=1;
        if(!ok)break;
        size_t reserve=(size_t)n*sizeof(*g->vertices)+(size_t)w*h*8;
        if(reserve>PROP_BUDGET-p->bytes){ok=0;break;}
        p->bytes+=reserve;g->count=n;g->one_sided=!two;
        g->vertices=malloc(n*sizeof(*g->vertices));g->texture.count=1;
        g->texture.levels[0]=(LhxTextureLevel){w,h,malloc((size_t)w*h*4)};
        ok=g->vertices&&g->texture.levels[0].pixels;
        for(unsigned j=0;ok&&j<n;j++){
            LhxPropVertex *v=g->vertices+j;
            ok=number(f,&v->position.x)&&number(f,&v->position.y)&&number(f,&v->position.z)&&number(f,&v->uv.u)&&number(f,&v->uv.v)&&
               v->uv.u>=0&&v->uv.u<=1&&v->uv.v>=0&&v->uv.v<=1;
            if(!ok)break;
            const float xyz[3]={v->position.x,v->position.y,v->position.z};
            for(unsigned k=0;ok&&k<3;k++)ok=xyz[k]>=m->minimum[k]-.001f&&xyz[k]<=m->maximum[k]+.001f;
        }
        unsigned char rgb[3];
        for(size_t j=0;ok&&j<(size_t)w*h;j++){
            ok=bytes(f,rgb,3);if(ok)g->texture.levels[0].pixels[j]=0xff000000u|(uint32_t)rgb[0]<<16|(uint32_t)rgb[1]<<8|rgb[2];
        }
        if(ok)ok=lhx_texture_mipmaps(&g->texture);
    }
    if(ok)ok=fgetc(f)==EOF&&!ferror(f);
    fclose(f);if(!ok){free_model(m);p->bytes=before;}return ok;
}
static int filename(const char s[64])
{
    const char *end=memchr(s,0,64);if(!end||end-s<5||strcmp(end-4,".lhp"))return 0;
    for(const char *p=s;p<end-4;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'))return 0;
    return 1;
}
LhxProps *lhx_props_load(const char *directory)
{
    if(!directory||strlen(directory)>480)return NULL;
    char path[600],magic[8],files[MODELS][64];snprintf(path,sizeof path,"%s/catalog.lhc",directory);
    FILE *f=fopen(path,"rb");if(!f){fprintf(stderr,"[scenery] no catalog in %s; keeping original models\n",directory);return NULL;}
    LhxProps *p=calloc(1,sizeof(*p));if(!p){fclose(f);return NULL;}
    uint32_t n,t;
    int ok=bytes(f,magic,8)&&!memcmp(magic,"LHXCAT1\0",8)&&word(f,&n)&&word(f,&t)&&n>0&&n<=MODELS&&t>0&&t<=TYPES;
    if(ok){p->models=n;p->types=t;}
    for(unsigned i=0;ok&&i<n;i++)ok=bytes(f,files[i],64)&&filename(files[i]);
    for(unsigned i=0;ok&&i<t;i++){
        Type *type=p->type+i;unsigned char flags[2];
        ok=bytes(f,type->name,14)&&memchr(type->name,0,14)&&bytes(f,flags,2)&&flags[0]<=1&&flags[1]>0&&flags[1]<=VARIANTS;
        if(!ok)break;
        type->height=flags[0];type->count=flags[1];
        for(unsigned j=0;ok&&j<type->count;j++){
            unsigned char b[2];ok=bytes(f,b,2);if(ok){type->models[j]=b[0]|(unsigned)b[1]<<8;ok=type->models[j]<n;}
        }
    }
    if(ok)ok=fgetc(f)==EOF&&!ferror(f);
    fclose(f);if(!ok){fprintf(stderr,"[scenery] invalid catalog; keeping original models\n");lhx_props_free(p);return NULL;}
    unsigned loaded=0;
    for(unsigned i=0;i<n;i++){
        snprintf(path,sizeof path,"%.480s/%.63s",directory,files[i]);loaded+=model(p,p->model+i,path);
    }
    fprintf(stderr,"[scenery] %u/%u static models loaded; unavailable classes keep originals\n",loaded,n);
    if(!loaded){lhx_props_free(p);return NULL;}return p;
}
const LhxPropModel *lhx_props_select(const LhxProps *p,const LhxSceneMesh *mesh,LhxPropFit *fit)
{
    if(!p||!mesh||!mesh->has_placement||!fit||!memchr(mesh->model_name,0,sizeof mesh->model_name))return NULL;
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<4;j++)if(!isfinite(mesh->model_to_view[i][j]))return NULL;
    const Type *type=NULL;
    for(unsigned i=0;i<p->types;i++)if(!strcmp(p->type[i].name,mesh->model_name)){type=p->type+i;break;}
    if(!type)return NULL;
    double size[3];
    for(unsigned i=0;i<3;i++){
        if(!isfinite(mesh->world_origin[i]))return NULL;
        size[i]=mesh->model_bounds[i*2+1]-mesh->model_bounds[i*2];
        if(!isfinite(size[i])||size[i]<=0)return NULL;
    }
    double rank[VARIANTS];unsigned order[VARIANTS];
    for(unsigned i=0;i<type->count;i++){
        const LhxPropModel *m=p->model+type->models[i];if(!m->count)return NULL;
        double x=m->maximum[0]-m->minimum[0],z=m->maximum[2]-m->minimum[2];
        rank[i]=fabs(log(fmax(x,z)/fmin(x,z))-log(fmax(size[0],size[2])/fmax(1,fmin(size[0],size[2]))));
        order[i]=i;
    }
    if(!type->height)for(unsigned i=1;i<type->count;i++){
        unsigned a=order[i],j=i;
        while(j&&rank[order[j-1]]>rank[a]){order[j]=order[j-1];j--;}order[j]=a;
    }
    double wx=floor(mesh->world_origin[0]+.5),wz=floor(mesh->world_origin[2]+.5);
    if(!isfinite(wx)||!isfinite(wz)||fabs(wx)>2147483647.0||fabs(wz)>2147483647.0)return NULL;
    uint32_t hash=(uint32_t)(int32_t)wx*73856093u^(uint32_t)(int32_t)wz*19349663u;
    uint32_t absolute=hash&0x80000000u?0u-hash:hash;
    unsigned variants=type->height?type->count:type->count<3?type->count:3;
    const LhxPropModel *m=p->model+type->models[order[absolute%variants]];
    double x=m->maximum[0]-m->minimum[0],y=m->maximum[1]-m->minimum[1],z=m->maximum[2]-m->minimum[2];
    fit->turn=!type->height&&((x>z)!=(size[0]>size[2]));
    double scale[3]={size[0]/(fit->turn?z:x),size[1]/y,size[2]/(fit->turn?x:z)};
    if(type->height)scale[0]=scale[2]=scale[1];
    else{double volume=cbrt(scale[0]*scale[1]*scale[2]);for(unsigned i=0;i<3;i++)scale[i]=sqrt(scale[i]*volume);}
    for(unsigned i=0;i<3;i++){
        if(!isfinite(scale[i])||scale[i]<=0||scale[i]>1e8)return NULL;
        fit->scale[i]=scale[i];fit->center[i]=i==1?m->minimum[i]:(m->minimum[i]+m->maximum[i])*.5f;
        fit->offset[i]=i==1?fmaxf(mesh->model_bounds[2],-mesh->world_origin[1]):(mesh->model_bounds[i*2]+mesh->model_bounds[i*2+1])*.5f;
    }
    return m;
}
LhxSceneVertex lhx_props_vertex(const LhxSceneMesh *mesh,const LhxPropFit *fit,LhxSceneVertex p)
{
    double local[3]={-(p.x-fit->center[0]),p.y-fit->center[1],p.z-fit->center[2]};
    if(fit->turn){double x=local[0];local[0]=local[2];local[2]=-x;}
    for(unsigned i=0;i<3;i++)local[i]=local[i]*fit->scale[i]+fit->offset[i];
    double v[3];for(unsigned i=0;i<3;i++){
        v[i]=mesh->model_to_view[i][3];for(unsigned j=0;j<3;j++)v[i]+=mesh->model_to_view[i][j]*local[j];
    }
    return (LhxSceneVertex){v[0],v[1],v[2]};
}
