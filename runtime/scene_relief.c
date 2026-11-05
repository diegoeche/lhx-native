#include "scene_relief.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t u32(FILE *f,int *ok){unsigned char b[4];if(fread(b,1,4,f)!=4){*ok=0;return 0;}
 return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;}
static float f32(FILE *f,int *ok){uint32_t b=u32(f,ok);float v;memcpy(&v,&b,4);if(!isfinite(v))*ok=0;return v;}
static double f64(FILE *f,int *ok){uint64_t lo=u32(f,ok),hi=u32(f,ok),b=lo|(hi<<32);double v;
 memcpy(&v,&b,8);if(!isfinite(v))*ok=0;return v;}
void lhx_relief_free(LhxTerrainGrid *g){if(!g)return;free(g->heights);free(g->vertices);free(g->indices);
 g->heights=NULL;g->vertices=NULL;g->indices=NULL;g->n=g->vertex_count=g->index_count=0;}
int lhx_relief_load(LhxTerrainGrid *g,const char *path){
 if(!g || !path)return 0;
 FILE *f=fopen(path,"rb");if(!f)return 0;int ok=1;char magic[8];
 LhxTerrainGrid r={0};
 if(fread(magic,1,8,f)!=8 || memcmp(magic,"LHXREL1\0",8))ok=0;
 r.source_n=u32(f,&ok);r.step=u32(f,&ok);r.n=u32(f,&ok);
 r.vertex_count=u32(f,&ok);r.index_count=u32(f,&ok);
 for(unsigned i=0;i<4;i++){double b=f64(f,&ok);if(fabs(b-g->bounds[i])>0.001)ok=0;}
 if(!ok || r.source_n<4 || r.source_n>1024 || r.step!=2 || r.n!=(r.source_n-1)/2+1 ||
    r.vertex_count<r.n*r.n || r.vertex_count>500000 || r.index_count>2100000 ||
    r.index_count%3)goto done;
 r.heights=malloc((size_t)r.n*r.n*sizeof(float));
 r.vertices=malloc((size_t)r.vertex_count*sizeof(*r.vertices));
 r.indices=malloc((size_t)r.index_count*sizeof(*r.indices));
 if(!r.heights || !r.vertices || !r.indices){ok=0;goto done;}
 for(unsigned i=0;i<r.n*r.n && ok;i++){
  r.heights[i]=f32(f,&ok);if(r.heights[i]<0 || r.heights[i]>32767)ok=0;
 }
 for(unsigned i=0;i<r.vertex_count && ok;i++){
  LhxTerrainVertex *v=r.vertices+i;
  v->x=f32(f,&ok);v->y=f32(f,&ok);v->z=f32(f,&ok);
  v->u=f32(f,&ok);v->v=f32(f,&ok);v->shade=f32(f,&ok);
  if(fabsf(v->x)>2000000 || fabsf(v->z)>2000000 || v->y < -10 || v->y>100000 ||
     fabsf(v->u)>10 || fabsf(v->v)>10 || v->shade<0.5 || v->shade>1.3)ok=0;
 }
 for(unsigned i=0;i<r.index_count && ok;i++){r.indices[i]=u32(f,&ok);if(r.indices[i]>=r.vertex_count)ok=0;}
 if(fgetc(f)!=EOF)ok=0;
done:
 fclose(f);
 if(!ok || !r.heights || !r.vertices || !r.indices){lhx_relief_free(&r);return 0;}
 static uint64_t revision;lhx_relief_free(g);
 g->heights=r.heights;g->vertices=r.vertices;g->indices=r.indices;
 g->source_n=r.source_n;g->step=r.step;g->n=r.n;g->vertex_count=r.vertex_count;g->index_count=r.index_count;
 g->relief_revision=++revision;return 1;
}
int lhx_relief_ready(const LhxTerrain *t){return t && t->near.n>=2 && t->far.n>=2 &&
 t->near.heights && t->far.heights && t->near.vertices && t->far.vertices;}
static int grid_height(const LhxTerrainGrid *g,double x,double z,double *result){
 if(!g || g->n<2 || !g->heights)return 0;
 double gx=((x-g->bounds[0])/(g->bounds[1]-g->bounds[0])*g->source_n-.5)/g->step;
 double gz=((g->bounds[3]-z)/(g->bounds[3]-g->bounds[2])*g->source_n-.5)/g->step;
 if(!(gx>=0 && gz>=0 && gx<=g->n-1 && gz<=g->n-1))return 0;
 unsigned i=fmin(floor(gx),g->n-2),j=fmin(floor(gz),g->n-2);double u=gx-i,v=gz-j;
 const float *h=g->heights+j*g->n+i;double a=h[0],b=h[1],c=h[g->n],d=h[g->n+1];
 *result=u+v<=1?a+(b-a)*u+(c-a)*v:d+(c-d)*(1-u)+(b-d)*(1-v);return 1;
}
double lhx_relief_height(const LhxTerrain *t,double x,double z){double h;
 if(!t || !isfinite(x) || !isfinite(z))return 0;
 if(grid_height(&t->near,x,z,&h) || grid_height(&t->far,x,z,&h))return h;
 return 0;
}
LhxSceneVertex lhx_relief_view(const LhxSceneView *v,const LhxTerrain *t,double x,double y,double z){
 double p[3]={x-v->camera[0],y-v->camera[1]-lhx_relief_height(t,v->camera[0],v->camera[2]),z-v->camera[2]};
 double out[3];
 for(unsigned i=0;i<3;i++)out[i]=v->world_to_view[i][0]*p[0]+v->world_to_view[i][1]*p[1]+v->world_to_view[i][2]*p[2];
 return (LhxSceneVertex){out[0],out[1],out[2]};
}
