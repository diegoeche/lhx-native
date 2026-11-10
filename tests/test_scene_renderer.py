"""Exercise native scene geometry without SDL or the original game executable."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include "scene_renderer.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t palette[768];
static uint32_t *pixels, *reference;
static const uint32_t BLACK=0xff000000u, RED=0xffff0000u;
static const uint32_t GREEN=0xff00ff00u;
static const unsigned capacity=1920*1200;
static LhxSceneView view;
static LhxSceneFrame frame;

static LhxSceneVertex at(double x,double y,double z) {
    return (LhxSceneVertex){(float)((x-160)*z/128),
                            (float)((100-y)*z/120), (float)z};
}
static LhxScenePrimitive primitive(unsigned color,unsigned count,uint16_t*indices) {
    return (LhxScenePrimitive){.kind=0,.color=color,.count=count,.indices=indices};
}
static LhxSceneMesh mesh(LhxSceneVertex*v,unsigned nv,LhxScenePrimitive*p,unsigned np,int layer) {
    return (LhxSceneMesh){.vertices=v,.primitives=p,.vertex_count=nv,
                          .primitive_count=np,.layer=layer};
}
static void setup(void) {
    memset(palette,0,sizeof palette);
    palette[3]=63; palette[7]=63; palette[11]=63;
    palette[12]=palette[13]=palette[14]=63;
    view=(LhxSceneView){.x0=0,.y0=0,.x1=320,.y1=200,.cx=160,.cy=100,.focus=128};
    frame=(LhxSceneFrame){&view,1,1};
    memset(pixels,0x5a,(size_t)capacity*sizeof *pixels);
}
static void render(unsigned w,unsigned h) {
    assert(lhx_scene_render(&frame,palette,pixels,w,h)==1);
}
static uint32_t sample(unsigned w,double x,double y) {
    return pixels[(unsigned)(y*w/320.0)*(size_t)w+(unsigned)(x*w/320.0)];
}
static unsigned difference(unsigned n) {
    unsigned different=0;
    for(unsigned i=0;i<n;i++) different+=pixels[i]!=reference[i];
    return different;
}
static void test_winding(void) {
    setup();
    LhxSceneVertex v[]={at(60,40,10),at(260,40,10),at(160,160,10)};
    uint16_t idx[]={0,1,2};
    LhxScenePrimitive p=primitive(1,3,idx);p.one_sided=1;
    LhxSceneMesh m=mesh(v,3,&p,1,2);view.meshes=&m;view.mesh_count=1;
    render(960,600);assert(sample(960,160,80)==RED);
    idx[1]=2;idx[2]=1;render(960,600);assert(sample(960,160,80)==BLACK);
    p.one_sided=0;render(960,600);assert(sample(960,160,80)==RED);
    for(unsigned i=0;i<3;i++)v[i].z=1;
    render(960,600);assert(sample(960,160,80)==BLACK);
}
static LhxSceneVertex cut(LhxSceneVertex a,LhxSceneVertex b) {
    double t=(2-a.z)/(b.z-a.z);
    return (LhxSceneVertex){(float)(a.x+(b.x-a.x)*t),
                            (float)(a.y+(b.y-a.y)*t),2};
}
static void test_near(void) {
    setup();
    LhxSceneVertex v[]={at(50,30,1),at(280,55,9),at(150,180,6)};
    uint16_t idx[]={0,1,2};LhxScenePrimitive p=primitive(1,3,idx);
    LhxSceneMesh m=mesh(v,3,&p,1,2);view.meshes=&m;view.mesh_count=1;
    render(960,600);memcpy(reference,pixels,960*600*sizeof *pixels);
    unsigned red=0;for(unsigned i=0;i<960*600;i++)red+=pixels[i]==RED;
    assert(red>1000 && red<960*600);
    LhxSceneVertex expected[]={cut(v[0],v[1]),v[1],v[2],cut(v[0],v[2])};
    uint16_t quad[]={0,1,2,3};p.indices=quad;p.count=4;m.vertices=expected;m.vertex_count=4;
    render(960,600);assert(difference(960*600)<10);
    /* A line crossing both the eye and the near plane must terminate at Z=2. */
    v[0]=(LhxSceneVertex){-1,0,-2};v[1]=(LhxSceneVertex){1,0,8};
    p.kind=1;p.count=2;p.indices=idx;m.vertices=v;m.vertex_count=2;
    render(960,600);assert(sample(960,160,100)==RED);
    for(unsigned i=0;i<960*600;i++)assert(pixels[i]==BLACK || pixels[i]==RED);
}
static void test_depth(void) {
    setup();
    LhxSceneVertex v[]={at(40,30,10),at(280,30,10),at(160,180,10),
                        at(40,30,20),at(280,30,20),at(160,180,20)};
    uint16_t a[]={0,1,2},b[]={3,4,5};
    LhxScenePrimitive p[]={primitive(1,3,a),primitive(2,3,b)};
    LhxSceneMesh m=mesh(v,6,p,2,2);view.meshes=&m;view.mesh_count=1;
    render(960,600);assert(sample(960,160,80)==RED);
    memcpy(reference,pixels,960*600*sizeof *pixels);
    LhxScenePrimitive tmp=p[0];p[0]=p[1];p[1]=tmp;
    render(960,600);assert(difference(960*600)==0);
    /* Inverse Z, rather than interpolated Z, decides the crossing surfaces. */
    v[3]=at(40,30,4);v[4]=at(280,30,20);v[5]=at(160,180,20);
    render(960,600);
    assert(sample(960,75,50)==GREEN);
    assert(sample(960,240,50)==RED);
    /* A solid object is drawn after the separately cleared ground depth. */
    LhxSceneMesh layers[]={mesh(v,3,&p[1],1,1),mesh(v,6,&p[0],1,2)};
    layers[0].primitives=&p[1];layers[1].primitives=&p[0];
    view.meshes=layers;view.mesh_count=2;
    /* p[1] references near vertices 0..2 and p[0] references 3..5. */
    for(unsigned i=3;i<6;i++)v[i]=at(i==3?40:i==4?280:160,i==5?180:30,20);
    render(960,600);assert(sample(960,160,80)==GREEN);
}
static void test_concave(void) {
    setup();
    double xy[][2]={{50,40},{270,40},{270,160},{190,160},{190,80},{130,80},{130,160},{50,160}};
    LhxSceneVertex v[8];uint16_t idx[8];
    for(unsigned i=0;i<8;i++){v[i]=at(xy[i][0],xy[i][1],20);idx[i]=(uint16_t)i;}
    LhxScenePrimitive p=primitive(1,8,idx);p.one_sided=1;
    LhxSceneMesh m=mesh(v,8,&p,1,2);view.meshes=&m;view.mesh_count=1;
    render(960,600);
    assert(sample(960,90,120)==RED && sample(960,230,120)==RED);
    assert(sample(960,160,60)==RED && sample(960,160,120)==BLACK);
    /* Reverse a concave contour: two-sided geometry must have identical coverage. */
    memcpy(reference,pixels,960*600*sizeof *pixels);p.one_sided=0;
    for(unsigned i=0;i<4;i++){uint16_t t=idx[i];idx[i]=idx[7-i];idx[7-i]=t;}
    render(960,600);assert(difference(960*600)==0);
}
static void test_decal_stipple(void) {
    setup();
    LhxSceneVertex v[]={at(40,30,100),at(280,30,100),at(280,170,100),at(40,170,100),
                       at(100,70,100.03),at(220,70,100.03),at(220,130,100.03),at(100,130,100.03)};
    uint16_t a[]={0,1,2,3},b[]={4,5,6,7};
    LhxScenePrimitive p[]={primitive(1,4,a),primitive(2,4,b)};
    LhxSceneMesh m=mesh(v,8,p,2,2);view.meshes=&m;view.mesh_count=1;
    render(960,600);assert(sample(960,160,100)==GREEN && sample(960,60,100)==RED);
    memcpy(reference,pixels,960*600*sizeof *pixels);
    LhxScenePrimitive tmp=p[0];p[0]=p[1];p[1]=tmp;
    render(960,600);assert(difference(960*600)==0);
    p[0].stipple=1;render(960,600);
    for(unsigned y=280;y<320;y++)for(unsigned x=450;x<500;x++)
        assert(pixels[y*960+x]==(((x^y)&1)?RED:GREEN));
}
static void test_native_resolution(void) {
    setup();
    LhxSceneVertex v[]={at(40.2,40.3,10),at(250.7,65.2,10),at(110.4,170.8,10)};
    uint16_t idx[]={0,1,2};LhxScenePrimitive p=primitive(1,3,idx);
    LhxSceneMesh m=mesh(v,3,&p,1,2);view.meshes=&m;view.mesh_count=1;
    render(320,200);memcpy(reference,pixels,320*200*sizeof *pixels);
    render(960,600);
    unsigned changed=0;
    for(unsigned y=0;y<600;y++)for(unsigned x=0;x<960;x++)
        changed+=pixels[y*960+x]!=reference[(y/3)*320+x/3];
    assert(changed>500);
    render(1920,1200);
    assert(sample(1920,120,100)==RED && sample(1920,280,170)==BLACK);
}
static void test_lines(void) {
    setup();
    double segments[][4]={{20.3,30.1,300.2,180.8},{50.7,190.3,270.2,10.4},
                          {160,5,160,195},{4,100,316,100},{160,100,160,100}};
    uint16_t idx[]={0,1};LhxScenePrimitive p=primitive(1,2,idx);p.kind=1;
    LhxSceneVertex v[2];LhxSceneMesh m=mesh(v,2,&p,1,2);
    view.meshes=&m;view.mesh_count=1;
    for(unsigned i=0;i<sizeof(segments)/sizeof(*segments);i++) {
        v[0]=at(segments[i][0],segments[i][1],8);
        v[1]=at(segments[i][2],segments[i][3],20);
        if(i==4)v[1]=v[0];
        render(960,600);
        double ax=3*(160+128*(double)v[0].x/v[0].z);
        double ay=3*(100-120*(double)v[0].y/v[0].z);
        double bx=3*(160+128*(double)v[1].x/v[1].z);
        double by=3*(100-120*(double)v[1].y/v[1].z);
        double dx=bx-ax,dy=by-ay,len=dx*dx+dy*dy;
        unsigned differences=0;
        for(unsigned y=0;y<600;y++)for(unsigned x=0;x<960;x++) {
            double px=x+0.5-ax,py=y+0.5-ay;
            double t=len>1e-18?(px*dx+py*dy)/len:0;
            t=fmax(0,fmin(1,t));double ex=px-t*dx,ey=py-t*dy;
            uint32_t expected=ex*ex+ey*ey<=1.2*1.2?RED:BLACK;
            differences+=pixels[y*960+x]!=expected;
        }
        assert(differences<3);
    }
}
static void test_viewport(void) {
    setup();
    view.x0=50;view.x1=270;view.y0=25;view.y1=175;
    view.horizon=(LhxSceneHorizon){{1,2},1,160,100,1,-0.37f};
    render(960,600);
    for(unsigned y=0;y<600;y++)for(unsigned x=0;x<960;x++) {
        uint32_t expected=0x5a5a5a5a;
        if(x>=150 && x<810 && y>=75 && y<525) {
            double side=((x+0.5)/3-160)-0.37f*((y+0.5)/3-100);
            expected=side>=0?RED:GREEN;
        }
        assert(pixels[y*960+x]==expected);
    }
}
static void test_sphere_points(void) {
    setup();
    LhxSceneVertex v[]={at(160,100,40),at(20,20,10),at(300,20,10),at(160,190,10)};
    uint16_t idx[]={0};LhxScenePrimitive p=primitive(1,1,idx);p.kind=3;p.radius=5;
    LhxSceneMesh m=mesh(v,4,&p,1,2);view.meshes=&m;view.mesh_count=1;
    render(960,600);
    assert(sample(960,160,100)==RED && sample(960,177,100)==BLACK);
    assert(sample(960,160,115)==BLACK);
    p.radius=0;render(320,200);
    unsigned red=0;for(unsigned i=0;i<320*200;i++)red+=pixels[i]==RED;
    assert(red>=1 && red<=4);
    p.kind=2;render(960,600);assert(sample(960,160,100)==RED);
    /* Background points do not occlude a distant polygon. */
    uint16_t triangle[]={1,2,3};LhxScenePrimitive polygon=primitive(2,3,triangle);
    LhxSceneMesh layers[]={mesh(v,4,&p,1,0),mesh(v,4,&polygon,1,2)};
    view.meshes=layers;view.mesh_count=2;v[0].z=2;
    render(960,600);assert(sample(960,160,100)==GREEN);
}
static void test_invalid(void) {
    setup();assert(lhx_scene_render(NULL,palette,pixels,960,600)==0);
    assert(lhx_scene_render(&frame,palette,pixels,0,600)==-1);
    assert(lhx_scene_render(&frame,palette,pixels,8192,8192)==-1);
    LhxSceneVertex v[]={at(160,100,20)};uint16_t idx[]={3};
    LhxScenePrimitive p=primitive(1,1,idx);LhxSceneMesh m=mesh(v,1,&p,1,2);
    view.meshes=&m;view.mesh_count=1;
    assert(lhx_scene_render(&frame,palette,pixels,960,600)==-1);
    idx[0]=0;p.count=1025;
    assert(lhx_scene_render(&frame,palette,pixels,960,600)==-1);
    p.count=1;v[0].x=NAN;
    assert(lhx_scene_render(&frame,palette,pixels,960,600)==-1);
}
int main(int argc,char **argv) {
    assert(argc==2);pixels=malloc((size_t)capacity*sizeof *pixels);
    reference=malloc((size_t)capacity*sizeof *reference);assert(pixels && reference);
    if(!strcmp(argv[1],"winding"))test_winding();
    else if(!strcmp(argv[1],"near"))test_near();
    else if(!strcmp(argv[1],"depth"))test_depth();
    else if(!strcmp(argv[1],"concave"))test_concave();
    else if(!strcmp(argv[1],"decal"))test_decal_stipple();
    else if(!strcmp(argv[1],"native"))test_native_resolution();
    else if(!strcmp(argv[1],"lines"))test_lines();
    else if(!strcmp(argv[1],"viewport"))test_viewport();
    else if(!strcmp(argv[1],"sphere"))test_sphere_points();
    else if(!strcmp(argv[1],"invalid"))test_invalid();
    else assert(!"unknown renderer test");
    lhx_scene_renderer_reset();lhx_scene_renderer_reset();free(pixels);free(reference);
    return 0;
}
'''


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class SceneRenderer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="lhx-scene-")
        directory = Path(cls.temporary.name)
        source = directory / "check.c"
        source.write_text(HARNESS)
        cls.binary = directory / "check"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "runtime"), str(source),
                        str(ROOT / "runtime/scene_renderer.c"), str(ROOT / "runtime/scene_motion.c"), str(ROOT / "runtime/scene_relief.c"), str(ROOT / "runtime/scene_props.c"),
                        str(ROOT / "runtime/scene_texture.c"), "-lm", "-o", str(cls.binary)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def check_scene(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_clockwise_faces_and_hidden_backfaces(self):
        self.check_scene("winding")

    def test_near_clipping_matches_explicit_geometry(self):
        self.check_scene("near")

    def test_depth_interpolation_and_separate_ground_layer(self):
        self.check_scene("depth")

    def test_concave_polygon_preserves_notch(self):
        self.check_scene("concave")

    def test_coplanar_details_and_native_pixel_stipple(self):
        self.check_scene("decal")

    def test_geometry_is_sampled_at_output_resolution(self):
        self.check_scene("native")

    def test_line_capsules_match_full_distance_reference(self):
        self.check_scene("lines")

    def test_viewport_scissor_and_native_horizon(self):
        self.check_scene("viewport")

    def test_spheres_points_and_background_layer(self):
        self.check_scene("sphere")

    def test_invalid_resources_are_reported(self):
        self.check_scene("invalid")


if __name__ == "__main__":
    unittest.main()
