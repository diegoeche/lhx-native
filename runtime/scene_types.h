#pragma once
#include <stdint.h>

struct LhxTexture;
typedef struct { float u, v; } LhxSceneUV;

/* Camera-space geometry copied from the original engine, in common units. */
typedef struct { float x, y, z; } LhxSceneVertex;
typedef struct {
  unsigned kind, color, count;
  int one_sided, stipple;
  float radius;
  uint16_t *indices;
} LhxScenePrimitive;
typedef struct {
  LhxSceneVertex *vertices;
  LhxScenePrimitive *primitives;
  unsigned vertex_count, primitive_count;
  int layer; /* 0=background points, 1=flat ground markings, 2=solid objects */
  uint32_t object_id; /* Linear guest object address; zero for synthetic meshes. */
  float object_distance; /* Object-origin radial distance in world feet. */
  const struct LhxTexture *texture;
  const LhxSceneUV *uvs; /* Optional texture coordinates, one per vertex. */
  int runway;
  LhxSceneVertex runway_corners[4]; /* Camera-space, in UV order 00,10,11,01. */
  char model_name[14];
  int has_placement;
  double world_origin[3],model_bounds[6]; /* bounds: x min/max, y min/max, z min/max. */
  double model_to_view[3][4]; /* Original model-local common units -> camera. */
} LhxSceneMesh;
typedef struct {
  unsigned colors[2];
  int two_colors;
  float x, y, nx, ny; /* Original 320x200 coordinates; positive side=colors[0]. */
} LhxSceneHorizon;
typedef struct {
  int x0, y0, x1, y1, cx, cy;
  float focus;
  LhxSceneHorizon horizon;
  LhxSceneMesh *meshes;
  unsigned mesh_count;
  int has_camera; /* Captured main view; render options may also enhance target world views. */
  double camera[3], world_to_view[3][3];
  unsigned sky_color, ground_color;
} LhxSceneView;
typedef struct {
  LhxSceneView *views;
  unsigned view_count;
  uint64_t serial;
} LhxSceneFrame;
