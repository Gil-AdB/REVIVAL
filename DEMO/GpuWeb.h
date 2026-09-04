#pragma once

namespace rev {

// Returns true if WebGL2 GPU hardware renderer is initialized and ready
bool GpuWeb_Init();

// Is GPU mode currently active?
bool GpuWeb_IsEnabled();
void GpuWeb_SetEnabled(bool enabled);

// Load a scene for GPU rendering (e.g. "greets", "fountain", "city", etc.)
bool GpuWeb_LoadScene(const char* sceneName, float demoT);

// Reanimate and render one frame on the GPU
// width, height: canvas dimensions in device pixels
// demoT: current demo timer value
void GpuWeb_RenderFrame(int width, int height, float demoT);

// Free GPU resources
void GpuWeb_Shutdown();

} // namespace rev
