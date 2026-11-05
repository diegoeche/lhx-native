#include "scene_texture.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lhx_texture_free(LhxTexture *t)
{
    if (!t) return;
    for (unsigned i=0;i<t->count && i<13;i++) free(t->levels[i].pixels);
    memset(t,0,sizeof(*t));
}

static uint32_t blend(uint32_t a,uint32_t b,double f)
{
    unsigned weight=(unsigned)(f*256+0.5),other=256-weight;
    uint32_t rb=(((a&0xff00ff)*other+(b&0xff00ff)*weight+0x800080)>>8)&0xff00ff;
    uint32_t g=(((a&0xff00)*other+(b&0xff00)*weight+0x8000)>>8)&0xff00;
    return 0xff000000u|rb|g;
}

int lhx_texture_mipmaps(LhxTexture *t)
{
    static uint64_t next_revision;
    if (!t || t->count!=1 || !t->levels[0].pixels ||
        !t->levels[0].width || !t->levels[0].height ||
        t->levels[0].width>4096 || t->levels[0].height>4096) return 0;
    t->revision=++next_revision;
    while (t->count<13) {
        const LhxTextureLevel *a=&t->levels[t->count-1];
        if (a->width==1 && a->height==1) return 1;
        LhxTextureLevel *b=&t->levels[t->count];
        b->width=(a->width+1)/2; b->height=(a->height+1)/2;
        b->pixels=malloc((size_t)b->width*b->height*sizeof(*b->pixels));
        if (!b->pixels) return 0;
        t->count++;
        for (unsigned y=0;y<b->height;y++) for (unsigned x=0;x<b->width;x++) {
            unsigned x0=x*2,y0=y*2,x1=x0+1<a->width?x0+1:x0;
            unsigned y1=y0+1<a->height?y0+1:y0;
            uint32_t color=0xff000000u;
            for (unsigned s=0;s<=16;s+=8) {
                unsigned sum=((a->pixels[y0*a->width+x0]>>s)&255)+
                    ((a->pixels[y0*a->width+x1]>>s)&255)+
                    ((a->pixels[y1*a->width+x0]>>s)&255)+
                    ((a->pixels[y1*a->width+x1]>>s)&255);
                color|=((sum+2)/4)<<s;
            }
            b->pixels[y*b->width+x]=color;
        }
    }
    return 1;
}

static uint32_t sample_level(const LhxTextureLevel *l,double u,double v)
{
    if (!l->pixels || !l->width || !l->height) return 0;
    if (l->width==1 && l->height==1) return l->pixels[0];
    double x=u*l->width-0.5,y=v*l->height-0.5;
    x=x<0?0:x>l->width-1?l->width-1:x;
    y=y<0?0:y>l->height-1?l->height-1:y;
    unsigned x0=(unsigned)x,y0=(unsigned)y;
    unsigned x1=x0+1<l->width?x0+1:x0,y1=y0+1<l->height?y0+1:y0;
    return blend(blend(l->pixels[y0*l->width+x0],l->pixels[y0*l->width+x1],x-x0),
                 blend(l->pixels[y1*l->width+x0],l->pixels[y1*l->width+x1],x-x0),y-y0);
}

uint32_t lhx_texture_sample(const LhxTexture *t,double u,double v,double footprint)
{
    if (!t || !t->count || t->count>13 || !isfinite(u) || !isfinite(v)) return 0;
    if (t->count==1 || footprint<=1) return sample_level(&t->levels[0],u,v);
    double lod=isfinite(footprint)?log2(footprint):(double)t->count-1;
    if (lod>=t->count-1) return sample_level(&t->levels[t->count-1],u,v);
    unsigned a=(unsigned)lod,b=a+1<t->count?a+1:a;
    uint32_t first=sample_level(&t->levels[a],u,v);
    if (lod==a) return first;
    return blend(first,sample_level(&t->levels[b],u,v),lod-a);
}

/* Deterministic concrete and markings, adapted from reference Runways.concrete.
 * Normalized coordinates keep one small texture reusable for every runway. */
int lhx_texture_runway(LhxTexture *t)
{
    LhxTexture result={.count=1,.levels={{256,2048,NULL}}};
    uint32_t *pixels=malloc(256u*2048*sizeof(*pixels));
    if (!pixels) return 0;
    result.levels[0].pixels=pixels;
    for (unsigned y=0;y<2048;y++) for (unsigned x=0;x<256;x++) {
        unsigned hash=(x/20)*73856093u^(y/20)*19349663u;
        int c=137+(int)(hash%15)-7;
        if (x%20==0 || y%20==0) c-=22;
        c+=(int)((x*17u+y*31u)%7)-3;
        double u=x/256.0,v=y/2048.0;
        double edge=fmin(u,1-u),end=fmin(v,1-v);
        int white=(edge>0.025 && edge<0.04) ||
            (end>0.025 && end<0.09 && edge>0.10 && fabs(u-0.5)>0.08 && x%25<11) ||
            (fabs(u-0.5)<0.012 && end>0.15 && fabs(v-0.5)>0.075 && y%100<55);
        double dx=(u-0.5)*2,dy=(v-0.5)*12,r=hypot(dx,dy);
        white|=(r>0.61 && r<0.67) ||
            (fabs(dy)<0.40 && fabs(fabs(dx)-0.25)<0.04) ||
            (fabs(dy)<0.04 && fabs(dx)<0.29);
        if (white) c=237;
        pixels[y*256+x]=0xff000000u|(unsigned)(c+3)<<16|(unsigned)(c+3)<<8|(unsigned)c;
    }
    if (!lhx_texture_mipmaps(&result)) { lhx_texture_free(&result); return 0; }
    lhx_texture_free(t); *t=result; return 1;
}

static unsigned runway_random(unsigned *seed){*seed=*seed*1103515245u+12345u;return (*seed&0x7fffffffu);}
static void runway_rect(uint32_t *p,int x0,int y0,int x1,int y1,uint32_t color,double alpha){
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>256)x1=256;
    if(y1>2048)y1=2048;
    for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++)p[y*256+x]=blend(p[y*256+x],color,alpha);
}
int lhx_texture_runway_detail(LhxTexture *t,double length,double width,int bearing)
{
    if(!isfinite(length) || !isfinite(width) || length<1 || width<1 || length>100000 || width>10000)return 0;
    LhxTexture result={.count=1,.levels={{256,2048,NULL}}};
    uint32_t *pixels=malloc(256u*2048*sizeof(*pixels));if(!pixels)return 0;result.levels[0].pixels=pixels;
    double px=256/width,py=2048/length;unsigned seed=12345;
    for(unsigned y=0;y<2048;y++)for(unsigned x=0;x<256;x++){
        unsigned cx=(unsigned)(x/px/50),cz=(unsigned)(y/py/50),hash=cx*73856093u^cz*19349663u;
        int value=128+(int)(hash%19)-9+14;
        if(fmod(x/px,50)<1/px || fmod(y/py,50)<1/py)value-=24;
        value+=(int)((x*17+y*31)%9)-4;
        pixels[y*256+x]=0xff000000u|(unsigned)value<<16|(unsigned)value<<8|(unsigned)(value-5);
    }
    #define WHITE(x,z,w,l) runway_rect(pixels,(int)((x)*px),(int)((z)*py),(int)(((x)+(w))*px+1),(int)(((z)+(l))*py+1),0xfff5f5f0,.92)
    for(unsigned end=0;end<2;end++)for(unsigned i=0;i<70;i++){
        double r=runway_random(&seed)/2147483647.,s=runway_random(&seed)/2147483647.;
        double z=end?length-700-r*900:700+r*900,x=128+(s-.5)*64;
        unsigned w=2+runway_random(&seed)%3;double l=40+runway_random(&seed)%160;
        runway_rect(pixels,(int)x,(int)(z*py),(int)x+w,(int)((z+l)*py),0xff191919,.08+(runway_random(&seed)%150)/1000.);
    }
    WHITE(8,0,6,length);WHITE(width-14,0,6,length);
    static const unsigned char digits[10][7]={
        {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}};
    for(unsigned end=0;end<2;end++){
        for(unsigned i=0;i<8;i++){
            double x=30+i*(width-60)/8;if(fabs(x+12-width/2)>30){double z=end?length-340:40;WHITE(x,z,24,300);}
        }
        const double marks[3][3]={{900,250,40},{1500,180,24},{2000,180,24}};
        for(unsigned i=0;i<3;i++){
            double z=end?length-marks[i][0]-marks[i][1]:marks[i][0];
            WHITE(width/2-70-marks[i][2],z,marks[i][2],marks[i][1]);WHITE(width/2+70,z,marks[i][2],marks[i][1]);
        }
        int number=(int)floor((((bearing+(end?180:0))%360+360)%360)/10.+.5);if(!number)number=36;
        for(unsigned digit=0;digit<2;digit++)for(unsigned row=0;row<7;row++)for(unsigned col=0;col<5;col++){
            unsigned value=digit?number%10:number/10;if(!(digits[value][row]&(1u<<(4-col))))continue;
            double x=width/2-90+digit*100+col*16,z=420+row*(170./7);
            if(!end){x=width-x-16;z=590-(z-420)-170./7;}else z=length-590+(z-420);
            WHITE(x,z,16,170./7);
        }
    }
    for(double z=760;z<length-760;z+=400)if(fabs(z+120-length/2)>260){WHITE(width/2-3,z,6,240);}
    for(unsigned y=0;y<2048;y++)for(unsigned x=0;x<256;x++){
        double dx=x/px-width/2,dz=y/py-length/2,r=hypot(dx,dz);
        if((r>116 && r<124) || (fabs(dz)<70 && fabs(fabs(dx)-46)<9) || (fabs(dz)<9 && fabs(dx)<55))
            pixels[y*256+x]=blend(pixels[y*256+x],0xfff5f5f0,.92);
    }
    #undef WHITE
    if(!lhx_texture_mipmaps(&result)){lhx_texture_free(&result);return 0;}
    lhx_texture_free(t);*t=result;return 1;
}

static int dimensions(const char *line,unsigned *width,unsigned *height)
{
    unsigned values[2];
    for (unsigned i=0;i<2;i++) {
        while (isspace((unsigned char)*line)) line++;
        if (*line<'0' || *line>'9') return 0;
        char *end;errno=0;
        unsigned long value=strtoul(line,&end,10);
        if (errno || end==line || !value || value>2048) return 0;
        values[i]=(unsigned)value;line=end;
        if (i==0 && !isspace((unsigned char)*line)) return 0;
    }
    while (isspace((unsigned char)*line)) line++;
    if (*line) return 0;
    *width=values[0];*height=values[1];return 1;
}

int lhx_texture_load(LhxTerrainGrid *grid,const char *path)
{
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    LhxTerrainGrid result={0};
    char line[256],extra;
    unsigned width,height;
    int ok=fgets(line,sizeof line,f) && !strcmp(line,"LHX texture 1\n") &&
        fgets(line,sizeof line,f) && dimensions(line,&width,&height) &&
        fgets(line,sizeof line,f) && sscanf(line,"%lf %lf %lf %lf %c",
            &result.bounds[0],&result.bounds[1],&result.bounds[2],&result.bounds[3],&extra)==4;
    for (unsigned i=0;ok && i<4;i++) ok=isfinite(result.bounds[i]);
    if (ok) ok=result.bounds[0]<result.bounds[1] && result.bounds[2]<result.bounds[3];
    if (ok) {
        result.texture.count=1;
        LhxTextureLevel *l=&result.texture.levels[0];
        l->width=width;l->height=height;
        l->pixels=malloc((size_t)width*height*sizeof(*l->pixels));
        ok=l->pixels!=NULL;
        unsigned char row[2048*3];
        for (unsigned y=0;ok && y<height;y++) {
            ok=fread(row,3,width,f)==width;
            if (ok) for (unsigned x=0;x<width;x++) l->pixels[y*width+x]=
                0xff000000u|(uint32_t)row[x*3]<<16|(uint32_t)row[x*3+1]<<8|row[x*3+2];
        }
        if (ok) ok=fgetc(f)==EOF && !ferror(f) && lhx_texture_mipmaps(&result.texture);
    }
    fclose(f);
    if (!ok) { lhx_texture_free(&result.texture); return 0; }
    lhx_texture_free(&grid->texture);*grid=result;return 1;
}
