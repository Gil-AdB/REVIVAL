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

// Re-render request signaling for the editor's idle throttle. The render loop
// renders only while the view is dirty and idles otherwise (a static frozen
// frame doesn't need re-drawing at rAF rate). Any surface edit marks dirty; the
// host also marks dirty on camera/frame/resize changes.
void Editor_MarkDirty();
bool Editor_ConsumeDirty();   // true if marked since the last call; clears it

// Split a surface's spatially-separate INSTANCES into independent surfaces so
// they can be edited apart (two mummies share the material "momy" — editing
// one edits both). Faces are clustered by world position (grid single-linkage,
// radius ~15% of the union diagonal); the biggest cluster keeps the original
// material(s), every other cluster gets clones named "<name>#2", "#3", …
// (::mirUV handedness clones are cloned alongside and keep their suffix, so
// all name-keyed ops collapse correctly). Appends to MatLib + rebuilds the
// scene mat table. LIVE-ONLY: the LWO has one shared surface, so saves apply
// to the base name. Returns a JSON array of the NEW base names ([] = nothing
// to split — only one instance found).
std::string Editor_SplitInstances(const char* name);

// Set one property on the i-th scene-authored light (Omni_SceneAuthored, file
// order — the same index the LWS/FLD write-back patchers use). Keys: r/g/b
// (0-255; also re-points the omni's flare sprite at a texture baked for the
// new color), intensity, range (splines — every key set), flareScale (sprite
// size multiplier on top of intensity; 0 = legacy).
bool Editor_SetLightProp(int index, const char *key, float value);

// Import a PBR map onto a surface from raw image-file bytes (a browser upload),
// by role: "albedo" | "normal" | "height" | "roughness" | "ao". Writes the bytes
// to a temp file and reuses MaterialImport_ApplyMapFile (same load/convert/assign
// as --material-import) — no native filesystem path needed. Returns true on
// success and marks the view dirty.
bool Editor_ImportTexture(const char* surface, const char* role,
                          const char* filename, const unsigned char* data, unsigned long len);

} // namespace rev

#endif // REVIVAL_MATERIAL_EDITOR_H
