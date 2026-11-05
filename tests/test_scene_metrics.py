"""Check optional scene measurement counts, quantiles, caching, and reports."""
import csv
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include "scene_metrics.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static LhxSceneVertex vertices_a[]={{3,4,12},{0,0,10},{-6,8,-24}};
static LhxSceneVertex vertices_b[]={{0,0,50},{12,0,5}};
static LhxSceneMesh meshes[]={
    {.vertices=vertices_a,.vertex_count=3,.primitive_count=4,.layer=2},
    {.vertices=vertices_b,.vertex_count=2,.primitive_count=3,.layer=1}
};
static LhxSceneView views[]={
    {.meshes=meshes,.mesh_count=1}, {.meshes=meshes+1,.mesh_count=1}
};
static LhxSceneFrame frame={views,2,17};

int main(int argc,char**argv) {
    assert(argc==2 || argc==3);
    if(!strcmp(argv[1],"disabled")) {
        assert(!lhx_scene_metrics_enabled());
        lhx_scene_metrics_record_frame(&frame,20,30);
        lhx_scene_metrics_record_capture(10,100,20,2);
        lhx_scene_metrics_record_visibility(100,40,30,30,1);
        assert(lhx_scene_metrics_report_json(stdout)==0);
        assert(lhx_scene_metrics_report_csv(stdout)==0);
        lhx_scene_metrics_reset();lhx_scene_metrics_reset();return 0;
    }
    unsigned limit=!strncmp(argv[1],"reservoir",9)?8:!strcmp(argv[1],"default")?0:100;
    assert(lhx_scene_metrics_init(limit)==1);
    assert(lhx_scene_metrics_enabled());
    if(!strcmp(argv[1],"known")) {
        for(unsigned i=1;i<=100;i++) {
            if(i==51){frame.serial++;vertices_b[0].z=100;}
            lhx_scene_metrics_record_frame(&frame,i,2*(101-i));
            if(i%10==0)lhx_scene_metrics_record_capture(i*10,100+i,i,i%3);
            if(i%20==0)lhx_scene_metrics_record_visibility(100+i,i/2,i/4,100+i-i/2-i/4,i>=80);
        }
    } else if(!strcmp(argv[1],"cache")) {
        vertices_a[0]=(LhxSceneVertex){3,4,0};
        meshes[0].vertex_count=1;frame.view_count=1;
        lhx_scene_metrics_record_frame(&frame,1,2);
        vertices_a[0]=(LhxSceneVertex){6,8,0};
        lhx_scene_metrics_record_frame(&frame,3,4); /* same serial: cached 5 */
        frame.serial++;lhx_scene_metrics_record_frame(&frame,5,6);
    } else if(!strncmp(argv[1],"reservoir",9)) {
        for(unsigned i=1;i<=10000;i++) {
            if(!strcmp(argv[1],"reservoir"))lhx_scene_metrics_record_frame(&frame,i,i%7);
            lhx_scene_metrics_record_capture(10001-i,i,i/2,i%3);
        }
    } else if(!strcmp(argv[1],"independent")) {
        lhx_scene_metrics_record_capture(10,100,10,1);
        lhx_scene_metrics_record_capture(30,200,20,2);
        lhx_scene_metrics_record_capture(20,300,30,3);
        for(unsigned i=0;i<5;i++)lhx_scene_metrics_record_visibility(100+i,30+2*i,20,50-i,(i%2)?-1:0);
    } else if(!strcmp(argv[1],"origins")) {
        meshes[0].object_id=1;meshes[0].object_distance=1500;
        meshes[1].object_id=2;meshes[1].object_distance=9000;meshes[1].layer=0;
        lhx_scene_metrics_record_frame(&frame,1,1);
        meshes[0].object_distance=2000;frame.serial++;lhx_scene_metrics_record_frame(&frame,1,1);
        meshes[0].object_id=0;frame.serial++;lhx_scene_metrics_record_frame(&frame,1,1);
        meshes[0].object_id=1;meshes[0].object_distance=NAN;frame.serial++;
        lhx_scene_metrics_record_frame(&frame,1,1);
        meshes[0].object_distance=-1;frame.serial++;lhx_scene_metrics_record_frame(&frame,1,1);
        meshes[0].object_distance=INFINITY;frame.serial++;lhx_scene_metrics_record_frame(&frame,1,1);
        meshes[1].layer=1;meshes[1].object_distance=3000;frame.serial++;
        lhx_scene_metrics_record_frame(&frame,1,1);
    } else if(!strcmp(argv[1],"invalid")) {
        frame.views=NULL;lhx_scene_metrics_record_frame(&frame,1,2);
        frame.views=views;frame.view_count=1;frame.serial++;
        meshes[0].vertex_count=1;vertices_a[0].x=NAN;
        lhx_scene_metrics_record_frame(&frame,3,4);
    } else if(!strcmp(argv[1],"large")) {
        lhx_scene_metrics_record_frame(&frame,UINT64_MAX,UINT64_MAX);
    } else if(!strcmp(argv[1],"reinit")) {
        lhx_scene_metrics_record_frame(&frame,1,2);
        assert(lhx_scene_metrics_init(2)==1);
        lhx_scene_metrics_record_frame(&frame,100,20);
        assert(lhx_scene_metrics_init(LHX_SCENE_METRICS_MAX_SAMPLES+1)==0);
    } else if(!strcmp(argv[1],"io")) {
        assert(lhx_scene_metrics_report_json(NULL)==-1);
        assert(lhx_scene_metrics_report_csv(NULL)==-1);
        FILE *out=fopen("/dev/full","w");
        if(out){setvbuf(out,NULL,_IONBF,0);
            assert(lhx_scene_metrics_report_json(out)==-1);fclose(out);
            out=fopen("/dev/full","w");assert(out);setvbuf(out,NULL,_IONBF,0);
            assert(lhx_scene_metrics_report_csv(out)==-1);fclose(out);
        }
        lhx_scene_metrics_reset();return 0;
    } else assert(!strcmp(argv[1],"empty") || !strcmp(argv[1],"default"));
    if(argc==3) {
        FILE*out=fopen(argv[2],"w");assert(out);
        assert(lhx_scene_metrics_report_csv(out)==1);assert(fclose(out)==0);
    }
    assert(lhx_scene_metrics_report_json(stdout)==1);
    lhx_scene_metrics_reset();
    assert(!lhx_scene_metrics_enabled());
    assert(lhx_scene_metrics_report_json(stdout)==0);
    return 0;
}
'''


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class SceneMetrics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="lhx-metrics-")
        directory = Path(cls.temporary.name)
        source = directory / "check.c"
        source.write_text(HARNESS)
        cls.binary = directory / "check"
        cls.csv_path = directory / "report.csv"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "runtime"), str(source),
                        str(ROOT / "runtime/scene_metrics.c"), "-lm", "-o", str(cls.binary)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_case(self, name, csv_report=False):
        args = [str(self.binary), name]
        if csv_report:
            args.append(str(self.csv_path))
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout) if result.stdout else None

    def test_exact_known_stage_quantiles(self):
        data = self.run_case("known")
        self.assertEqual(data["host_draws"], 100)
        expected = {
            "render": (50.5, 50, 95, 100, 100),
            "composite": (101, 100, 190, 200, 2),
            "combined": (151.5, 151, 196, 201, 102),
        }
        for stage, values in expected.items():
            timing = data["stage_timing_us"][stage]
            self.assertEqual(timing["count"], 100)
            self.assertEqual(tuple(timing[key] for key in ("mean", "p50", "p95", "max", "last")), values)
        self.assertEqual(data["quantiles"], {"method": "nearest_rank", "sampling": "exact",
                                             "sample_count": 100, "sample_limit": 100})
        self.assertEqual(data["host_captures"], 10)
        self.assertEqual(data["stage_timing_us"]["capture"],
                         {"count": 10, "mean": 550, "p50": 500, "p95": 1000, "max": 1000, "last": 1000})

    def test_draw_counts_geometry_and_common_unit_distances(self):
        data = self.run_case("known")
        for key, value in {"views": 2, "meshes": 2, "primitives": 7, "vertices": 5}.items():
            self.assertEqual(data["geometry_per_draw"][key], {"mean": value, "max": value, "last": value})
        self.assertEqual(data["scene_draws"], 100)
        self.assertEqual(data["geometry_scans"], 2)
        self.assertEqual(data["repeated_scene_draws"], 98)
        self.assertEqual(data["scene_serial_changes"], 1)
        self.assertEqual((data["first_scene_serial"], data["last_scene_serial"]), (17, 18))
        distances = data["camera_distance_common_units"]
        self.assertEqual((distances["max_radial"], distances["max_forward_z"]), (100, 100))
        self.assertEqual((distances["mean_frame_max_radial"], distances["mean_frame_max_forward_z"]), (75, 75))
        self.assertNotIn("miles", json.dumps(data))

    def test_geometry_cache_uses_capture_serial(self):
        data = self.run_case("cache")
        self.assertEqual(data["geometry_scans"], 2)
        self.assertEqual(data["host_draws"], 3)
        distance = data["camera_distance_common_units"]
        self.assertAlmostEqual(distance["mean_frame_max_radial"], 20 / 3, places=5)
        self.assertEqual(distance["last_max_radial"], 10)

    def test_bounded_reservoir_is_explicit_and_deterministic(self):
        first = self.run_case("reservoir")
        self.assertEqual(first, self.run_case("reservoir"))
        self.assertEqual(first["host_draws"], 10000)
        self.assertEqual(first["quantiles"]["sample_count"], 8)
        self.assertEqual(first["quantiles"]["sampling"], "deterministic_reservoir")
        render = first["stage_timing_us"]["render"]
        self.assertEqual(render["mean"], 5000.5)
        self.assertEqual(render["max"], 10000)
        self.assertGreaterEqual(render["p50"], 1)
        self.assertLessEqual(render["p50"], render["p95"])
        self.assertLessEqual(render["p95"], 10000)
        capture_only = self.run_case("reservoir_capture_only")
        self.assertEqual(first["stage_timing_us"]["capture"], capture_only["stage_timing_us"]["capture"])
        self.assertEqual(first["capture_quantiles"]["sampling"], "deterministic_reservoir")
        self.assertEqual(first["capture_quantiles"]["sample_count"], 8)

    def test_csv_matches_json_and_has_consistent_columns(self):
        data = self.run_case("known", csv_report=True)
        rows = list(csv.DictReader(io.StringIO(self.csv_path.read_text())))
        self.assertTrue(all(None not in row for row in rows))
        by_metric = {row["metric"]: row for row in rows}
        for stage, timing in data["stage_timing_us"].items():
            row = by_metric[stage]
            self.assertEqual(row["unit"], "microseconds")
            self.assertEqual(int(row["samples"]), timing["count"])
            for key in ("mean", "p50", "p95", "max", "last"):
                self.assertEqual(float(row[key]), timing[key])
        self.assertEqual(by_metric["frame_max_radial"]["unit"], "common_camera_units")
        self.assertEqual(float(by_metric["frame_max_radial"]["max"]), 100)
        self.assertEqual(by_metric["quantile_sampling"]["last"], "exact")

    def test_disabled_and_empty_reports(self):
        self.assertIsNone(self.run_case("disabled"))
        data = self.run_case("empty")
        self.assertEqual(data["host_draws"], 0)
        self.assertEqual(data["quantiles"]["sample_count"], 0)
        self.assertIsNone(data["stage_timing_us"]["render"]["mean"])
        default = self.run_case("default")
        self.assertEqual(default["quantiles"]["sample_limit"], 262144)

    def test_reinitialize_and_invalid_limit_preserve_contract(self):
        data = self.run_case("reinit")
        self.assertEqual(data["host_draws"], 1)
        self.assertEqual(data["stage_timing_us"]["render"]["mean"], 100)
        self.assertEqual(data["quantiles"]["sample_limit"], 2)

    def test_invalid_geometry_and_nonfinite_coordinates_are_reported(self):
        data = self.run_case("invalid")
        self.assertEqual(data["invalid_scene_draws"], 1)
        self.assertEqual(data["invalid_vertex_observations"], 1)
        self.assertEqual(data["camera_distance_common_units"]["max_radial"], 0)

    def test_combined_duration_does_not_overflow_integer_sum(self):
        data = self.run_case("large")
        self.assertEqual(data["stage_timing_us"]["combined"]["max"], float(2 * (2**64 - 1)))

    def test_output_failures_are_reported(self):
        self.assertIsNone(self.run_case("io"))

    def test_capture_and_visibility_have_independent_denominators(self):
        data = self.run_case("independent", csv_report=True)
        self.assertEqual(data["host_draws"], 0)
        self.assertEqual(data["host_captures"], 3)
        self.assertEqual(data["main_view_visibility_samples"], 5)
        self.assertEqual(data["full_arena_count"], 2)
        self.assertEqual(data["stage_timing_us"]["capture"],
                         {"count": 3, "mean": 20, "p50": 20, "p95": 30, "max": 30, "last": 20})
        self.assertIsNone(data["stage_timing_us"]["combined"]["mean"])
        capture = data["capture_geometry_per_capture"]
        self.assertEqual(capture["considered"], {"mean": 200, "max": 300, "last": 300})
        self.assertEqual(capture["emitted"], {"mean": 20, "max": 30, "last": 30})
        self.assertEqual(capture["unsupported"], {"mean": 2, "max": 3, "last": 3})
        visibility = data["original_visibility_per_main_view"]
        self.assertEqual(visibility["candidates"], {"mean": 102, "max": 104, "last": 104})
        self.assertEqual(visibility["range_rejected"], {"mean": 34, "max": 38, "last": 38})
        self.assertEqual(visibility["arena_full"], {"mean": 0.4, "max": 1, "last": 0})
        rows = {row["metric"]: row for row in csv.DictReader(io.StringIO(self.csv_path.read_text()))}
        self.assertEqual(rows["capture"]["samples"], "3")
        self.assertEqual(rows["capture_considered"]["samples"], "3")
        self.assertEqual(rows["visibility_candidates"]["samples"], "5")
        self.assertEqual(rows["visibility_arena_full"]["mean"], "0.400")

    def test_object_origin_distance_excludes_background_and_invalid_meshes(self):
        data = self.run_case("origins")
        distance = data["camera_distance_common_units"]
        self.assertEqual(distance["max_object_origin"], 3000)
        self.assertEqual(distance["last_max_object_origin"], 3000)
        self.assertAlmostEqual(distance["mean_frame_max_object_origin"], 6500 / 7, places=5)
        self.assertEqual(distance["max_radial"], 50)


if __name__ == "__main__":
    unittest.main()
