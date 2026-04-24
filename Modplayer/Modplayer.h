#pragma once
typedef void *ModplayerHandle;

// Emscripten build stubs music out for now. The Rust modplayer-lib does
// cross-compile to wasm32-unknown-emscripten, but its sdl2 crate bundles
// its own SDL2 which collides with emcc's -sUSE_SDL=2 at link time.
// TODO(wasm): either strip the bundled SDL from the wasm rust build or
// replace the audio shim with a direct Emscripten audio backend.
#if defined(__EMSCRIPTEN__)
inline ModplayerHandle Modplayer_Create(const char* path) { return 0; }
inline void Modplayer_Start(ModplayerHandle handle) {}
inline void Modplayer_Stop(ModplayerHandle handle) {}
inline void Modplayer_SetOrder(ModplayerHandle handle, unsigned int order) {}

#else
extern "C" {
	ModplayerHandle Modplayer_Create(const char* path);
	void Modplayer_Start(ModplayerHandle handle);
	void Modplayer_Stop(ModplayerHandle handle);
	void Modplayer_SetOrder(ModplayerHandle handle, unsigned int order);
}
#endif
