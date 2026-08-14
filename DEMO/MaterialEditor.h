#ifndef REVIVAL_MATERIAL_EDITOR_H
#define REVIVAL_MATERIAL_EDITOR_H

#include <string>

struct Vector;   // Base/Vector.h — Editor_ComputeFocus in/out params

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

// Collapse a static-bake chunk object name ("Piramid.lwo:c17") onto its
// authored object name ("Piramid.lwo") — same idea as Editor_BaseSurfName but
// for the ':c<N>' suffix MeshOps chunk-splitting appends at scene init.
std::string Editor_ChunkBaseObjName(const char *name);

// JSON array of the scene's OBJECTS — groups of surfaces that belong to one
// logical model (the mech, the room, a city ship …), derived from the FLD
// OBJECT TREE (Obj->Name + Parent links) in EVERY scene, with collapsing
// rules for init-time mesh surgery (':c<N>' chunk suffixes, engine helper
// meshes → a hidden '(engine)' bucket) and the greets clone-file material
// naming merged in as enrichment:
//   [{name, obj, meshes, surfaces:[...names as in GetSurfacesJSON...]
//     [, children:[{name, obj, surfaces}]] [, engine:1]}, ...]
// `obj` is the raw (chunk-collapsed) engine object name — the key that
// Editor_SetObjectScale takes ("" on the '(engine)' bucket). Multi-part roots
// (city trains, the mech's null root) read "<stem> (model)" and carry one
// child per part; instances (8 taxis) dedupe into one entry.
std::string Editor_GetObjectsJSON();

// LIVE per-object uniform scale multiplier (the objects panel's scale knob).
// `objName` = an objects-JSON entry's 'obj' field (raw chunk-collapsed engine
// object name — 'SHIP1.lwo', 'taxi.lwo', 'mech  null'). Applies to EVERY
// instance of the name and composes into child objects (model roots scale the
// whole assembly around their pivot); takes effect on the next Animate tick.
// Returns the LIVE (non-static-baked) mesh count set; 0 = no effect. Native
// validation: OBJSCALE_TEST=<object>:<scale>. Persisted via the scene
// sidecar's 'obj:<name>|scale|v' lines (MaterialImport.h).
int Editor_SetObjectScale(const char *objName, float scale);

// Set one float property (key) on every CurScene material whose Name matches.
// Keys: baseR/baseG/baseB, diffuse, specular, glossiness, luminosity,
// transparency, reflection. Returns true if at least one material was updated.
// A successful edit marks the view dirty (see below).
bool Editor_SetSurfaceProp(const char* name, const char* key, float value);

// AUTO-CENTER the env probe of `surface` — the one-click button beside the
// "probe offset X/Y/Z" boxes. Computes the offset that moves this probe's
// capture point from wherever the CURRENTLY ACTIVE derivation puts it to the
// --env_probe_center corrected point (area-weighted centroid over the whole
// instance group), and writes it through the ordinary envBakeOfs* property
// path — so it live re-bakes through the same targeted store drop the boxes
// use, and persists through the same LWO RVSF bit 0x1000, unchanged. Because
// it is a DIFFERENCE it composes: with --env_probe_center already on it
// correctly writes (0,0,0) rather than double-applying the correction.
//
// Returns a JSON object the panel can read back into its three boxes:
// {"ok":1,"x":..,"y":..,"z":..,"from":"<material>","active":[..],"corrected":[..]}
// or {"ok":0,"why":"..."}. Derives from the BASE material of the surface; the
// ::mirUV clone inherits the value, exactly as a hand-typed offset does.
std::string Editor_AutoCenterProbe(const char* surface);

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
// to MatLib + rebuilds the scene mat table. LIVE mechanism: the LWO still has
// one shared surface until the editor's Save BAKES the split into it (server
// lwopatch split_surface — the parts become real authored surfaces like
// 'momy2'); scenes without authoring sources (crash) stay live-only and the
// server collapses their (#k)+ names onto the base. Returns a JSON OBJECT:
//   {"clusters":C, "faces":F, "names":["<name>#1","<name>#2",…],
//    "centroids":{"<name>#k":[x,y,z], …}}
// names is empty when nothing was split (C==1: all faces are one spatial
// cluster; C==0: surface not found / no faces) so the UI can say why.
// centroids = per-part WORLD face-centroid means — the shell ships them with
// the save payload so the bake can match its LWO polygon clusters to the
// live parts geometrically (engine face order != LWO poly order once init
// chunking reorders faces, so order alone would swap same-size parts).
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
// Spot keys: type (0 = Light_Omni, 1 = Light_SpotLight — flipping to spot
// seeds a valid aim + cone, see editorSeedSpotDefaults), coneAngle /
// innerAngle (HALF-angles in DEGREES from the cone axis, the same units the
// LWS "ConeAngle" carries), dirX/dirY/dirZ (cone axis; an edit that would
// leave it (0,0,0) is REFUSED — a zero aim NaNs the kernel's Vector_Norm),
// volBeamGain + forceVolCone (per-light volumetric beam), shadow (live in the
// OFF direction only — turning it on needs a ShadowMaps_Rebuild) and
// shadowMapRes (recorded; read once by ShadowMaps_Rebuild at scene init).
bool Editor_SetLightProp(int index, const char *key, float value);

// JSON array of the scene's AUTHORED lights (mirror-clone omnis excluded —
// BuildMirror's memcpy'd clones inherit Omni_SceneAuthored and are prepended
// to OmniHead, so an unfiltered walk lists ~4x phantom clones first). Entry
// fields: i (authored index — Editor_SetLightProp / LWS write-back space),
// rawI (position in the legacy unfiltered Omni_SceneAuthored walk — the index
// MainLoop's editorFocusLight still counts), r/g/b, intensity, range,
// flareScale, x/y/z, type, shadow, posKeys/sizeKeys/rangeKeys, plus the spot
// set: coneAngle/innerAngle (HALF-angles, degrees), volBeamGain,
// forceVolCone, shadowMapRes, dirX/dirY/dirZ.
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

// Focus-framing core (shared native/wasm — MainLoop.cpp's editorFocusSurface
// and the native FOCUS_TEST hook). World-space centre + bounding radius for a
// ';'-separated surface-name selection: matching faces gather by editor base
// name (mirror-clone meshes excluded) and are single-linkage-clustered from
// the face nearest `nearPos`; only that cluster is framed, so multi-instance
// surfaces/objects (city taxis, greets lamps) resolve to the instance nearest
// the camera instead of a mid-air union centroid. Callers place the camera at
// centre − viewDir·max(2.5·radius, 6) — object centred, ~1/3 of the view,
// nothing hidden. Returns false when no faces match. outUsedFaces/
// outTotalFaces (optional) report cluster/total matching face counts.
bool Editor_ComputeFocus(const char *names, const Vector &nearPos,
                         Vector &outCenter, float &outRadius,
                         long *outUsedFaces = nullptr,
                         unsigned long *outTotalFaces = nullptr);

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
