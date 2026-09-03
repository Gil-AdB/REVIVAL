#ifndef REVIVAL_SEAMPHASE_H
#define REVIVAL_SEAMPHASE_H

// --greets_uv_seam_phase: make both sheets of every stone CREASE carry the same
// texture column along the shared edge, by shifting (and, optionally, very
// slightly scaling) u per authored UV patch.
//
// Why (2026-09-02): his reading of --uv_viz at H6194 - "the uv at the seam is
// not continous" - and the mesh census [STONE-UVSEAM]: 37 of the 72 authored
// crease edges on 'rooms' are vertical corners whose u steps by a constant
// 0.03-0.49 tile (28 corner lines, all ten mitre lines among them). Along such
// an edge the two sheets sample two different height columns, so they displace
// one world line to two heights: the double-valued profile the mitre weld then
// tries to reconcile. A per-patch u shift closes every such step that is a pure
// phase; a step that varies along the edge (the sloped joints) is a scale or
// direction mismatch, reported and left alone.
//
// Runs at greets init in the same slot as the v4 world-UV rewrite: BEFORE the
// flat shadow-proxy snapshot copies the face structs and before any UV-derived
// table is built. Off = no call = byte-null.

struct Scene;

namespace rev {

// Shift u per patch for faces whose material name is in mats[0..nMats).
// sweeps = Gauss-Seidel sweeps after the spanning-tree pass (0 = tree only).
// scaleTerm = also solve a per-patch u scale (1 + e) that closes the loops an
// offset cannot (a ring of walls whose perimeter is not a whole number of
// tiles), least-squares with e penalised by patch area; 0 = offsets only.
// Returns the number of faces whose UV was rewritten.
long long SeamPhaseUV_Apply(Scene *Sc, const char *const *mats, int nMats, int sweeps, int scaleTerm);

}  // namespace rev

#endif  // REVIVAL_SEAMPHASE_H
