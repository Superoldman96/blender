/* SPDX-FileCopyrightText: 2009 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include <variant>

#include "BLI_array.hh"
#include "BLI_bit_vector.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_ordered_edge.hh"
#include "BLI_set.hh"
#include "BLI_shared_cache.hh"
#include "BLI_utility_mixins.hh"

#include "DNA_brush_enums.h"
#include "DNA_customdata_types.h"
#include "DNA_object_enums.h"

#include "BKE_pbvh.hh"
#include "BKE_subdiv_ccg.hh"

struct BMFace;
struct BMLog;
struct BMVert;
struct BMesh;
struct BlendDataReader;
struct BlendWriter;
struct Brush;
struct CustomDataLayer;
struct CurveMapping;
struct Depsgraph;
struct EnumPropertyItem;
namespace blender {
namespace bke {
enum class AttrDomain : int8_t;
namespace pbvh {
class Tree;
}
}  // namespace bke
namespace ed::sculpt_paint {
namespace expand {
struct Cache;
}
namespace filter {
struct Cache;
}
struct StrokeCache;
}  // namespace ed::sculpt_paint
}  // namespace blender
struct GHash;
struct GridPaintMask;
struct Image;
struct ImagePool;
struct ImageUser;
struct KeyBlock;
struct Main;
struct Mesh;
struct MDeformVert;
struct MultiresModifierData;
struct Object;
struct Paint;
struct PaintCurve;
struct PaintModeSettings;
struct Palette;
struct PaletteColor;
struct RegionView3D;
struct Scene;
struct Sculpt;
struct SculptSession;
struct SubdivCCG;
struct Tex;
struct ToolSettings;
struct UnifiedPaintSettings;
struct View3D;
struct ViewLayer;
struct bContext;
struct bToolRef;
struct tPaletteColorHSV;

extern const uchar PAINT_CURSOR_SCULPT[3];
extern const uchar PAINT_CURSOR_VERTEX_PAINT[3];
extern const uchar PAINT_CURSOR_WEIGHT_PAINT[3];
extern const uchar PAINT_CURSOR_TEXTURE_PAINT[3];
extern const uchar PAINT_CURSOR_SCULPT_CURVES[3];
extern const uchar PAINT_CURSOR_PAINT_GREASE_PENCIL[3];
extern const uchar PAINT_CURSOR_SCULPT_GREASE_PENCIL[3];

enum class PaintMode : int8_t {
  Sculpt = 0,
  /** Vertex color. */
  Vertex = 1,
  Weight = 2,
  /** 3D view (projection painting). */
  Texture3D = 3,
  /** Image space (2D painting). */
  Texture2D = 4,
  GPencil = 6,
  /* Grease Pencil Vertex Paint */
  VertexGPencil = 7,
  SculptGPencil = 8,
  WeightGPencil = 9,
  /** Curves. */
  SculptCurves = 10,
  /** Grease Pencil. */
  SculptGreasePencil = 11,

  /** Keep last. */
  Invalid = 12,
};

/* overlay invalidation */
enum ePaintOverlayControlFlags {
  PAINT_OVERLAY_INVALID_TEXTURE_PRIMARY = 1,
  PAINT_OVERLAY_INVALID_TEXTURE_SECONDARY = (1 << 2),
  PAINT_OVERLAY_INVALID_CURVE = (1 << 3),
  PAINT_OVERLAY_OVERRIDE_CURSOR = (1 << 4),
  PAINT_OVERLAY_OVERRIDE_PRIMARY = (1 << 5),
  PAINT_OVERLAY_OVERRIDE_SECONDARY = (1 << 6),
};
ENUM_OPERATORS(ePaintOverlayControlFlags, PAINT_OVERLAY_OVERRIDE_SECONDARY);

#define PAINT_OVERRIDE_MASK \
  (PAINT_OVERLAY_OVERRIDE_SECONDARY | PAINT_OVERLAY_OVERRIDE_PRIMARY | \
   PAINT_OVERLAY_OVERRIDE_CURSOR)

/**
 * Defines 8 areas resulting of splitting the object space by the XYZ axis planes. This is used to
 * flip or mirror transform values depending on where the vertex is and where the transform
 * operation started to support XYZ symmetry on those operations in a predictable way.
 */
#define PAINT_SYMM_AREA_DEFAULT 0

enum ePaintSymmetryAreas {
  PAINT_SYMM_AREA_X = (1 << 0),
  PAINT_SYMM_AREA_Y = (1 << 1),
  PAINT_SYMM_AREA_Z = (1 << 2),
};
ENUM_OPERATORS(ePaintSymmetryAreas, PAINT_SYMM_AREA_Z);

#define PAINT_SYMM_AREAS 8

void BKE_paint_invalidate_overlay_tex(Scene *scene, ViewLayer *view_layer, const Tex *tex);
void BKE_paint_invalidate_cursor_overlay(Scene *scene, ViewLayer *view_layer, CurveMapping *curve);
void BKE_paint_invalidate_overlay_all();
ePaintOverlayControlFlags BKE_paint_get_overlay_flags();
void BKE_paint_reset_overlay_invalid(ePaintOverlayControlFlags flag);
void BKE_paint_set_overlay_override(enum eOverlayFlags flag);

/* Palettes. */

Palette *BKE_palette_add(Main *bmain, const char *name);
PaletteColor *BKE_palette_color_add(Palette *palette);
bool BKE_palette_is_empty(const Palette *palette);
/**
 * Remove color from palette. Must be certain color is inside the palette!
 */
void BKE_palette_color_remove(Palette *palette, PaletteColor *color);
void BKE_palette_clear(Palette *palette);

void BKE_palette_sort_hsv(tPaletteColorHSV *color_array, int totcol);
void BKE_palette_sort_svh(tPaletteColorHSV *color_array, int totcol);
void BKE_palette_sort_vhs(tPaletteColorHSV *color_array, int totcol);
void BKE_palette_sort_luminance(tPaletteColorHSV *color_array, int totcol);
bool BKE_palette_from_hash(Main *bmain, GHash *color_table, const char *name, bool linear);

/* Paint curves. */

PaintCurve *BKE_paint_curve_add(Main *bmain, const char *name);

/**
 * Call when entering each respective paint mode.
 */
bool BKE_paint_ensure(Main *bmain, ToolSettings *ts, Paint **r_paint);
void BKE_paint_init(Main *bmain, Scene *sce, PaintMode mode, const uchar col[3]);
void BKE_paint_free(Paint *paint);
/**
 * Called when copying scene settings, so even if 'src' and 'tar' are the same still do a
 * #id_us_plus(), rather than if we were copying between 2 existing scenes where a matching
 * value should decrease the existing user count as with #paint_brush_set()
 */
void BKE_paint_copy(const Paint *src, Paint *dst, int flag);

void BKE_paint_cavity_curve_preset(Paint *paint, int preset);

eObjectMode BKE_paint_object_mode_from_paintmode(PaintMode mode);
bool BKE_paint_ensure_from_paintmode(Main *bmain, Scene *sce, PaintMode mode);
Paint *BKE_paint_get_active_from_paintmode(Scene *sce, PaintMode mode);
const EnumPropertyItem *BKE_paint_get_tool_enum_from_paintmode(PaintMode mode);
uint BKE_paint_get_brush_type_offset_from_paintmode(PaintMode mode);
std::optional<int> BKE_paint_get_brush_type_from_obmode(const Brush *brush,
                                                        const eObjectMode ob_mode);
Paint *BKE_paint_get_active(Scene *sce, ViewLayer *view_layer);
Paint *BKE_paint_get_active_from_context(const bContext *C);
PaintMode BKE_paintmode_get_active_from_context(const bContext *C);
PaintMode BKE_paintmode_get_from_tool(const bToolRef *tref);

/* Paint brush retrieval and assignment. */

Brush *BKE_paint_brush(Paint *paint);
const Brush *BKE_paint_brush_for_read(const Paint *paint);
Brush *BKE_paint_brush_from_essentials(Main *bmain, const char *name);

/**
 * Activates \a brush for painting, and updates #Paint.brush_asset_reference so the brush can be
 * restored after file read.
 *
 * \return True on success. If \a brush is already active, this is considered a success (the brush
 * asset reference will still be updated).
 */
bool BKE_paint_brush_set(Paint *paint, Brush *brush);
bool BKE_paint_brush_set_default(Main *bmain, Paint *paint);
bool BKE_paint_brush_set_essentials(Main *bmain, Paint *paint, const char *name);

void BKE_paint_brushes_set_default_references(ToolSettings *ts);
void BKE_paint_brushes_validate(Main *bmain, Paint *paint);

/* Secondary eraser brush. */

Brush *BKE_paint_eraser_brush(Paint *paint);
const Brush *BKE_paint_eraser_brush_for_read(const Paint *paint);
Brush *BKE_paint_eraser_brush_from_essentials(Main *bmain, const char *name);

bool BKE_paint_eraser_brush_set(Paint *paint, Brush *brush);
bool BKE_paint_eraser_brush_set_default(Main *bmain, Paint *paint);
bool BKE_paint_eraser_brush_set_essentials(Main *bmain, Paint *paint, const char *name);

/* Paint palette. */

Palette *BKE_paint_palette(Paint *paint);
void BKE_paint_palette_set(Paint *paint, Palette *palette);
void BKE_paint_curve_clamp_endpoint_add_index(PaintCurve *pc, int add_index);

/**
 * Return true when in vertex/weight/texture paint + face-select mode?
 */
bool BKE_paint_select_face_test(const Object *ob);
/**
 * Return true when in vertex/weight paint + vertex-select mode?
 */
bool BKE_paint_select_vert_test(const Object *ob);
/**
 * used to check if selection is possible
 * (when we don't care if its face or vert)
 */
bool BKE_paint_select_elem_test(const Object *ob);
/**
 * Checks if face/vertex hiding is always applied in the current mode.
 * Returns true in vertex/weight paint.
 */
bool BKE_paint_always_hide_test(const Object *ob);

/* Partial visibility. */

/**
 * Returns whether any of the corners of the grid face whose inner corner is at (x, y) are hidden.
 */
bool paint_is_grid_face_hidden(blender::BoundedBitSpan grid_hidden, int gridsize, int x, int y);
/**
 * Return true if all vertices in the face are visible, false otherwise.
 */
bool paint_is_bmesh_face_hidden(const BMFace *f);

/* Paint masks. */

float paint_grid_paint_mask(const GridPaintMask *gpm, uint level, uint x, uint y);

void BKE_paint_face_set_overlay_color_get(int face_set, int seed, uchar r_color[4]);

/* Stroke related. */

bool paint_calculate_rake_rotation(UnifiedPaintSettings &ups,
                                   const Brush &brush,
                                   const float mouse_pos[2],
                                   PaintMode paint_mode,
                                   bool stroke_has_started);
void paint_update_brush_rake_rotation(UnifiedPaintSettings &ups,
                                      const Brush &brush,
                                      float rotation);

void BKE_paint_stroke_get_average(const Scene *scene, const Object *ob, float stroke[3]);

/* .blend I/O */

void BKE_paint_blend_write(BlendWriter *writer, Paint *paint);
void BKE_paint_blend_read_data(BlendDataReader *reader, const Scene *scene, Paint *paint);

#define SCULPT_FACE_SET_NONE 0

/* Data used for displaying extra visuals while using the Pose brush */
struct SculptPoseIKChainPreview {
  blender::Array<blender::float3> initial_orig_coords;
  blender::Array<blender::float3> initial_head_coords;
};

struct SculptVertexInfo {
  /* Indexed by base mesh vertex index, stores if that vertex is a boundary. */
  blender::BitVector<> boundary;
};

/* Data used for displaying extra visuals while using the Boundary brush. */
struct SculptBoundaryPreview {
  blender::Vector<std::pair<blender::float3, blender::float3>> edges;
  blender::float3 pivot_position;
  blender::float3 initial_vert_position;
};

struct SculptFakeNeighbors {
  bool use_fake_neighbors;

  /* Max distance used to calculate neighborhood information. */
  float current_max_distance;

  /* Indexed by vertex, stores the vertex index of its fake neighbor if available. */
  blender::Array<int> fake_neighbor_index;
};

struct SculptTopologyIslandCache {
  /**
   * An ID for the island containing each geometry vertex. Will be empty if there is only a single
   * island.
   */
  blender::Array<uint8_t> vert_island_ids;
};

using ActiveVert = std::variant<std::monostate, int, SubdivCCGCoord, BMVert *>;

struct SculptSession : blender::NonCopyable, blender::NonMovable {
  /* Mesh data (not copied) can come either directly from a Mesh, or from a MultiresDM */
  struct { /* Special handling for multires meshes */
    bool active = false;
    MultiresModifierData *modifier = nullptr;
    int level = 0;
  } multires = {};

  /* Depsgraph for the Cloth Brush solver to get the colliders. */
  Depsgraph *depsgraph = nullptr;

  /* These are always assigned to base mesh data when using Type::Mesh. */
  blender::OffsetIndices<int> faces;
  blender::Span<int> corner_verts;

  /* These contain the vertex and poly counts of the final mesh. */
  int totvert = 0;
  int faces_num = 0;

  KeyBlock *shapekey_active = nullptr;

  /* Mesh connectivity maps. */
  /* Vertices to adjacent polys. */
  blender::GroupedSpan<int> vert_to_face_map;

  /* Edges to adjacent faces. */
  blender::Array<int> edge_to_face_offsets;
  blender::Array<int> edge_to_face_indices;
  blender::GroupedSpan<int> edge_to_face_map;

  /* Vertices to adjacent edges. */
  blender::Array<int> vert_to_edge_offsets;
  blender::Array<int> vert_to_edge_indices;
  blender::GroupedSpan<int> vert_to_edge_map;

  /* Mesh Face Sets */
  /* Total number of faces of the base mesh. */
  int totfaces = 0;

  /* The 0 ID is not used by the tools or the visibility system, it is just used when creating new
   * geometry (the trim tool, for example) to detect which geometry was just added, so it can be
   * assigned a valid Face Set after creation. Tools are not intended to run with Face Sets IDs set
   * to 0. */
  const int *face_sets = nullptr;
  /**
   * A reference to the ".hide_poly" attribute, to store whether (base) faces are hidden.
   * May be null.
   */
  const bool *hide_poly = nullptr;

  /* BMesh for dynamic topology sculpting */
  BMesh *bm = nullptr;
  /* Undo/redo log for dynamic topology sculpting */
  BMLog *bm_log = nullptr;

  /* Limit surface/grids. */
  SubdivCCG *subdiv_ccg = nullptr;

  /* BVH tree acceleration structure */
  std::unique_ptr<blender::bke::pbvh::Tree> pbvh;

  /* Object is deformed with some modifiers. */
  bool deform_modifiers_active = false;
  /* Coords of deformed mesh but without stroke displacement. */
  blender::Array<blender::float3, 0> deform_cos;
  /* Crazy-space deformation matrices. */
  blender::Array<blender::float3x3, 0> deform_imats;

  /**
   * Normals corresponding to the #deform_cos evaluated/deform positions. Stored as a #SharedCache
   * for consistency with mesh caches in #MeshRuntime::vert_normals_cache.
   */
  blender::SharedCache<blender::Vector<blender::float3>> vert_normals_deform;
  blender::SharedCache<blender::Vector<blender::float3>> face_normals_deform;

  /* Pool for texture evaluations. */
  ImagePool *tex_pool = nullptr;

  blender::ed::sculpt_paint::StrokeCache *cache = nullptr;
  blender::ed::sculpt_paint::filter::Cache *filter_cache = nullptr;
  blender::ed::sculpt_paint::expand::Cache *expand_cache = nullptr;

  /* Cursor data and active vertex for tools */
  int active_face_index = -1;
  int active_grid_index = -1;

  /* When active, the cursor draws with faded colors, indicating that there is an action
   * enabled.
   */
  bool draw_faded_cursor = false;
  float cursor_radius = 0.0f;
  blender::float3 cursor_location;
  blender::float3 cursor_normal;
  blender::float3 cursor_sampled_normal;
  blender::float3 cursor_view_normal;

  /* TODO(jbakker): Replace rv3d and v3d with ViewContext */
  RegionView3D *rv3d = nullptr;
  View3D *v3d = nullptr;
  Scene *scene = nullptr;

  /* Dynamic mesh preview */
  blender::Array<int> preview_verts;

  /* Pose Brush Preview */
  std::unique_ptr<SculptPoseIKChainPreview> pose_ik_chain_preview;

  /* Boundary Brush Preview */
  std::unique_ptr<SculptBoundaryPreview> boundary_preview;

  SculptVertexInfo vertex_info = {};
  SculptFakeNeighbors fake_neighbors = {};

  /* Transform operator */
  blender::float3 pivot_pos = {};
  blender::float4 pivot_rot = {};
  blender::float3 pivot_scale = {};

  blender::float3 init_pivot_pos = {};
  blender::float4 init_pivot_rot = {};
  blender::float3 init_pivot_scale = {};

  blender::float3 prev_pivot_pos = {};
  blender::float4 prev_pivot_rot = {};
  blender::float3 prev_pivot_scale = {};

  struct {
    struct {
      /* Keep track of how much each vertex has been painted (non-airbrush only). */
      float *alpha_weight;

      /* Needed to continuously re-apply over the same weights (BRUSH_ACCUMULATE disabled).
       * Lazy initialize as needed (flag is set to 1 to tag it as uninitialized). */
      MDeformVert *dvert_prev;
    } wpaint;

    /* TODO: identify sculpt-only fields */
    // struct { ... } sculpt;
  } mode = {};
  eObjectMode mode_type;

  /* This flag prevents bke::pbvh::Tree from being freed when creating the vp_handle for
   * texture paint. */
  bool building_vp_handle = false;

  /**
   * ID data is older than sculpt-mode data.
   * Set #Main.is_memfile_undo_flush_needed when enabling.
   */
  char needs_flush_to_id = false;

  /**
   * Some tools follows the shading chosen by the last used tool canvas.
   * When not set the viewport shading color would be used.
   *
   * NOTE: This setting is temporarily until paint mode is added.
   */
  bool sticky_shading_color = false;

  /**
   * Last used painting canvas key.
   */
  char *last_paint_canvas_key = nullptr;
  blender::float3 last_normal;

  std::unique_ptr<SculptTopologyIslandCache> topology_island_cache;

 private:
  /* In general, this value is expected to be valid (non-empty) as long as the cursor is over the
   * mesh. Changing the underlying mesh type (e.g. enabling dyntopo, changing multires levels)
   * should invalidate this value.
   */
  ActiveVert active_vert_ = {};

  /* This value should always exist except when the cursor has never been over the mesh, or when
   * the underlying mesh type has changed and the last `active_vert_` value no longer corresponds
   * to a value that can be correctly interpreted */
  ActiveVert last_active_vert_ = {};

 public:
  SculptSession();
  ~SculptSession();

  PBVHVertRef active_vert_ref() const;
  ActiveVert active_vert() const;

  PBVHVertRef last_active_vert_ref() const;
  ActiveVert last_active_vert() const;

  /**
   * Retrieves the corresponding index of the ActiveVert inside a mesh-sized array.
   *
   * Helpful in generic cases where we are unlikely to already be processing data in a backing-type
   * specific manner.
   *
   * \note For BMesh, a call to SCULPT_vertex_random_access_ensure is needed to get valid results.
   * \returns -1 if there is no currently active vertex.
   */
  int active_vert_index() const;

  /**
   * Retrieves the active vertex position.
   *
   * This method should be avoided if already working with the relevant position-backing structures
   * for each of the mesh types. In cases where we want more generic code, this abstraction helps
   * to remove boilerplate.
   *
   * \returns float3 at negative infinity if there is no currently active vertex
   */
  blender::float3 active_vert_position(const Depsgraph &depsgraph, const Object &object) const;

  void set_active_vert(ActiveVert vert);
  void clear_active_vert(bool persist_last_active);
};

void BKE_sculptsession_free(Object *ob);
void BKE_sculptsession_free_deformMats(SculptSession *ss);
void BKE_sculptsession_free_vwpaint_data(SculptSession *ss);
void BKE_sculptsession_free_pbvh(Object &object);
void BKE_sculptsession_bm_to_me(Object *ob, bool reorder);
void BKE_sculptsession_bm_to_me_for_render(Object *object);
int BKE_sculptsession_vertex_count(const SculptSession *ss);

/**
 * Create new color layer on object if it doesn't have one and if experimental feature set has
 * sculpt vertex color enabled. Returns truth if new layer has been added, false otherwise.
 */
void BKE_sculpt_color_layer_create_if_needed(Object *object);

/**
 * \warning Expects a fully evaluated depsgraph.
 */
void BKE_sculpt_update_object_for_edit(Depsgraph *depsgraph, Object *ob_orig, bool is_paint_tool);
void BKE_sculpt_update_object_before_eval(Object *ob_eval);
void BKE_sculpt_update_object_after_eval(Depsgraph *depsgraph, Object *ob_eval);

/**
 * Sculpt mode handles multi-res differently from regular meshes, but only if
 * it's the last modifier on the stack and it is not on the first level.
 */
MultiresModifierData *BKE_sculpt_multires_active(const Scene *scene, Object *ob);
/**
 * Update the pointer to the ".hide_poly" attribute. This is necessary because it is dynamically
 * created, removed, and made mutable.
 */
void BKE_sculpt_hide_poly_pointer_update(Object &object);

/**
 * Ensures a mask layer exists. If depsgraph and bmain are non-null,
 * a mask doesn't exist and the object has a multi-resolution modifier
 * then the scene depsgraph will be evaluated to update the runtime
 * subdivision data.
 *
 * \note always call *before* #BKE_sculpt_update_object_for_edit.
 */
void BKE_sculpt_mask_layers_ensure(Depsgraph *depsgraph,
                                   Main *bmain,
                                   Object *ob,
                                   MultiresModifierData *mmd);
void BKE_sculpt_toolsettings_data_ensure(Main *bmain, Scene *scene);

void BKE_sculpt_sync_face_visibility_to_grids(const Mesh &mesh, SubdivCCG &subdiv_ccg);

/**
 * Test if blender::bke::pbvh::Tree can be used directly for drawing, which is faster than
 * drawing the mesh and all updates that come with it.
 */
bool BKE_sculptsession_use_pbvh_draw(const Object *ob, const RegionView3D *rv3d);

namespace blender::bke::object {

pbvh::Tree &pbvh_ensure(Depsgraph &depsgraph, Object &object);

/**
 * Access the acceleration structure for raycasting, nearest queries, and spatially contiguous mesh
 * updates and drawing. The BVH tree is used by sculpt, vertex paint, and weight paint object
 * modes. This just accesses the BVH, to ensure it's built, use #pbvh_ensure.
 */
pbvh::Tree *pbvh_get(Object &object);
const pbvh::Tree *pbvh_get(const Object &object);

}  // namespace blender::bke::object
bool BKE_object_sculpt_use_dyntopo(const Object *object);

/* paint_canvas.cc */

/**
 * Create a key that can be used to compare with previous ones to identify changes.
 * The resulting 'string' is owned by the caller.
 */
char *BKE_paint_canvas_key_get(PaintModeSettings *settings, Object *ob);

bool BKE_paint_canvas_image_get(PaintModeSettings *settings,
                                Object *ob,
                                Image **r_image,
                                ImageUser **r_image_user);
int BKE_paint_canvas_uvmap_layer_index_get(const PaintModeSettings *settings, Object *ob);
void BKE_sculpt_check_cavity_curves(Sculpt *sd);
CurveMapping *BKE_sculpt_default_cavity_curve();
