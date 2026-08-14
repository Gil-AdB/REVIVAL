#pragma once
typedef void *ModplayerHandle;

extern "C" {
	ModplayerHandle Modplayer_Create(const char* path);
	void Modplayer_Start(ModplayerHandle handle);
	void Modplayer_Stop(ModplayerHandle handle);
	void Modplayer_SetOrder(ModplayerHandle handle, unsigned int order);
	// Enable/disable the display path (oscilloscope copy, channel-status
	// snapshot, master FFT). Embedders that don't render a visualizer should
	// pass `false` to skip per-buffer FFT work. Defaults to enabled.
	void Modplayer_SetDisplay(ModplayerHandle handle, bool on);

	// Read the current playback position for music sync. Lock-free: it reads
	// the same display/status snapshot the player publishes each audio buffer,
	// so it never blocks and never touches the song mutex (safe to call from
	// any thread, including while the audio/mixer thread is running).
	//
	// Units:
	//   order      pattern-order index (position in the song's order table)
	//   row        row within the current pattern
	//   tickInRow  tick within the current row (0 .. speed-1)
	//   songTick   monotonic playback clock in MILLISECONDS since playback
	//              start; keeps counting across Modplayer_SetOrder jumps —
	//              the robust sync reference clock.
	//
	// Any out-pointer may be NULL (that field is skipped). The values only
	// advance while the display path is enabled: call
	// Modplayer_SetDisplay(handle, true) before polling. With display disabled
	// the player never republishes the snapshot, so the getter returns the
	// last-published (initially all-zero) values.
	void Modplayer_GetPosition(ModplayerHandle handle,
	                           unsigned int* order,
	                           unsigned int* row,
	                           unsigned int* tickInRow,
	                           unsigned long long* songTick);

#if defined(__EMSCRIPTEN__)
	// external-audio backend: host opens its own audio device (SDL_AudioDevice
	// in our case) and pulls samples via this entry point. `frames` must be
	// AUDIO_BUF_FRAMES (512) and `out` receives `frames * 2` interleaved
	// stereo f32s.
	void Modplayer_FillBuffer(ModplayerHandle handle, float* out, unsigned int frames);

	// Planar variant — writes `frames` f32s each into `left` and `right`.
	// Saves a deinterleave when the consumer expects per-channel buffers
	// (AudioWorkletNode's process() in particular).
	void Modplayer_FillBufferPlanar(ModplayerHandle handle,
	                                float* left, float* right,
	                                unsigned int frames);
#endif
}
