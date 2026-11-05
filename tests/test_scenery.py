"""Check static art conversion, bounded loading, fitting and original-model fallback."""
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("lhx_import_scenery", ROOT / "tools/import_scenery.py")
IMPORTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IMPORTER)


def glb(path, *, invalid_index=False):
    positions = [( -1, 0, -2), (1, 0, -2), (1, 2, 2), (-1, 2, 2)]
    data = b"".join(struct.pack("<3f", *p) for p in positions)
    data += struct.pack("<6H", 0, 1, 2, 0, 2, 9 if invalid_index else 3)
    document = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "translation": [2, 3, 4]}], "buffers": [{"byteLength": len(data)}],
        "bufferViews": [{"buffer": 0, "byteLength": 48}, {"buffer": 0, "byteOffset": 48, "byteLength": 12}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "type": "VEC3", "count": 4},
                      {"bufferView": 1, "componentType": 5123, "type": "SCALAR", "count": 6}],
        "materials": [{"doubleSided": True, "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1]}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}]}
    encoded = json.dumps(document).encode()
    encoded += b" "*((-len(encoded))%4)
    chunks = struct.pack("<II", len(encoded), 0x4e4f534a)+encoded+struct.pack("<II", len(data), 0x004e4942)+data
    path.write_bytes(struct.pack("<III", 0x46546c67, 2, 12+len(chunks))+chunks)


def prepare_fixture(directory):
    directory.mkdir(parents=True, exist_ok=True)
    source = directory / "fixture.glb"
    glb(source)
    payload, _ = IMPORTER.convert_model(source, "unused")
    (directory / "fixture.lhp").write_bytes(payload)
    catalog = b"LHXCAT1\0"+struct.pack("<II", 1, 1)+b"fixture.lhp".ljust(64, b"\0")
    catalog += b"hangar".ljust(14, b"\0")+struct.pack("<BBH", 0, 1, 0)
    (directory / "catalog.lhc").write_bytes(catalog)
    return directory


HARNESS = r'''
#include "scene_props.h"
#include "scene_renderer.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static uint32_t image[320*200];
int main(int argc,char **argv){
 assert(argc==4);LhxProps *props=lhx_props_load(argv[1]);assert(props);
 assert(!lhx_props_load(argv[2]));
 LhxSceneMesh mesh={.has_placement=1,.world_origin={1000,80,2000},
    .model_bounds={-100,100,-80,80,-200,200},.model_name="hangar"};
 for(unsigned i=0;i<3;i++)mesh.model_to_view[i][i]=1;
 mesh.model_to_view[1][3]=-20;mesh.model_to_view[2][3]=1000;
 LhxPropFit fit;const LhxPropModel *model=lhx_props_select(props,&mesh,&fit);assert(model);
 LhxProps *partial=lhx_props_load(argv[3]);assert(partial);assert(!lhx_props_select(partial,&mesh,&fit));lhx_props_free(partial);
 assert(model->count==1 && model->groups[0].count==6);
 assert(model->minimum[0]==1 && model->minimum[1]==3 && model->minimum[2]==2);
 assert(model->maximum[0]==3 && model->maximum[1]==5 && model->maximum[2]==6);
 double volume=fit.scale[0]*fit.scale[1]*fit.scale[2];assert(fabs(volume-800000)<2);
 LhxSceneVertex bottom=lhx_props_vertex(&mesh,&fit,(LhxSceneVertex){2,3,4});
 assert(fabs(bottom.x)<.001 && fabs(bottom.y+100)<.001 && fabs(bottom.z-1000)<.001);
 strcpy(mesh.model_name,"crater");assert(!lhx_props_select(props,&mesh,&fit));strcpy(mesh.model_name,"hangar");
 mesh.model_bounds[1]=mesh.model_bounds[0];assert(!lhx_props_select(props,&mesh,&fit));mesh.model_bounds[1]=100;
 uint8_t palette[768]={0};palette[21]=palette[22]=palette[23]=63;
 LhxSceneVertex original[]={{-100,-100,1000},{100,-100,1000},{0,100,1000}};
 uint16_t index[]={0,1,2};LhxScenePrimitive face={.kind=0,.count=3,.indices=index,.color=7};
 mesh.vertices=original;mesh.vertex_count=3;mesh.primitives=&face;mesh.primitive_count=1;mesh.layer=2;
 LhxSceneView view={.x1=320,.y1=200,.cx=160,.cy=100,.focus=128,.sky_color=7,.has_camera=1,.meshes=&mesh,.mesh_count=1};
 for(unsigned i=0;i<3;i++)view.world_to_view[i][i]=1;
 LhxSceneFrame frame={.views=&view,.view_count=1};LhxSceneRenderOptions options={.props=props};
 assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);assert(image[100*320+160]==0xffff0000);
 options.props=NULL;assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);assert(image[100*320+160]==0xffffffff);
 options.props=props;strcpy(mesh.model_name,"crater");
 assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);assert(image[100*320+160]==0xffffffff);
 assert(mesh.vertex_count==3 && original[0].x==-100); /* Borrowed simulation geometry is untouched. */
 lhx_props_free(props);lhx_scene_renderer_reset();
}
'''


class Scenery(unittest.TestCase):
    def test_conversion_transform_material_and_invalid_indices(self):
        with tempfile.TemporaryDirectory(prefix="lhx-prop-convert-") as directory:
            root = Path(directory)
            path = root / "fixture.glb"
            glb(path)
            payload, _ = IMPORTER.convert_model(path, "unused")
            self.assertEqual(payload[:8], b"LHXPROP1")
            self.assertEqual(struct.unpack_from("<I6f", payload, 8), (1, 1, 3, 2, 3, 5, 6))
            self.assertEqual(payload[-3:], b"\xff\0\0")
            glb(path, invalid_index=True)
            with self.assertRaisesRegex(ValueError, "Invalid vertex index"):
                IMPORTER.convert_model(path, "unused")

    @unittest.skipUnless(shutil.which("cc"), "C compiler required")
    def test_loading_fit_render_and_fallback(self):
        with tempfile.TemporaryDirectory(prefix="lhx-props-") as directory:
            root = Path(directory)
            valid = prepare_fixture(root / "valid")
            invalid = root / "invalid"
            invalid.mkdir()
            catalog = (valid / "catalog.lhc").read_bytes()
            (invalid / "catalog.lhc").write_bytes(catalog[:16]+b"../escape.lhp".ljust(64, b"\0")+catalog[80:])
            partial = root / "partial"
            partial.mkdir()
            (partial / "fixture.lhp").write_bytes((valid / "fixture.lhp").read_bytes())
            (partial / "broken.lhp").write_bytes((valid / "fixture.lhp").read_bytes()[:60])
            partial_catalog = b"LHXCAT1\0"+struct.pack("<II", 2, 1)
            partial_catalog += b"fixture.lhp".ljust(64, b"\0")+b"broken.lhp".ljust(64, b"\0")
            partial_catalog += b"hangar".ljust(14, b"\0")+struct.pack("<BBHH", 0, 2, 0, 1)
            (partial / "catalog.lhc").write_bytes(partial_catalog)
            source = root / "check.c"
            source.write_text(HARNESS)
            binary = root / "check"
            build = subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "runtime"), str(source), str(ROOT / "runtime/scene_props.c"),
                str(ROOT / "runtime/scene_renderer.c"), str(ROOT / "runtime/scene_relief.c"), str(ROOT / "runtime/scene_texture.c"),
                "-lm", "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary), str(valid), str(invalid), str(partial)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout+run.stderr)


if __name__ == "__main__":
    unittest.main()
