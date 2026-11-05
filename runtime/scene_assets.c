#include "scene_assets.h"
#include "scene_relief.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { uint32_t start,end; unsigned theatre; } Range;
static struct { Range ranges[3];unsigned count; } files[64];
static const char *names[]={"gulf","asia","europe"};
static const char *entries[]={"GULF.W","ASIA.W","EUROPE.W"};
static int enabled,selected=-1,ready,relief_enabled;
static char directory[512];
static LhxTerrain terrain;

static uint32_t little32(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }

void lhx_terrain_reset(void)
{
    lhx_texture_free(&terrain.near.texture);lhx_texture_free(&terrain.far.texture);
    lhx_relief_free(&terrain.near);lhx_relief_free(&terrain.far);
    memset(files,0,sizeof files);ready=0;selected=-1;enabled=0;relief_enabled=0;
}

void lhx_terrain_configure(int active,const char *path)
{
    lhx_terrain_reset();
    if (!active || !path || strlen(path)>=sizeof directory) return;
    strcpy(directory,path);enabled=1;
}

void lhx_terrain_relief_configure(int active){relief_enabled=active;}

void lhx_terrain_file_close(unsigned handle)
{ if (handle<64) memset(&files[handle],0,sizeof files[handle]); }

void lhx_terrain_file_open(unsigned handle,FILE *f)
{
    if (!enabled || handle>=64 || !f) return;
    lhx_terrain_file_close(handle);
    long position=ftell(f);
    if (position<0 || fseek(f,0,SEEK_END)) return;
    long size=ftell(f);
    unsigned char header[7],row[18],next[18];
    if (size<25 || fseek(f,0,SEEK_SET) || fread(header,1,7,f)!=7 ||
        memcmp(header,"EALIB",5)) goto done;
    unsigned count=header[5]|(unsigned)header[6]<<8;
    if (count>4096 || (uint64_t)7+(count+1)*18>(uint64_t)size ||
        fread(row,1,18,f)!=18) goto done;
    for (unsigned i=0;i<count;i++) {
        if (fread(next,1,18,f)!=18) { lhx_terrain_file_close(handle);break; }
        uint32_t start=little32(row+14),end=little32(next+14);
        if (start>=(uint32_t)(7+(count+1)*18) && end>start && end<=(uint64_t)size) {
            char name[14];memcpy(name,row,13);name[13]=0;
            for (unsigned t=0;t<3;t++) if (!strcasecmp(name,entries[t]) && files[handle].count<3)
                files[handle].ranges[files[handle].count++]=(Range){start,end,t};
        }
        memcpy(row,next,18);
    }
done:
    if (fseek(f,position,SEEK_SET)) lhx_terrain_file_close(handle);
}

void lhx_terrain_file_read(unsigned handle,long position,unsigned bytes)
{
    if (!enabled || handle>=64 || position<0 || !bytes) return;
    for (unsigned i=0;i<files[handle].count;i++) {
        const Range *r=&files[handle].ranges[i];
        if ((uint64_t)position>=r->end || (uint64_t)position+bytes<=r->start) continue;
        if (selected==(int)r->theatre) return;
        selected=(int)r->theatre;ready=0;
        lhx_texture_free(&terrain.near.texture);lhx_texture_free(&terrain.far.texture);
        lhx_relief_free(&terrain.near);lhx_relief_free(&terrain.far);
        char near[1024],far[1024];
        snprintf(near,sizeof near,"%s/%s_near.lht",directory,names[selected]);
        snprintf(far,sizeof far,"%s/%s_far.lht",directory,names[selected]);
        if (lhx_texture_load(&terrain.near,near) && lhx_texture_load(&terrain.far,far)) {
            ready=1;fprintf(stderr,"[terrain] %s: flat satellite ground loaded\n",names[selected]);
            if(relief_enabled){
                snprintf(near,sizeof near,"%s/%s_near.lhr",directory,names[selected]);
                snprintf(far,sizeof far,"%s/%s_far.lhr",directory,names[selected]);
                if(lhx_relief_load(&terrain.near,near) && lhx_relief_load(&terrain.far,far))
                    fprintf(stderr,"[terrain] %s: visual elevation and hill mounds loaded\n",names[selected]);
                else{lhx_relief_free(&terrain.near);lhx_relief_free(&terrain.far);
                    fprintf(stderr,"[terrain] %s: missing/invalid relief; keeping flat terrain\n",names[selected]);}
            }
        } else {
            lhx_texture_free(&terrain.near.texture);lhx_texture_free(&terrain.far.texture);
        lhx_relief_free(&terrain.near);lhx_relief_free(&terrain.far);
            fprintf(stderr,"[terrain] %s: missing/invalid textures in %s; using original ground\n",
                    names[selected],directory);
        }
        return;
    }
}

const LhxTerrain *lhx_terrain_current(void)
{ return ready?&terrain:NULL; }
