#ifndef REVIVAL_MATERIAL_IMPORT_H
#define REVIVAL_MATERIAL_IMPORT_H

// CLI material import for testing — point a scene material at a folder of PBR
// maps (FreePBR / ambientCG / textures.com layout) and have it auto-detect and
// apply albedo / normal / height / roughness / AO by filename, with OpenGL↔
// DirectX normal-convention detection + conversion. Lets you swap materials at
// runtime without recompiling. Designed to be folded into the LWO editor later
// (same apply path).
//
// CLI (repeatable):
//   --material-import=<materialName>:<dir>
//     e.g. --material-import=momy:~/work/blender/walls-bl/stonewall-bl
//   Optional knobs:
//     --material-import-flip-normal   force a green-channel flip (override the
//                                     auto OGL/DX detection if relief looks
//                                     inverted — same idea as --greets-nmap-flip-g)
//     --material-import-no-mips        skip mip generation (faster load)
//
// Filename role detection (case-insensitive token on the file stem):
//   albedo:    albedo | basecolor | base_color | base-color | _color | diffuse
//   normal:    normal | nrm | _nor     (+ -ogl / -dx suffix → source convention)
//   height:    height | disp | displacement | bump
//   roughness: roughness | rough | rgh
//   ao:        ao | occlusion
//   (metallic/metalness: detected but IGNORED — the deferred path is
//    diffuse+spec, no metallic workflow; logged so it's not silent)
//   preview/thumbnail files are skipped.

struct Scene;
struct Material;

namespace fds {

// Parse --material-import* tokens out of argv (call once, early in main).
// Unknown to FeatureFlags, so we pre-scan argv like --snapshot does.
void MaterialImport_ParseArgs(int argc, const char *const *argv);

// True if any --material-import spec was given (cheap gate for scene init).
bool MaterialImport_Active();

// Apply every parsed spec whose material name resolves in `sc` (matched against
// MatLib entries with RelScene==sc). Call from a scene's init AFTER textures are
// loaded + Scene_RebuildMatTable (same point as the greets stone-tex block).
// No-op when no specs were given. Logs what it detected/applied per material.
void MaterialImport_Apply(Scene *sc, const char *sceneName);

// Apply ONE PBR map (already on disk at `path` — e.g. an uploaded file the
// browser editor wrote to MEMFS) to material `matName` in scene `sc`, by role:
// "albedo" | "normal" | "height" | "roughness" | "ao". Reuses the same load +
// convert (MakeNormal16/MakeHeight8) + assign + tangent-recompute as the CLI
// path. Returns true on success. The single-map entry point for the LWO editor.
bool MaterialImport_ApplyMapFile(Scene *sc, const char *matName,
                                 const char *role, const char *path);

// Editor "reset map": restore the surface's (role) slot to its authored
// default — the value it held before the first ApplyMapFile override this
// run (live import or sidecar apply at scene init). Success when the surface
// exists and the role is known, including the never-overridden no-op case.
bool MaterialImport_ClearSurfaceMap(Scene *sc, const char *matName,
                                    const char *role);

// Classify a map filename into its role using the same token rules as the CLI
// dir scan (see the table above). Returns "albedo" | "normal" | "height" |
// "roughness" | "ao" | "" (unrecognized / non-image / preview-skip / metallic —
// roles the editor can't apply). Lets the browser's load-a-whole-pack upload
// share the native detection instead of duplicating it in JS.
const char *MaterialImport_ClassifyRole(const char *filename);

// Sidecar loader — the PERSISTED form of the editor's PBR map assignments
// (LWO1 has no slot for them, so they can't live in the .lwo like the numeric
// surface values do) and, for scenes WITHOUT pinned LWO authoring sources
// (city/chase/fountain), of numeric surface-property overrides too. Line
// format, paths relative to Runtime/ (the CWD):
//   # comment / blank lines ignored
//   surface|role|TEXTURES/PBR/file.png      (role: albedo/normal/height/roughness/ao)
//   surface|prop|value                      (prop: diffuse/specular/glossiness/
//                                            luminosity/transparency/reflection/
//                                            baseR/baseG/baseB — engine scale)
//   light:<i>|key|value                     (per-light engine-only extensions;
//                                            <i> = authored light index in
//                                            FLD/LWS file order; currently
//                                            flareScale)
//   scene:|key|value                        (SCENE-level authored defaults for
//                                            world-space / scene-owned engine
//                                            quantities that live in global
//                                            FeatureFlags; applied via
//                                            FeatureFlags::setDefault at the
//                                            scene FACTORY — after the scene's
//                                            ApplyCinematicProfile, so authored
//                                            values beat the profile, while an
//                                            explicit CLI/env --flag still wins.
//                                            NOT handled by ApplySidecar; see
//                                            MaterialImport_ApplySceneDefaults.
//                                            Keys (all float-valued):
//                                              boltFlashPeak   → bolt_flash_peak
//                                              boltFlashRange  → bolt_flash_range
//                                              fastFogBottom   → fast_fog_bottom
//                                              fastFogTop      → fast_fog_top
//                                              fastFogCell     → fast_fog_cell
//                                            Note: SCRIPTS/<scene>.params lines
//                                            for the same flag still win (the
//                                            per-frame script yields only to
//                                            explicitly-SET flags, and scene:
//                                            defaults deliberately don't mark).
//                                            Caveat: flags are process-global —
//                                            a scene's authored default persists
//                                            into later scenes unless they author
//                                            or profile the same key.)
//   obj:<name>|scale|value                  (per-OBJECT overrides; <name> =
//                                            chunk-collapsed FLD object name,
//                                            e.g. 'SHIP1.lwo', 'taxi.lwo',
//                                            'mech  null'. scale = uniform
//                                            multiplier on the object's Scale
//                                            spline, 1 = authored; applies to
//                                            every instance of the name and
//                                            composes into child objects —
//                                            see ObjectImport_SetObjectScale)
// '|' separator because surface names contain spaces ("hull not smooth").
// Map lines go through MaterialImport_ApplyMapFile (same load/convert/assign/
// tangent-recompute as the CLI path, ::mirUV clones included); prop lines go
// through MaterialImport_SetSurfaceProp below. Call at scene init after
// Scene_RebuildMatTable, BEFORE MaterialImport_Apply so explicit
// --material-import CLI specs still override the sidecar. A missing sidecar is
// a silent no-op; a bad line inside one logs and skips that line.
// The editor's dev server (tools/editor_server.py) writes this file on Save.
void MaterialImport_ApplySidecar(Scene *sc, const char *path);

// Apply ONLY the sidecar's `scene:|key|value` lines (authored scene-level
// defaults for FeatureFlags-backed quantities — see the format table above).
// Values go through FeatureFlags::setDefault, so an explicit CLI/env flag
// still wins. Call from the scene FACTORY, immediately AFTER its
// ApplyCinematicProfile, so authored values override the cinematic profile's
// setDefaults (precedence: CLI/env > scene: sidecar > cinematic profile >
// compile default). A missing sidecar is a silent no-op; an unknown scene:
// key logs and skips. Separate from MaterialImport_ApplySidecar because
// scene init (where the sidecar's surface lines apply) runs BEFORE the
// factory's profile — scene: lines applied there would be stomped by it.
void MaterialImport_ApplySceneDefaults(const char *path);

// Set one numeric property (engine scale) on every material of `sc` whose
// base name (::mirUV collapsed) matches `surface`. The shared setter under
// both the sidecar prop lines and the editor's live Editor_SetSurfaceProp.
bool MaterialImport_SetSurfaceProp(Scene *sc, const char *surface,
                                   const char *prop, float value);

// Current normal-G flip parity of the material's normal map (0 = as the file
// loaded, 1 = flipped). Set via SetSurfaceProp("normalFlip", 0|1); tracked per
// TEXTURE so shared clones flip once and the editor UI reads a truthful state.
int MaterialImport_GetNormalFlip(const Material *M);

// Per-object uniform scale multiplier (the editor's objects-panel scale knob;
// persisted via 'obj:<name>|scale|v' sidecar lines). Sets TriMesh::EditorScale
// on every object whose chunk-collapsed name matches; Animate_Objects folds it
// into the Scale-spline result, so it pivots on the object pivot and composes
// down the parent→child chain (scaling a model root scales the assembly).
// Returns the number of LIVE (non-Tri_Possessed) meshes set — 0 means the
// object doesn't exist or is fully static-baked (greets' chunked room).
int   ObjectImport_SetObjectScale(Scene *sc, const char *objName, float scale);
// Read-back (first matching mesh; unset → 1.0) for the objects JSON.
float ObjectImport_GetObjectScale(Scene *sc, const char *objName);

} // namespace fds

#endif // REVIVAL_MATERIAL_IMPORT_H
