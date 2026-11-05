"""Check original horizon recovery and opaque pixel-art overlay independently."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class SceneComposite(unittest.TestCase):
    def test_horizon_and_original_overlay(self):
        with tempfile.TemporaryDirectory(prefix="lhx-scene-composite-") as directory:
            tmp = Path(directory)
            (tmp / "recomp_all.h").write_text("")
            source = tmp / "check.c"
            source.write_text(r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scene_composite.h"
enum { SCREEN=0x5000 };
static void word(CPU*c,unsigned off,unsigned value){mem_write16(c,c->ds,off,value);}
static unsigned row(CPU*c,unsigned y){return mem_read16(c,c->ds,0xeb6e + 2*y);}
static void window(CPU*c,unsigned x,unsigned y,unsigned width,unsigned height){
    word(c,0xeb5a,x);word(c,0xeb58,width/2);
    word(c,0xeed1,y);word(c,0xeed3,y+height-1);word(c,0xecfe,SCREEN);
    /* Reversed, offset rows prove the original row table is authoritative. */
    for(unsigned n=0;n<200;n++)word(c,0xeb6e + 2*n,(199-n)*320+16);
}
static void put(CPU*c,unsigned x,unsigned y,unsigned color){
    mem_write8(c,SCREEN,(uint16_t)(row(c,y)+x),color);
}
static unsigned get(CPU*c,unsigned x,unsigned y){
    return mem_read8(c,SCREEN,(uint16_t)(row(c,y)+x));
}
static void horizons(CPU*c){
    const unsigned x0=48,y0=12,width=160,height=140;
    window(c,x0,y0,width,height);
    LhxSceneView view={.x0=7,.y0=8,.x1=9,.y1=10,.cx=11,.cy=12,.focus=128};
    const double angles[]={0,.31,.7853981633974483,1.5707963267948966,
                           1.91,2.8,3.141592653589793,-.37,-1.5707963267948966};
    for(unsigned n=0;n<sizeof(angles)/sizeof(angles[0]);n++){
        double nx=cos(angles[n]),ny=sin(angles[n]);
        double cx=x0+width*.43,cy=y0+height*.56;
        for(unsigned y=y0;y<y0+height;y++)for(unsigned x=x0;x<x0+width;x++)
            put(c,x,y,(x+.5-cx)*nx+(y+.5-cy)*ny>=0?9:3);
        /* Small unrelated colors do not become the sky/ground pair. */
        for(unsigned k=0;k<9;k++)put(c,x0+k*13,y0+3,22);
        lhx_scene_read_horizon(c,&view);
        const LhxSceneHorizon*h=&view.horizon;
        assert(h->two_colors);
        assert((h->colors[0]==9 && h->colors[1]==3)||
               (h->colors[0]==3 && h->colors[1]==9));
        assert(fabs(h->nx*h->nx+h->ny*h->ny-1)<.00001);
        assert(fabs((h->x-cx)*nx+(h->y-cy)*ny)<.8);
        for(unsigned y=y0;y<y0+height;y++)for(unsigned x=x0;x<x0+width;x++){
            double side=(x+.5-cx)*nx+(y+.5-cy)*ny;
            if(fabs(side)<1.2 || get(c,x,y)==22)continue;
            unsigned actual=h->colors[(x+.5-h->x)*h->nx+(y+.5-h->y)*h->ny<0];
            assert(actual==(side>=0?9:3));
        }
        assert(view.x0==7 && view.y0==8 && view.x1==9 && view.y1==10);
        assert(view.cx==11 && view.cy==12 && view.focus==128);
    }
    for(unsigned y=y0;y<y0+height;y++)for(unsigned x=x0;x<x0+width;x++)put(c,x,y,23);
    lhx_scene_read_horizon(c,&view);
    assert(!view.horizon.two_colors && view.horizon.colors[0]==23);
    /* A one-pixel corner sliver remains a recoverable horizon. */
    put(c,x0,y0,4);lhx_scene_read_horizon(c,&view);
    assert(view.horizon.two_colors);
    const LhxSceneHorizon*h=&view.horizon;
    assert(h->colors[(x0+.5-h->x)*h->nx+(y0+.5-h->y)*h->ny<0]==4);
    assert(h->colors[(x0+10.5-h->x)*h->nx+(y0+10.5-h->y)*h->ny<0]==23);
    /* Clip a two-pixel-wide source window against the screen's right edge.
     * The remaining one-column window has exactly one boundary midpoint. */
    window(c,319,20,2,2);put(c,319,20,5);put(c,319,21,7);
    lhx_scene_read_horizon(c,&view);h=&view.horizon;
    assert(h->two_colors && fabs(h->x-319.5)<.0001 && fabs(h->y-21)<.0001);
    assert(h->colors[(320-h->x)*h->nx+(20.5-h->y)*h->ny<0]==5);
    assert(h->colors[(320-h->x)*h->nx+(21.5-h->y)*h->ny<0]==7);
    word(c,0xeb58,0);lhx_scene_read_horizon(c,&view);
    assert(!view.horizon.two_colors && view.horizon.colors[0]==0);
    word(c,0xeb58,10);word(c,0xeed1,30);word(c,0xeed3,20);
    lhx_scene_read_horizon(c,&view);assert(!view.horizon.two_colors);
}
static int hole(unsigned x,unsigned y){
    return (x>=10 && x<30 && y>=20 && y<40)||
           (x>=200 && x<210 && y>=100 && y<110)||(x<2 && y<2);
}
static uint32_t native_pixel(unsigned i){
    return 0xff000000u | (((i*2654435761u)^0x123456u)&0x00ffffffu);
}
static void overlay(unsigned WIDTH,unsigned HEIGHT){
    uint8_t vga[64000],palette[768]={0};
    size_t count=(size_t)WIDTH*HEIGHT;
    uint32_t*storage=malloc((count+2)*sizeof(*storage));assert(storage);
    uint32_t*pixels=storage+1;
    storage[0]=storage[count+1]=0xdeadbeefu;
    memset(vga,255,sizeof(vga));
    /* Mix short opaque/transparent runs, as well as the long index255 runs
     * crossing captured-window boundaries. Keep every destination's native
     * color distinct to catch row copies that smear transparent geometry. */
    for(unsigned y=10;y<35;y++)for(unsigned x=0;x<320;x++){
        unsigned pattern=(x*17+y*31)%13;
        if(pattern<3)vga[y*320+x]=7;
        else if(pattern==3)vga[y*320+x]=0;
    }
    palette[255*3]=palette[255*3+1]=palette[255*3+2]=63;
    palette[7*3]=0xc0|63;palette[7*3+2]=0xc0|31;
    vga[21*320+11]=7; /* Original HUD must stay opaque over the native view. */
    vga[22*320+12]=0; /* Black pixel art is also opaque. */
    vga[150*320+150]=7;
    LhxSceneView views[]={
        {.x0=10,.y0=20,.x1=30,.y1=40},
        {.x0=200,.y0=100,.x1=210,.y1=110},
        {.x0=-4,.y0=-4,.x1=2,.y1=2},
        {.x0=500,.y0=0,.x1=700,.y1=2},
        {.x0=40,.y0=40,.x1=30,.y1=50}};
    LhxSceneFrame frame={.views=views,.view_count=5};
    for(unsigned i=0;i<count;i++)pixels[i]=native_pixel(i);
    lhx_scene_composite(&frame,vga,palette,pixels,WIDTH,HEIGHT);
    for(unsigned y=0;y<HEIGHT;y++)for(unsigned x=0;x<WIDTH;x++){
        unsigned sx=(uint64_t)x*320/WIDTH,sy=(uint64_t)y*200/HEIGHT;
        unsigned color=vga[sy*320+sx];
        uint32_t expected=color==7?0xffff007d:color==0?0xff000000:
                          hole(sx,sy)?native_pixel(y*WIDTH+x):0xffffffff;
        assert(pixels[y*WIDTH+x]==expected);
    }
    /* Menu screens and index255 outside captured views remain original. */
    lhx_scene_composite(NULL,vga,palette,pixels,WIDTH,HEIGHT);
    for(unsigned y=0;y<HEIGHT;y++)for(unsigned x=0;x<WIDTH;x++){
        unsigned color=vga[((uint64_t)y*200/HEIGHT)*320+(uint64_t)x*320/WIDTH];
        assert(pixels[y*WIDTH+x]==(color==7?0xffff007d:color==0?0xff000000:0xffffffff));
    }
    LhxSceneFrame empty={0};
    lhx_scene_composite(&empty,vga,palette,pixels,WIDTH,HEIGHT);
    lhx_scene_composite(&frame,vga,palette,pixels,0,HEIGHT);
    assert(storage[0]==0xdeadbeefu && storage[count+1]==0xdeadbeefu);
    free(storage);
}
int main(void){
    CPU cpu={0};cpu.mem=calloc(1,MEM_SIZE);assert(cpu.mem);cpu.ds=0x2000;
    horizons(&cpu);
    const unsigned sizes[][2]={{1,1},{137,83},{319,199},{320,200},{641,403},
        {960,600},{1920,1200},{2560,1600},{320,600},{960,200}};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++)
        overlay(sizes[i][0],sizes[i][1]);
    free(cpu.mem);
    puts("rotated horizons, original HUD, menu opacity and clipped multiwindow overlay passed");
}
''')
            binary = tmp / "check"
            compiled = subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(tmp), "-I", str(ROOT / "runtime"),
                str(source), str(ROOT / "runtime/scene_composite.c"), "-lm", "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("rotated horizons, original HUD, menu opacity and clipped multiwindow overlay passed",
                          result.stdout)


if __name__ == "__main__":
    unittest.main()
