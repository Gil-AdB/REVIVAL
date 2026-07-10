#ifndef REVIVAL_MATERIAL_EDITOR_H
#define REVIVAL_MATERIAL_EDITOR_H

#include <string>

// Live surface editor core (LWO surface editor, Phase 1). Plain C++ so it can
// be unit-checked natively; the browser exposes these through Embind. Operates
// on the materials of the currently-rendered scene (CurScene) in the global
// MatLib — the deferred kernel reads Mat->X every frame, so a setSurfaceProp
// mutation shows up on the next rendered frame with no re-bake.
namespace rev {

// Collapse a "<surface>::mirUV" handedness-clone material name (greets'
// FixNormalMapSeam split) onto its base surface name. Some surfaces render
// ONLY through their clone (base "floor" covers 0 px), so every name-keyed
// operation — enumerate, edit, highlight, focus, PBR import — must match by
// base name or it silently misses the geometry that actually draws.
std::string Editor_BaseSurfName(const char* name);

// JSON array of the current scene's distinct surfaces + their editable props:
//   [{name, baseR,baseG,baseB, diffuse, specular, glossiness, luminosity,
//     transparency, reflection, flags, texture}, ...]
// Names are de-duplicated (a surface split across material clones — e.g. the
// per-handedness "rooms::mirUV" — collapses to one entry; setSurfaceProp then
// applies to every material sharing the name).
std::string Editor_GetSurfacesJSON();

// JSON array of the scene's OBJECTS — groups of surfaces that belong to one
// logical model (the mech, the room, …):
//   [{name, meshes, surfaces:[...names as in GetSurfacesJSON...]}, ...]
// The engine has no object names at runtime (TriMesh carries none), so objects
// are derived: meshes that share a surface TOKEN are one object. The token is
// the surface's base identity across the robot-clone naming scheme — base name
// (::mirUV collapsed), then the part after "X.lwo::", minus the _body/_upper
// clone suffixes. E.g. Hull.lwo + the four leg meshes all use token "hull", so
// they union into one object (the mech); the room mesh's surfaces form another.
std::string Editor_GetObjectsJSON();

// Set one float property (key) on every CurScene material whose Name matches.
// Keys: baseR/baseG/baseB, diffuse, specular, glossiness, luminosity,
// transparency, reflection. Returns true if at least one material was updated.
// A successful edit marks the view dirty (see below).
bool Editor_SetSurfaceProp(const char* name, const char* key, float value);

// LIVE per-surface normal-smoothing-angle edit (0=faceted .. 180=fully smooth).
// Registers the override (so it round-trips + persists on Save) AND re-smooths
// the current mesh normals so the shading changes on the NEXT frame — no scene
// reload. Bit-identical to a reload at the same angle for meshes split at init
// (see MeshOps_ResmoothSurface). Marks the view dirty.
void Editor_SetSmoothAngleLive(const char* surface, float angleDeg);

// Re-render request signaling for the editor's idle throttle. The render loop
// renders only while the view is dirty and idles otherwise (a static frozen
// frame doesn't need re-drawing at rAF rate). Any surface edit marks dirty; the
// host also marks dirty on camera/frame/resize changes.
void Editor_MarkDirty();
bool Editor_ConsumeDirty();   // true if marked since the last call; clears it

// Split a surface's spatially-separate INSTANCES into independent surfaces so
// they can be edited apart (two mummies share the material "momy" — editing
// one edits both). Faces are clustered by world position (grid single-linkage,
// radius ~15% of the union diagonal); mirror-clone ("__mirrorClone_*") meshes
// are EXCLUDED (their faces reference the same materials at mirrored positions
// behind the wall — clustering them inflated the radius and chain-linked real
// instances into one blob). The biggest cluster keeps the original material(s)
// but is RENAMED "<name>#1"; every other cluster gets clones named "<name>#2",
// "#3", … (::mirUV handedness clones are cloned alongside and keep their
// suffix OUTSIDE the "#k", so all name-keyed ops collapse correctly). Appends
// to MatLib + rebuilds the scene mat table. LIVE-ONLY: the LWO has one shared
// surface; the server's save path strips the whole trailing (#k)+ chain, so
// "#1" and "#2" both persist onto the base name. Returns a JSON OBJECT:
//   {"clusters":C, "faces":F, "names":["<name>#1","<name>#2",…]}
// names is empty when nothing was split (C==1: all faces are one spatial
// cluster; C==0: surface not found / no faces) so the UI can say why.
std::string Editor_SplitInstances(const char* name);

// Map-inspector overlay: draw `surface`'s `role` map (albedo|normal|height|
// roughness|ao|metallic) mip0 into the top-center quarter of the final frame
// (EnvReflection_DrawViz pano-viewer pattern; orange frame). role "off" or an
// empty surface hides it. Returns "on" / "off" / "no map". The draw itself
// (Editor_DrawMapViz) runs in RENDER.CPP's post-tonemap tail via the
// g_editorDrawMapViz hook; natively MAPVIZ_TEST=surface:role arms it for
// headless snapshot validation.
std::string Editor_SetMapViz(const char* surface, const char* role);
void Editor_DrawMapViz();

// Re-project a surface's texture coordinates live: proj 0=planar 1=cylindrical
// 2=spherical 3=cubic (LightWave's projections — UVs are baked at FLD load
// from exactly these parameters); size = world units per texture tile per
// axis; axis = LW axis flag (1=X 2=Y 0/4=Z; planar/cylindrical only, cubic
// picks per face). Rewrites face UVs + retangents affected meshes + updates
// the material's mapping fields (so LWO/FLD write-back can persist them).
// Returns a JSON summary ({} = surface not found).
std::string Editor_SetUVMapping(const char *name, int proj,
                                float sx, float sy, float sz, int axis);

// Set one property on the i-th scene-authored light (Omni_SceneAuthored, file
// order — the same index the LWS/FLD write-back patchers use; mirror-clone
// omnis are excluded from the walk, so the index space stays the FLD/LWS file
// order even with mirrors built). Keys: r/g/b (0-255; also re-points the
// omni's flare sprite at a texture baked for the new color), intensity, range
// (splines — every key set), flareScale (sprite size multiplier on top of
// intensity; 0 = legacy).
bool Editor_SetLightProp(int index, const char *key, float value);

// JSON array of the scene's AUTHORED lights (mirror-clone omnis excluded —
// BuildMirror's memcpy'd clones inherit Omni_SceneAuthored and are prepended
// to OmniHead, so an unfiltered walk lists ~4x phantom clones first). Entry
// fields: i (authored index — Editor_SetLightProp / LWS write-back space),
// rawI (position in the legacy unfiltered Omni_SceneAuthored walk — the index
// MainLoop's editorFocusLight still counts), r/g/b, intensity, range,
// flareScale, x/y/z, type, shadow, posKeys/sizeKeys/rangeKeys.
std::string Editor_GetLightsJSON();

// Resolve the surface under a click. (u,v) normalized [0,1] over the engine
// surface. Opaque surfaces resolve through the G-buffer matID plane exactly as
// before; Mat_Transparent surfaces (which never write it) resolve through a
// view-space ray cast against the scene's transparent faces, accepted when the
// hit is nearer than the opaque depth at that pixel (ZPage16). "" = nothing.
std::string Editor_PickSurface(float u, float v);

// Nearest authored light whose screen projection is within ~14 px (1080p-
// scaled) of the click; returns its authored index ("i" above), -1 = none.
int Editor_PickLight(float u, float v);

// Import a PBR map onto a surface from raw image-file bytes (a browser upload),
// by role: "albedo" | "normal" | "height" | "roughness" | "ao". Writes the bytes
// to a temp file and reuses MaterialImport_ApplyMapFile (same load/convert/assign
// as --material-import) — no native filesystem path needed. Returns true on
// success and marks the view dirty.
bool Editor_ImportTexture(const char* surface, const char* role,
                          const char* filename, const unsigned char* data, unsigned long len);

// Reset a surface's (role) map to its authored default — undoes editor imports
// (incl. the procedural displacement generator) and sidecar-applied overrides
// for this run. Persisting the removal is the shell's job (it tombstones the
// role so Save deletes the sidecar line). Returns true incl. the already-
// default no-op case; false for unknown surface/role.
bool Editor_ClearMap(const char* surface, const char* role);

} // namespace rev

#endif // REVIVAL_MATERIAL_EDITOR_H
