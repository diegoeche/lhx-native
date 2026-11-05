#include "scene_metrics.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { RENDER, COMPOSITE, COMBINED, STAGES };
enum { VIEWS, MESHES, PRIMITIVES, VERTICES, COUNTS };
enum { CONSIDERED, EMITTED, UNSUPPORTED, CAPTURE_COUNTS };
enum { CANDIDATES, RANGE_REJECTED, FRUSTUM_REJECTED, LISTED, ARENA_FULL, VISIBILITY_COUNTS };
typedef struct { uint64_t render, composite; } Sample;
typedef struct { long double sum, max, last; } Timing;
typedef struct { long double sum; uint64_t max, last; } Count;
typedef struct {
    uint64_t counts[COUNTS], invalid_vertices;
    double radial, forward, object_origin;
    int invalid;
} Geometry;
typedef struct {
    Sample *samples;
    uint64_t *capture_samples;
    unsigned sample_limit, sample_count, capture_sample_count;
    uint64_t draws, scene_draws, serial_changes, repeated_scene_draws;
    uint64_t captures, visibility_samples, full_arenas, capture_random;
    uint64_t geometry_scans, invalid_scene_draws, invalid_vertices;
    uint64_t previous_serial, first_serial, last_serial, random;
    int has_serial;
    Timing timing[STAGES], radial, forward, object_origin;
    Timing capture_timing;
    Count counts[COUNTS];
    Count capture_counts[CAPTURE_COUNTS], visibility_counts[VISIBILITY_COUNTS];
    const LhxSceneFrame *cache_frame;
    const LhxSceneView *cache_views;
    uint64_t cache_serial;
    unsigned cache_view_count;
    int cache_valid;
    Geometry cache_geometry;
} Metrics;
typedef struct { long double p50, p95; } Quantiles;

static Metrics metrics;
static const char *const stage_names[STAGES] = {"render", "composite", "combined"};
static const char *const count_names[COUNTS] = {"views", "meshes", "primitives", "vertices"};
static const char *const capture_names[CAPTURE_COUNTS] = {"considered", "emitted", "unsupported"};
static const char *const visibility_names[VISIBILITY_COUNTS] = {
    "candidates", "range_rejected", "frustum_rejected", "listed", "arena_full"
};

void lhx_scene_metrics_reset(void)
{
    free(metrics.samples);
    free(metrics.capture_samples);
    memset(&metrics, 0, sizeof metrics);
}

int lhx_scene_metrics_init(unsigned sample_limit)
{
    if (!sample_limit)
        sample_limit = LHX_SCENE_METRICS_DEFAULT_SAMPLES;
    if (sample_limit > LHX_SCENE_METRICS_MAX_SAMPLES)
        return 0;
    Sample *samples = malloc((size_t)sample_limit * sizeof *samples);
    uint64_t *capture_samples = malloc((size_t)sample_limit * sizeof *capture_samples);
    if (!samples || !capture_samples) {
        free(samples);
        free(capture_samples);
        return 0;
    }
    lhx_scene_metrics_reset();
    metrics.samples = samples;
    metrics.capture_samples = capture_samples;
    metrics.sample_limit = sample_limit;
    /* Independent of the original game's random-number state. */
    metrics.random = UINT64_C(0x6c68782d6d657472);
    metrics.capture_random = UINT64_C(0x6361707475726521);
    return 1;
}

int lhx_scene_metrics_enabled(void)
{
    return metrics.samples != NULL;
}

static uint64_t random64(uint64_t *state)
{
    uint64_t n = *state;
    n ^= n >> 12;
    n ^= n << 25;
    n ^= n >> 27;
    *state = n;
    return n * UINT64_C(2685821657736338717);
}

static uint64_t random_below(uint64_t *state, uint64_t bound)
{
    /* Rejection removes modulo bias from Algorithm R's reservoir selection. */
    uint64_t threshold = (uint64_t)(-bound) % bound;
    uint64_t n;
    do {
        n = random64(state);
    } while (n < threshold);
    return n % bound;
}

static Geometry scan_geometry(const LhxSceneFrame *frame)
{
    Geometry geometry = {0};
    if (!frame)
        return geometry;
    geometry.counts[VIEWS] = frame->view_count;
    if (frame->view_count && !frame->views) {
        geometry.invalid = 1;
        return geometry;
    }
    double max_radius_squared = 0.0;
    int has_forward = 0;
    for (unsigned v = 0; v < frame->view_count; v++) {
        const LhxSceneView *view = &frame->views[v];
        geometry.counts[MESHES] += view->mesh_count;
        if (view->mesh_count && !view->meshes) {
            geometry.invalid = 1;
            continue;
        }
        for (unsigned m = 0; m < view->mesh_count; m++) {
            const LhxSceneMesh *mesh = &view->meshes[m];
            if (mesh->layer != 0 && mesh->object_id &&
                isfinite(mesh->object_distance) && mesh->object_distance >= 0.0f &&
                mesh->object_distance > geometry.object_origin)
                geometry.object_origin = mesh->object_distance;
            geometry.counts[PRIMITIVES] += mesh->primitive_count;
            geometry.counts[VERTICES] += mesh->vertex_count;
            if (mesh->vertex_count && !mesh->vertices) {
                geometry.invalid = 1;
                continue;
            }
            for (unsigned i = 0; i < mesh->vertex_count; i++) {
                const LhxSceneVertex *p = &mesh->vertices[i];
                if (!isfinite(p->x) || !isfinite(p->y) || !isfinite(p->z)) {
                    geometry.invalid_vertices++;
                    continue;
                }
                double x = p->x, y = p->y, z = p->z;
                double squared = x * x + y * y + z * z;
                if (squared > max_radius_squared)
                    max_radius_squared = squared;
                if (!has_forward || z > geometry.forward)
                    geometry.forward = z;
                has_forward = 1;
            }
        }
    }
    /* One square root per captured scene, rather than one per vertex. */
    geometry.radial = sqrt(max_radius_squared);
    return geometry;
}

static Geometry geometry_for(const LhxSceneFrame *frame)
{
    if (frame && metrics.cache_valid && metrics.cache_frame == frame &&
        metrics.cache_serial == frame->serial && metrics.cache_views == frame->views &&
        metrics.cache_view_count == frame->view_count)
        return metrics.cache_geometry;
    Geometry geometry = scan_geometry(frame);
    metrics.geometry_scans++;
    metrics.cache_frame = frame;
    metrics.cache_serial = frame ? frame->serial : 0;
    metrics.cache_views = frame ? frame->views : NULL;
    metrics.cache_view_count = frame ? frame->view_count : 0;
    metrics.cache_geometry = geometry;
    metrics.cache_valid = frame != NULL;
    return geometry;
}

static void add_timing(Timing *timing, long double value, uint64_t count)
{
    timing->sum += value;
    if (count == 1 || value > timing->max)
        timing->max = value;
    timing->last = value;
}

void lhx_scene_metrics_record_frame(const LhxSceneFrame *frame,
                                    uint64_t render_us, uint64_t composite_us)
{
    if (!metrics.samples)
        return;
    metrics.draws++;
    add_timing(&metrics.timing[RENDER], render_us, metrics.draws);
    add_timing(&metrics.timing[COMPOSITE], composite_us, metrics.draws);
    add_timing(&metrics.timing[COMBINED], (long double)render_us + composite_us,
                metrics.draws);
    Sample sample = {render_us, composite_us};
    if (metrics.sample_count < metrics.sample_limit) {
        metrics.samples[metrics.sample_count++] = sample;
    } else {
        uint64_t slot = random_below(&metrics.random, metrics.draws);
        if (slot < metrics.sample_limit)
            metrics.samples[slot] = sample;
    }
    if (frame && frame->view_count) {
        metrics.scene_draws++;
        if (!metrics.has_serial) {
            metrics.first_serial = frame->serial;
            metrics.has_serial = 1;
        } else if (metrics.previous_serial == frame->serial) {
            metrics.repeated_scene_draws++;
        } else {
            metrics.serial_changes++;
        }
        metrics.previous_serial = frame->serial;
        metrics.last_serial = frame->serial;
    }
    Geometry geometry = geometry_for(frame);
    metrics.invalid_scene_draws += geometry.invalid != 0;
    metrics.invalid_vertices += geometry.invalid_vertices;
    for (unsigned i = 0; i < COUNTS; i++) {
        Count *count = &metrics.counts[i];
        count->sum += geometry.counts[i];
        if (geometry.counts[i] > count->max)
            count->max = geometry.counts[i];
        count->last = geometry.counts[i];
    }
    add_timing(&metrics.radial, geometry.radial, metrics.draws);
    add_timing(&metrics.forward, geometry.forward, metrics.draws);
    add_timing(&metrics.object_origin, geometry.object_origin, metrics.draws);
}

static void add_count(Count *count, uint64_t value)
{
    count->sum += value;
    if (value > count->max)
        count->max = value;
    count->last = value;
}

void lhx_scene_metrics_record_capture(uint64_t elapsed_us, unsigned considered,
                                      unsigned emitted, unsigned unsupported)
{
    if (!metrics.samples)
        return;
    metrics.captures++;
    add_timing(&metrics.capture_timing, elapsed_us, metrics.captures);
    if (metrics.capture_sample_count < metrics.sample_limit) {
        metrics.capture_samples[metrics.capture_sample_count++] = elapsed_us;
    } else {
        uint64_t slot = random_below(&metrics.capture_random, metrics.captures);
        if (slot < metrics.sample_limit)
            metrics.capture_samples[slot] = elapsed_us;
    }
    add_count(&metrics.capture_counts[CONSIDERED], considered);
    add_count(&metrics.capture_counts[EMITTED], emitted);
    add_count(&metrics.capture_counts[UNSUPPORTED], unsupported);
}

void lhx_scene_metrics_record_visibility(unsigned candidates, unsigned range_rejected,
                                         unsigned frustum_rejected, unsigned listed,
                                         int arena_full)
{
    if (!metrics.samples)
        return;
    metrics.visibility_samples++;
    metrics.full_arenas += arena_full != 0;
    add_count(&metrics.visibility_counts[CANDIDATES], candidates);
    add_count(&metrics.visibility_counts[RANGE_REJECTED], range_rejected);
    add_count(&metrics.visibility_counts[FRUSTUM_REJECTED], frustum_rejected);
    add_count(&metrics.visibility_counts[LISTED], listed);
    add_count(&metrics.visibility_counts[ARENA_FULL], arena_full != 0);
}

static int compare_time(const void *a, const void *b)
{
    long double x = *(const long double *)a, y = *(const long double *)b;
    return (x > y) - (x < y);
}

static void sorted_quantiles(long double *sorted, unsigned count, Quantiles *q)
{
    if (!count)
        return;
    qsort(sorted, count, sizeof *sorted, compare_time);
    /* Nearest-rank percentiles: ceil(percent * N) - 1. */
    q->p50 = sorted[(count * 50u + 99u) / 100u - 1u];
    q->p95 = sorted[(count * 95u + 99u) / 100u - 1u];
}

static int quantiles(Quantiles result[STAGES], Quantiles *capture)
{
    memset(result, 0, STAGES * sizeof *result);
    memset(capture, 0, sizeof *capture);
    unsigned count = metrics.sample_count;
    unsigned capacity = count > metrics.capture_sample_count ? count : metrics.capture_sample_count;
    if (!capacity)
        return 1;
    long double *sorted = malloc((size_t)capacity * sizeof *sorted);
    if (!sorted)
        return 0;
    for (unsigned stage = 0; stage < STAGES; stage++) {
        for (unsigned i = 0; i < count; i++) {
            const Sample *sample = &metrics.samples[i];
            sorted[i] = stage == RENDER ? sample->render :
                        stage == COMPOSITE ? sample->composite :
                        (long double)sample->render + sample->composite;
        }
        sorted_quantiles(sorted, count, &result[stage]);
    }
    for (unsigned i = 0; i < metrics.capture_sample_count; i++)
        sorted[i] = metrics.capture_samples[i];
    sorted_quantiles(sorted, metrics.capture_sample_count, capture);
    free(sorted);
    return 1;
}

static const char *sampling(void)
{
    return metrics.draws > metrics.sample_count ? "deterministic_reservoir" : "exact";
}

static const char *capture_sampling(void)
{
    return metrics.captures > metrics.capture_sample_count ? "deterministic_reservoir" : "exact";
}

static long double mean(long double sum)
{
    return metrics.draws ? sum / metrics.draws : 0.0L;
}

static void json_timing(FILE *out, const char *name, const Timing *t,
                         uint64_t count, Quantiles q)
{
    fprintf(out, "    \"%s\": {\"count\": %" PRIu64 ", ", name, count);
    if (count)
        fprintf(out, "\"mean\": %.3Lf, \"p50\": %.3Lf, \"p95\": %.3Lf, "
                     "\"max\": %.3Lf, \"last\": %.3Lf}",
                t->sum / count, q.p50, q.p95, t->max, t->last);
    else
        fputs("\"mean\": null, \"p50\": null, \"p95\": null, \"max\": null, \"last\": null}", out);
}

static void json_counts(FILE *out, const char *section, const char *const *names,
                         const Count *counts, unsigned size, uint64_t samples)
{
    fprintf(out, "  \"%s\": {\n", section);
    for (unsigned i = 0; i < size; i++) {
        const Count *c = &counts[i];
        fprintf(out, "    \"%s\": {\"mean\": %.3Lf, \"max\": %" PRIu64 ", \"last\": %" PRIu64 "}%s\n",
                names[i], samples ? c->sum / samples : 0.0L, c->max, c->last,
                i + 1 < size ? "," : "");
    }
    fputs("  }", out);
}

int lhx_scene_metrics_report_json(FILE *out)
{
    if (!out)
        return -1;
    if (!metrics.samples)
        return 0;
    Quantiles q[STAGES], capture;
    if (!quantiles(q, &capture))
        return -1;
    fprintf(out, "{\n  \"schema_version\": 1,\n"
                 "  \"timing_source\": \"caller supplied host elapsed microseconds\",\n"
                 "  \"host_draws\": %" PRIu64 ",\n"
                 "  \"host_captures\": %" PRIu64 ",\n"
                 "  \"main_view_visibility_samples\": %" PRIu64 ",\n"
                 "  \"full_arena_count\": %" PRIu64 ",\n"
                 "  \"scene_draws\": %" PRIu64 ",\n"
                 "  \"scene_serial_changes\": %" PRIu64 ",\n"
                 "  \"repeated_scene_draws\": %" PRIu64 ",\n"
                 "  \"first_scene_serial\": %" PRIu64 ",\n"
                 "  \"last_scene_serial\": %" PRIu64 ",\n"
                 "  \"geometry_scans\": %" PRIu64 ",\n"
                 "  \"invalid_scene_draws\": %" PRIu64 ",\n"
                 "  \"invalid_vertex_observations\": %" PRIu64 ",\n",
            metrics.draws, metrics.captures, metrics.visibility_samples, metrics.full_arenas,
            metrics.scene_draws, metrics.serial_changes,
            metrics.repeated_scene_draws, metrics.first_serial, metrics.last_serial,
            metrics.geometry_scans, metrics.invalid_scene_draws, metrics.invalid_vertices);
    fprintf(out, "  \"quantiles\": {\"method\": \"nearest_rank\", \"sampling\": \"%s\", "
                 "\"sample_count\": %u, \"sample_limit\": %u},\n"
                 "  \"capture_quantiles\": {\"method\": \"nearest_rank\", \"sampling\": \"%s\", "
                 "\"sample_count\": %u, \"sample_limit\": %u},\n"
                 "  \"stage_timing_us\": {\n", sampling(), metrics.sample_count, metrics.sample_limit,
            capture_sampling(), metrics.capture_sample_count, metrics.sample_limit);
    for (unsigned i = 0; i < STAGES; i++) {
        json_timing(out, stage_names[i], &metrics.timing[i], metrics.draws, q[i]);
        fputs(",\n", out);
    }
    json_timing(out, "capture", &metrics.capture_timing, metrics.captures, capture);
    fputs("\n  },\n", out);
    json_counts(out, "geometry_per_draw", count_names, metrics.counts, COUNTS, metrics.draws);
    fputs(",\n", out);
    json_counts(out, "capture_geometry_per_capture", capture_names, metrics.capture_counts,
                  CAPTURE_COUNTS, metrics.captures);
    fputs(",\n", out);
    json_counts(out, "original_visibility_per_main_view", visibility_names,
                  metrics.visibility_counts, VISIBILITY_COUNTS, metrics.visibility_samples);
    fprintf(out, ",\n  \"camera_distance_common_units\": {\n"
                 "    \"scope\": \"captured camera-space vertices; sphere vertices are centers\",\n"
                 "    \"object_origin_scope\": \"captured non-background meshes with nonzero object_id and finite nonnegative origin distance\",\n"
                 "    \"max_radial\": %.6Lf, \"mean_frame_max_radial\": %.6Lf, \"last_max_radial\": %.6Lf,\n"
                 "    \"max_forward_z\": %.6Lf, \"mean_frame_max_forward_z\": %.6Lf, \"last_max_forward_z\": %.6Lf,\n"
                 "    \"max_object_origin\": %.6Lf, \"mean_frame_max_object_origin\": %.6Lf, \"last_max_object_origin\": %.6Lf\n"
                 "  }\n}\n",
            metrics.radial.max, mean(metrics.radial.sum), metrics.radial.last,
            metrics.forward.max, mean(metrics.forward.sum), metrics.forward.last,
            metrics.object_origin.max, mean(metrics.object_origin.sum), metrics.object_origin.last);
    return ferror(out) ? -1 : 1;
}

static void csv_timing(FILE *out, const char *name, const Timing *t,
                        uint64_t count, Quantiles q)
{
    fprintf(out, "%s,microseconds,%" PRIu64, name, count);
    if (count)
        fprintf(out, ",%.3Lf,%.3Lf,%.3Lf,%.3Lf,%.3Lf\n",
                t->sum / count, q.p50, q.p95, t->max, t->last);
    else
        fputs(",,,,,\n", out);
}

static void csv_counts(FILE *out, const char *prefix, const char *const *names,
                        const Count *counts, unsigned size, uint64_t samples)
{
    for (unsigned i = 0; i < size; i++) {
        const Count *c = &counts[i];
        fprintf(out, "%s%s,count,%" PRIu64 ",%.3Lf,,,%" PRIu64 ",%" PRIu64 "\n",
                prefix, names[i], samples, samples ? c->sum / samples : 0.0L, c->max, c->last);
    }
}

int lhx_scene_metrics_report_csv(FILE *out)
{
    if (!out)
        return -1;
    if (!metrics.samples)
        return 0;
    Quantiles q[STAGES], capture;
    if (!quantiles(q, &capture))
        return -1;
    fputs("metric,unit,samples,mean,p50,p95,max,last\n", out);
    fprintf(out, "host_draws,count,,,,,,%" PRIu64 "\n"
                 "host_captures,count,,,,,,%" PRIu64 "\n"
                 "main_view_visibility_samples,count,,,,,,%" PRIu64 "\n"
                 "full_arena_count,count,,,,,,%" PRIu64 "\n"
                 "scene_draws,count,,,,,,%" PRIu64 "\n"
                 "scene_serial_changes,count,,,,,,%" PRIu64 "\n"
                 "repeated_scene_draws,count,,,,,,%" PRIu64 "\n"
                 "geometry_scans,count,,,,,,%" PRIu64 "\n"
                 "invalid_scene_draws,count,,,,,,%" PRIu64 "\n"
                 "invalid_vertex_observations,count,,,,,,%" PRIu64 "\n"
                 "quantile_sampling,method,,,,,,%s\n"
                 "quantile_sample_count,count,,,,,,%u\n"
                 "quantile_sample_limit,count,,,,,,%u\n"
                 "capture_quantile_sampling,method,,,,,,%s\n"
                 "capture_quantile_sample_count,count,,,,,,%u\n",
            metrics.draws, metrics.captures, metrics.visibility_samples, metrics.full_arenas,
            metrics.scene_draws, metrics.serial_changes,
            metrics.repeated_scene_draws, metrics.geometry_scans,
            metrics.invalid_scene_draws, metrics.invalid_vertices,
            sampling(), metrics.sample_count, metrics.sample_limit,
            capture_sampling(), metrics.capture_sample_count);
    for (unsigned i = 0; i < STAGES; i++)
        csv_timing(out, stage_names[i], &metrics.timing[i], metrics.draws, q[i]);
    csv_timing(out, "capture", &metrics.capture_timing, metrics.captures, capture);
    csv_counts(out, "", count_names, metrics.counts, COUNTS, metrics.draws);
    csv_counts(out, "capture_", capture_names, metrics.capture_counts, CAPTURE_COUNTS, metrics.captures);
    csv_counts(out, "visibility_", visibility_names, metrics.visibility_counts,
                   VISIBILITY_COUNTS, metrics.visibility_samples);
    const Timing *distances[] = {&metrics.radial, &metrics.forward, &metrics.object_origin};
    const char *names[] = {"frame_max_radial", "frame_max_forward_z", "frame_max_object_origin"};
    for (unsigned i = 0; i < 3; i++) {
        const Timing *t = distances[i];
        fprintf(out, "%s,common_camera_units,%" PRIu64 ",%.6Lf,,,%.6Lf,%.6Lf\n",
                names[i], metrics.draws, mean(t->sum), t->max, t->last);
    }
    return ferror(out) ? -1 : 1;
}
