/* Capture the original engine's transformed models; no runtime code decoding.
 * Addresses/layouts are covered by the lifter's exact executable digest.
 * Reference: lhx-attack-chopper-gpu/src/{capture,composite}.js. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "scene_capture.h"
#include "scene_composite.h"
#include "scene_metrics.h"
#include "scene_world.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { MAX_VIEWS = 16, MAX_MESHES = 4096, MAX_HOLDS = 8 };
#define MAX_CAPTURE_BYTES ((size_t)64 * 1024 * 1024)
uint32_t g_lhx_scene_filter_lo, g_lhx_scene_filter_hi;
static LhxSceneView pending[MAX_VIEWS], shown[MAX_VIEWS];
static LhxSceneView primary_key;
static unsigned pending_count, shown_count, pending_capacity[MAX_VIEWS];
static unsigned pending_world_count[MAX_VIEWS];
static uint64_t capture_sequence, pending_sequence[MAX_VIEWS], shown_sequence[MAX_VIEWS];
static LhxSceneFrame frame = {.views = shown};
static struct { uint32_t lo, hi; } holds[MAX_HOLDS];
static unsigned hold_count;
static int failed, scene_active, has_primary;
static size_t allocated_bytes;
typedef union { max_align_t alignment; size_t bytes; } Allocation;

static void invalid(CPU *cpu, const char *reason) {
  failed = 1;
  scene_active = 0;
  g_lhx_scene_filter_lo = g_lhx_scene_filter_hi = 0;
  recomp_unhandled(cpu, cpu->cs, cpu->ip, reason);
}
static void *allocate(CPU *cpu, size_t count, size_t size) {
  if (!count) return NULL;
  if (size && count > (MAX_CAPTURE_BYTES - allocated_bytes) / size) {
    invalid(cpu, "native scene capture exceeds 64 MiB bound"); return NULL;
  }
  size_t bytes = count * size;
  Allocation *p = calloc(1, sizeof(*p) + bytes);
  if (!p) { invalid(cpu, "native scene capture allocation failed"); return NULL; }
  p->bytes = bytes;
  allocated_bytes += bytes;
  return p + 1;
}
static void release(void *p) {
  if (!p) return;
  Allocation *a = (Allocation *)p - 1;
  allocated_bytes -= a->bytes;
  free(a);
}
static void free_mesh(LhxSceneMesh *mesh) {
  for (unsigned i = 0; i < mesh->primitive_count; ++i)
    release(mesh->primitives[i].indices);
  release(mesh->vertices);
  release(mesh->primitives);
  memset(mesh, 0, sizeof(*mesh));
}
static void free_view(LhxSceneView *view) {
  for (unsigned i = 0; i < view->mesh_count; ++i) free_mesh(view->meshes + i);
  release(view->meshes);
  memset(view, 0, sizeof(*view));
}
void lhx_scene_reset(void) {
  for (unsigned i = 0; i < pending_count; ++i) free_view(pending + i);
  for (unsigned i = 0; i < shown_count; ++i) free_view(shown + i);
  pending_count = shown_count = hold_count = 0;
  capture_sequence = 0;
  memset(pending_sequence, 0, sizeof(pending_sequence));
  memset(shown_sequence, 0, sizeof(shown_sequence));
  memset(pending_capacity, 0, sizeof(pending_capacity));
  memset(pending_world_count, 0, sizeof(pending_world_count));
  frame = (LhxSceneFrame){.views = shown};
  failed = scene_active = has_primary = 0;
  memset(&primary_key, 0, sizeof(primary_key));
  g_lhx_scene_filter_lo = g_lhx_scene_filter_hi = 0;
}
const LhxSceneFrame *lhx_scene_frame(void) { return &frame; }

static int readable(CPU *cpu, uint16_t segment, unsigned offset, unsigned bytes) {
  if (offset > 65536 || bytes > 65536 - offset ||
      ((uint32_t)segment << 4) + offset + bytes > MEM_SIZE) {
    invalid(cpu, "native scene data exceeds segment/memory bounds"); return 0;
  }
  return 1;
}
static uint16_t word(CPU *cpu, unsigned offset) {
  return mem_read16(cpu, cpu->ds, (uint16_t)offset);
}
static int32_t signed32(CPU *cpu, uint16_t segment, unsigned offset) {
  return (int32_t)mem_read32(cpu, segment, (uint16_t)offset);
}
static int view_key(CPU *cpu, LhxSceneView *view) {
  int x = word(cpu, 0xeb5a), width = word(cpu, 0xeb58) * 2;
  int y = word(cpu, 0xeed1), bottom = word(cpu, 0xeed3);
  if (x < 0 || width <= 0 || x + width > 320 || y < 0 ||
      y > bottom || bottom >= 200) {
    invalid(cpu, "native scene window outside 320x200 framebuffer"); return 0;
  }
  unsigned focus_shift = word(cpu, 0xf02f);
  if (focus_shift > 30) {
    invalid(cpu, "native scene projection shift exceeds bound"); return 0;
  }
  *view = (LhxSceneView){.x0=x, .y0=y, .x1=x+width, .y1=bottom+1,
    .cx=(int16_t)word(cpu, 0xeb52), .cy=(int16_t)word(cpu, 0xeb54),
    .focus=ldexpf(1.0f, (int)focus_shift)};
  return 1;
}
static int same_view(const LhxSceneView *a, const LhxSceneView *b) {
  return a->x0 == b->x0 && a->y0 == b->y0 && a->x1 == b->x1 &&
         a->y1 == b->y1 && a->cx == b->cx && a->cy == b->cy;
}
static int find_view(const LhxSceneView *views, unsigned count,
                     const LhxSceneView *key) {
  for (unsigned i = 0; i < count; ++i)
    if (same_view(views + i, key)) return (int)i;
  return -1;
}
static int find_rectangle(const LhxSceneView *views, unsigned count,
                          const LhxSceneView *key) {
  for (unsigned i = 0; i < count; ++i)
    if (views[i].x0 == key->x0 && views[i].y0 == key->y0 &&
        views[i].x1 == key->x1 && views[i].y1 == key->y1) return (int)i;
  return -1;
}
static void append_mesh(CPU *cpu, unsigned index, LhxSceneMesh *mesh);
typedef struct { CPU *cpu; unsigned view; } WorldCapture;
static void *world_allocate(void *context, size_t count, size_t size) {
  return allocate(((WorldCapture *)context)->cpu,count,size);
}
static void world_release(void *context, void *pointer) {
  (void)context;
  release(pointer);
}
static int world_emit(void *context, uint32_t object_id, LhxSceneMesh *mesh) {
  WorldCapture *capture=context;
  mesh->object_id=object_id;
  append_mesh(capture->cpu,capture->view,mesh);
  return 1; /* append_mesh adopts or releases every allocation. */
}
static uint64_t capture_time_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
}
static void after_horizon(CPU *cpu) {
  LhxSceneView key;
  if (!view_key(cpu, &key)) return;
  int index = find_rectangle(pending, pending_count, &key);
  if (index < 0) {
    if (pending_count == MAX_VIEWS) {
      invalid(cpu, "native scene exceeds 16 pending windows"); return;
    }
    index = (int)pending_count++;
  }
  free_view(pending + index);
  pending_capacity[index] = 0;
  pending_world_count[index] = 0;
  pending[index] = key;
  pending_sequence[index] = ++capture_sequence;
  lhx_scene_read_horizon(cpu, pending + index);
  lhx_scene_world_environment(cpu, pending + index);
  if (failed) return;
  if (lhx_scene_metrics_enabled() && key.x0==0 && key.x1==320) {
    /* Discovery has finished; the later persistent-cache trim can overwrite
     * the byte-sized arena flag with 2, so sample it before model drawing. */
    unsigned end=(unsigned)word(cpu,0xf031)+word(cpu,0xf033);
    unsigned cursor=word(cpu,0xf1f6);
    unsigned listed=end>=cursor+22 ? (end-cursor-22)/22 : 0;
    lhx_scene_metrics_record_visibility(word(cpu,0xf061),word(cpu,0xf063),
        word(cpu,0xf065),listed,mem_read8(cpu,cpu->ds,0xf0a1)==1);
  }
  if (g_lhx_scene_world_enabled && key.x0==0 && key.x1==320) {
    WorldCapture capture={cpu,(unsigned)index};
    LhxSceneWorldStats stats={0};
    uint64_t start=lhx_scene_metrics_enabled() ? capture_time_us() : 0;
    if (!lhx_scene_world_capture(cpu,pending+index,world_allocate,world_release,
                                 world_emit,&capture,&stats)) {
      if (!failed) invalid(cpu,"native world capture failed");
      return;
    }
    if (lhx_scene_metrics_enabled())
      lhx_scene_metrics_record_capture(capture_time_us()-start,stats.considered,
                                       stats.emitted,stats.unsupported);
    if (failed) return;
    pending_world_count[index]=pending[index].mesh_count;
  }
  uint16_t segment = word(cpu, 0xecfe);
  unsigned first = word(cpu, 0xeb6e + key.y0 * 2);
  unsigned last = first;
  for (int y = key.y0; y < key.y1; ++y) {
    unsigned row = word(cpu, 0xeb6e + y * 2);
    if (row != first + (unsigned)(y - key.y0) * 320 ||
        !readable(cpu, segment, row, 320)) {
      if (!failed) invalid(cpu, "native scene rows are not contiguous 320-byte lines");
      return;
    }
    memset(cpu->mem + ((uint32_t)segment << 4) + row + key.x0,
           255, (size_t)(key.x1 - key.x0));
    mem_write8(cpu, cpu->ds, (uint16_t)(0xed3c + y), 255);
    last = row;
  }
  g_lhx_scene_filter_lo = ((uint32_t)segment << 4) + first;
  g_lhx_scene_filter_hi = ((uint32_t)segment << 4) + last + 320;
  hold_count = 0;
  scene_active = 1;
}
static void append_mesh(CPU *cpu, unsigned index, LhxSceneMesh *mesh) {
  LhxSceneView *view = pending + index;
  if (view->mesh_count == MAX_MESHES) {
    free_mesh(mesh); invalid(cpu, "native scene exceeds 4096 meshes per window"); return;
  }
  if (view->mesh_count == pending_capacity[index]) {
    unsigned capacity = pending_capacity[index] ? pending_capacity[index] * 2 : 16;
    LhxSceneMesh *meshes = allocate(cpu, capacity, sizeof(*meshes));
    if (!meshes) { free_mesh(mesh); return; }
    if (view->mesh_count)
      memcpy(meshes, view->meshes, view->mesh_count * sizeof(*meshes));
    release(view->meshes);
    view->meshes = meshes;
    pending_capacity[index] = capacity;
  }
  view->meshes[view->mesh_count++] = *mesh;
}
static void capture_model(CPU *cpu) {
  if (!scene_active) return;
  LhxSceneView key;
  if (!view_key(cpu, &key)) return;
  int index = find_view(pending, pending_count, &key);
  if (index < 0) return; /* A non-scene utility draw has no captured horizon. */
  uint16_t object=word(cpu,0xf201), segment=word(cpu,0xf203);
  uint32_t object_id=seg_off(segment,object);
  for (unsigned i=0;i<pending_world_count[index];++i)
    if (pending[index].meshes[i].object_id==object_id) return;
  unsigned lod = word(cpu, 0xf209), record = word(cpu, 0xf207);
  unsigned points = word(cpu, 0x070e);
  if (!readable(cpu, cpu->ds, lod, 14) ||
      !readable(cpu, cpu->ds, record, 0x44)) return;
  unsigned count = mem_read8(cpu, cpu->ds, (uint16_t)lod);
  unsigned vertices = mem_read8(cpu, cpu->ds, (uint16_t)(lod + 1));
  if (!vertices || !count) return;
  if (!readable(cpu, cpu->ds, points, vertices * 16)) return;
  int model_shift = (int8_t)mem_read8(cpu, cpu->ds, (uint16_t)(record + 12));
  unsigned scale_shift = mem_read8(cpu, cpu->ds, 0xf1ff);
  if (model_shift < -30 || model_shift > 30 || scale_shift > 30) {
    invalid(cpu, "native model unit/precision shift exceeds bound"); return;
  }
  float unit = ldexpf(1.0f, model_shift - (int)scale_shift);
  LhxSceneMesh mesh = {.vertex_count=vertices};
  mesh.vertices = allocate(cpu, vertices, sizeof(*mesh.vertices));
  mesh.primitives = allocate(cpu, count, sizeof(*mesh.primitives));
  if (failed) { free_mesh(&mesh); return; }
  for (unsigned i = 0; i < vertices; ++i) {
    unsigned p = points + i * 16;
    mesh.vertices[i] = (LhxSceneVertex){
      (int16_t)word(cpu,p+2)*unit, (int16_t)word(cpu,p+6)*unit,
      (int16_t)word(cpu,p+10)*unit};
  }
  unsigned p = lod + 14;
  int points_only = 1;
  for (unsigned i = 0; i < count; ++i) {
    if (!readable(cpu, cpu->ds, p, 8)) { free_mesh(&mesh); return; }
    unsigned type = mem_read8(cpu, cpu->ds, (uint16_t)p), kind = type & 7;
    unsigned n, indices_at, bytes;
    if (kind == 0) {
      n = mem_read8(cpu,cpu->ds,(uint16_t)(p+6)); indices_at=p+7; bytes=7+n;
      if (n < 3) { free_mesh(&mesh); invalid(cpu,"native polygon has fewer than 3 vertices"); return; }
    } else if (kind == 1) { n=2; indices_at=p+5; bytes=7; }
    else if (kind == 2) { n=1; indices_at=p+5; bytes=6; }
    else if (kind == 3) { n=1; indices_at=p+7; bytes=8; }
    else if (kind == 4) { p+=8; continue; } /* Keep original 2D effect calls. */
    else { free_mesh(&mesh); invalid(cpu,"unsupported native scene primitive kind"); return; }
    if (!readable(cpu,cpu->ds,p,bytes)) { free_mesh(&mesh); return; }
    LhxScenePrimitive *primitive = mesh.primitives + mesh.primitive_count++;
    *primitive = (LhxScenePrimitive){.kind=kind,
      .color=mem_read8(cpu,cpu->ds,(uint16_t)(p+3)), .count=n,
      .one_sided=kind==0 && (type & 0x10),
      .stipple=kind==0 && mem_read8(cpu,cpu->ds,(uint16_t)(p+4))!=255};
    primitive->indices = allocate(cpu,n,sizeof(*primitive->indices));
    if (!primitive->indices) { free_mesh(&mesh); return; }
    for (unsigned j=0;j<n;++j) {
      unsigned v=mem_read8(cpu,cpu->ds,(uint16_t)(indices_at+j));
      if (v >= vertices) { free_mesh(&mesh); invalid(cpu,"native primitive vertex index exceeds point count"); return; }
      primitive->indices[j]=(uint16_t)v;
    }
    if (kind==3)
      primitive->radius=mem_read16(cpu,cpu->ds,(uint16_t)(p+5))*ldexpf(1.0f,model_shift);
    if (kind < 2) points_only=0;
    p+=bytes;
  }
  if (!readable(cpu,segment,object,0x12)) { free_mesh(&mesh); return; }
  mesh.object_id=object_id;
  double dx=((double)signed32(cpu,segment,object+6)-signed32(cpu,cpu->ds,0xf01b))/256.0;
  double dy=((double)signed32(cpu,segment,object+10)-signed32(cpu,cpu->ds,0xf01f))/256.0;
  double dz=((double)signed32(cpu,segment,object+14)-signed32(cpu,cpu->ds,0xf023))/256.0;
  mesh.object_distance=(float)hypot(hypot(dx,dy),dz);
  int32_t bottom=signed32(cpu,cpu->ds,record+0x34);
  int32_t top=signed32(cpu,cpu->ds,record+0x38);
  mesh.layer=points_only ? 0 : bottom==top && signed32(cpu,segment,object+0x0a)==0 ? 1 : 2;
  lhx_scene_world_surface(cpu,record,segment,object,&mesh);
  if (mesh.primitive_count) append_mesh(cpu,(unsigned)index,&mesh);
  else free_mesh(&mesh);
}
static void publish(CPU *cpu) {
  LhxSceneView key;
  if (!view_key(cpu,&key)) return;
  int from=find_view(pending,pending_count,&key);
  if (from<0) return;
  uint64_t sequence=pending_sequence[from];
  if (key.x0==0 && key.x1==320 &&
      (!has_primary || !same_view(&primary_key,&key))) {
    /* Cockpit and chase views have different centres/heights. A transition
     * retires the old family, but ordinary primary redraws must retain cached
     * instruments: the original attitude/TV views update less frequently. Keep
     * auxiliary views already captured after this primary began. */
    for (unsigned i=0;i<shown_count;) {
      if (shown_sequence[i] < sequence) {
        free_view(shown+i);
        --shown_count;
        memmove(shown+i,shown+i+1,(shown_count-i)*sizeof(*shown));
        memmove(shown_sequence+i,shown_sequence+i+1,
                (shown_count-i)*sizeof(*shown_sequence));
        memset(shown+shown_count,0,sizeof(*shown));
      } else ++i;
    }
    primary_key=key;
    has_primary=1;
  }
  /* A resized main window replaces its previous geometry in the same slot. */
  /* The briefing map scrolls its projection centre through a fixed physical
   * window. That remains one displayed viewport, not a new persistent view
   * for every map position. Capture still matches the complete context. */
  int to=find_rectangle(shown,shown_count,&key);
  if (to<0) {
    for (unsigned i=0;i<shown_count;++i)
      if (shown[i].cx==key.cx && shown[i].cy==key.cy) { to=(int)i; break; }
  }
  if (to<0) {
    if (shown_count==MAX_VIEWS) { invalid(cpu,"native scene exceeds 16 published windows"); return; }
    to=(int)shown_count++;
  }
  free_view(shown+to);
  shown[to]=pending[from];
  shown_sequence[to]=sequence;
  --pending_count;
  pending[from]=pending[pending_count];
  pending_capacity[from]=pending_capacity[pending_count];
  pending_world_count[from]=pending_world_count[pending_count];
  pending_sequence[from]=pending_sequence[pending_count];
  memset(pending+pending_count,0,sizeof(*pending));
  pending_capacity[pending_count]=0;
  pending_world_count[pending_count]=0;
  frame.view_count=shown_count;
  ++frame.serial;
}
void lhx_scene_event(CPU *cpu, unsigned offset) {
  if (!g_lhx_scene_enabled || failed) return;
  switch (offset) {
  case 0x1468a: after_horizon(cpu); break;
  case 0x14792:
    g_lhx_scene_filter_lo=g_lhx_scene_filter_hi=0;
    hold_count=0; scene_active=0; break;
  case 0x146be: case 0x1aa0d:
    if (hold_count==MAX_HOLDS) { invalid(cpu,"native scene pixel-filter nesting exceeds 8"); break; }
    holds[hold_count].lo=g_lhx_scene_filter_lo;
    holds[hold_count++].hi=g_lhx_scene_filter_hi;
    g_lhx_scene_filter_lo=g_lhx_scene_filter_hi=0; break;
  case 0x146c3: case 0x1aa10:
    if (hold_count) {
      --hold_count;
      g_lhx_scene_filter_lo=holds[hold_count].lo;
      g_lhx_scene_filter_hi=holds[hold_count].hi;
    }
    break;
  case 0x1cd76: capture_model(cpu); break;
  case 0x11096: publish(cpu); break;
  default: invalid(cpu,"unknown authored native scene observation address"); break;
  }
}
