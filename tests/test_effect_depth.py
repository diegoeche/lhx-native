"""Verify finite smoke spheres occlude scenery at their actual depth."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "scene_renderer.h"
#include <assert.h>
#include <string.h>
static uint32_t image[320*200];
int main(void) {
    uint8_t palette[768]={0};
    palette[3]=63;palette[8]=63; /* red smoke, blue solid */
    uint16_t ring[]={0,1,2,3},index=0;
    LhxScenePrimitive face={.kind=0,.color=2,.count=4,.indices=ring};
    LhxScenePrimitive sphere={.kind=3,.color=1,.count=1,.indices=&index,.radius=10};
    LhxSceneVertex wall[]={{-80,-80,200},{80,-80,200},{80,80,200},{-80,80,200}};
    LhxSceneVertex particle={0,0,100};
    LhxSceneMesh smoke={.vertices=&particle,.vertex_count=1,.primitives=&sphere,
                        .primitive_count=1,.layer=0};
    LhxSceneMesh solid={.vertices=wall,.vertex_count=4,.primitives=&face,
                        .primitive_count=1,.layer=2};
    LhxSceneMesh meshes[]={smoke,solid};
    LhxSceneView view={.x1=320,.y1=200,.cx=160,.cy=100,.focus=128,
                       .meshes=meshes,.mesh_count=2};
    LhxSceneFrame frame={.views=&view,.view_count=1};
    LhxSceneRenderOptions options={.sphere_depth=1};
    const unsigned center=100*320+160;
    /* The old background pass incorrectly lets the farther wall cover smoke. */
    assert(lhx_scene_render(&frame,palette,image,320,200)==1);
    assert(image[center]==0xff0000ff);
    for (unsigned order=0;order<2;order++) {
        meshes[order]=smoke;meshes[1-order]=solid;
        particle.z=100;
        assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);
        assert(image[center]==0xffff0000);
        particle.z=300;
        assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);
        assert(image[center]==0xff0000ff);
    }
    /* Zero-radius spheres and point markers still belong in the background. */
    particle.z=100;sphere.radius=0;
    assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);
    assert(image[center]==0xff0000ff);
    sphere.kind=2;
    assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);
    assert(image[center]==0xff0000ff);
    /* Each option also works with AA; capture metadata remains unchanged. */
    sphere.kind=3;sphere.radius=10;options.antialiasing=1;
    assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);
    assert(image[center]==0xffff0000);
    assert(meshes[1].layer==0 && particle.z==100 && sphere.radius==10);
    options=(LhxSceneRenderOptions){0};
    assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);
    assert(image[center]==0xff0000ff);
    lhx_scene_renderer_reset();
}
'''


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class EffectDepth(unittest.TestCase):
    def test_smoke_depth_and_legacy_layering(self):
        with tempfile.TemporaryDirectory(prefix="lhx-effect-depth-") as directory:
            work = Path(directory)
            source = work / "check.c"
            source.write_text(HARNESS)
            binary = work / "check"
            subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "runtime"), str(source),
                str(ROOT / "runtime/scene_renderer.c"), str(ROOT / "runtime/scene_relief.c"), str(ROOT / "runtime/scene_props.c"), str(ROOT / "runtime/scene_texture.c"),
                "-lm", "-o", str(binary)], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
