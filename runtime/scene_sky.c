#include "scene_sky.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int panorama(LhxTexture *t,const char *path)
{
    FILE *f=fopen(path,"rb");if(!f)return 0;
    unsigned char header[16];
    int ok=fread(header,1,sizeof header,f)==sizeof header&&!memcmp(header,"LHXSKY1\0",8);
    unsigned w=0,h=0;
    if(ok){
        for(unsigned i=0;i<4;i++){w|=(unsigned)header[8+i]<<(i*8);h|=(unsigned)header[12+i]<<(i*8);}
        ok=w>0&&w<=4096&&h>0&&h<=2048&&w==h*2;
    }
    if(ok){
        t->count=1;t->levels[0]=(LhxTextureLevel){w,h,malloc((size_t)w*h*4)};
        ok=t->levels[0].pixels!=NULL;
    }
    unsigned char row[4096*3];
    for(unsigned y=0;ok&&y<h;y++){
        ok=fread(row,3,w,f)==w;
        if(ok)for(unsigned x=0;x<w;x++)t->levels[0].pixels[(size_t)y*w+x]=
            0xff000000u|(uint32_t)row[x*3]<<16|(uint32_t)row[x*3+1]<<8|row[x*3+2];
    }
    if(ok)ok=fgetc(f)==EOF&&!ferror(f)&&lhx_texture_mipmaps(t);
    fclose(f);if(!ok)lhx_texture_free(t);return ok;
}
void lhx_sky_free(LhxSky *sky)
{if(sky){lhx_texture_free(&sky->day);lhx_texture_free(&sky->night);free(sky);}}
LhxSky *lhx_sky_load(const char *directory)
{
    if(!directory||strlen(directory)>480)return NULL;
    LhxSky *sky=calloc(1,sizeof(*sky));if(!sky)return NULL;
    char day[512],night[512];snprintf(day,sizeof day,"%.480s/day.lhs",directory);snprintf(night,sizeof night,"%.480s/night.lhs",directory);
    if(!panorama(&sky->day,day)||!panorama(&sky->night,night)){
        fprintf(stderr,"[sky] missing/invalid panoramas in %s; retaining original sky\n",directory);
        lhx_sky_free(sky);return NULL;
    }
    fprintf(stderr,"[sky] day/night panoramas loaded\n");return sky;
}
