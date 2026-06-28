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

} // namespace fds

#endif // REVIVAL_MATERIAL_IMPORT_H
