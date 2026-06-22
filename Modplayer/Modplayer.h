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
