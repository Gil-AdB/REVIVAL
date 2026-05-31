// Canonical TU for hot-path rasterizer template instantiations.
//
// Why this file exists: TheOtherBarry<BlendMode, TextureMode> (forward
// rasterizer) and MekaleleImpl<MekaleleTarget> (deferred rasterizer)
// are templates whose instantiations historically got emitted as weak
// symbols in every TU that called them — FOUNTAIN.CPP, Snapshot.cpp,
// PREPROC.CPP, RenderInner.cpp, FillerTest.cpp, FILLERS.CPP, etc.
//
// That's fine when all TUs are compiled with the same flags. It is
// NOT fine when one TU is compiled with -ffp-contract=fast and others
// with -on — the two emitted bodies have bit-different FMA fusion in
// their edge-function math, the linker silently picks one, and city's
// rasterizer ends up using a randomly-selected version. On the city
// scene the FMA-fused version's 1-ULP shifts at frustum boundaries
// produced a stretched-triangle "horizon smear" artifact (bisected
// 2026-05-31 to FOUNTAIN.CPP getting -fast in the global 7e4c2ac).
//
// The fix: declare `extern template` for the used instantiations in
// the header (TheOtherBarry.h / Mekalele.h) so no consumer TU emits
// its own copy, then explicitly instantiate them here. Only this TU
// emits the symbols; only this TU's -ffp-contract setting matters.
// CMake pins this file to -ffp-contract=fast so apply_exact gets its
// FMA fusion win without ODR risk elsewhere.

#include "TheOtherBarry.h"
#include "Mekalele.h"

// TheOtherBarry — every variant observed in the binary as of
// 2026-05-31. If a new (BlendMode, TextureMode) pair is used, add its
// instantiation here AND an `extern template` line in TheOtherBarry.h.
template void TheOtherBarry<barry::TBlendMode::OVERWRITE,   barry::TTextureMode::NORMAL>        (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
template void TheOtherBarry<barry::TBlendMode::OVERWRITE,   barry::TTextureMode::TEXTURETEXTURE>(Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
template void TheOtherBarry<barry::TBlendMode::OVERWRITE,   barry::TTextureMode::NONE>          (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
template void TheOtherBarry<barry::TBlendMode::TRANSPARENT, barry::TTextureMode::NORMAL>        (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
template void TheOtherBarry<barry::TBlendMode::TRANSPARENT, barry::TTextureMode::NONE>          (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
template void TheOtherBarry<barry::TBlendMode::ADDITIVE,    barry::TTextureMode::NORMAL>        (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);

// MekaleleImpl — three targets (Opaque + TransparentFront + TransparentBack).
template void MekaleleImpl<MekaleleTarget::Opaque>           (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
template void MekaleleImpl<MekaleleTarget::TransparentFront> (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
template void MekaleleImpl<MekaleleTarget::TransparentBack>  (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
