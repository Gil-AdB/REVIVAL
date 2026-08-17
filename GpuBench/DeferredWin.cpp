// DeferredWin.cpp — Direct3D 11 backend for GpuBench (Windows).

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "DeferredWin.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace gpubench {

namespace {

struct FrameUniforms {
    float camRow0[4];
    float camRow1[4];
    float camRow2[4];
    float camSrc[4];
    float sx, ox, sy, oy;
    float dza, dzb, invSx, invSy;
    float nearZ, farZ, exposure;
    uint32_t numLights;
    uint32_t shadowsOn;
    float ambientFactor, diffuseFactor, specularFactor;
    float lightRangeScale;
    int32_t vizLight;
    float pad0[2];
    float clipPlane[4];
    uint32_t mirrorCount;
    float envReflGain;
    float reflUv[2];
    float aabbMin[4];
    float aabbMax[4];
    float envProbePos[8][4];
    float metalCompat[4];
    float flatAmbient[4];
    float hdrMode[4];
};

struct BatchUniforms {
    float rotRow0[4];
    float rotRow1[4];
    float rotRow2[4];
    float objPos[4];
    float baseColor[4];
    float matParams[4];
    float mapFlags[4];
    float misc[4];
    float misc2[4];
    float xpar[4];
};

void Die(const char *msg, HRESULT hr = S_OK) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "[GPUBENCH] FATAL: %s (HRESULT 0x%08X)\n", msg, (unsigned)hr);
    } else {
        std::fprintf(stderr, "[GPUBENCH] FATAL: %s\n", msg);
    }
    std::exit(1);
}

#include <windows.h>

std::string ReadFile(const char *path) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s(size_t(n < 0 ? 0 : n), '\0');
    if (n > 0 && std::fread(s.data(), 1, size_t(n), f) != size_t(n)) s.clear();
    std::fclose(f);
    return s;
}

std::string FindShaderFile(const char *name) {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char *lastSlash = std::strrchr(exePath, '\\');
    if (!lastSlash) lastSlash = std::strrchr(exePath, '/');
    if (lastSlash) *lastSlash = '\0';

    std::string candidate1 = std::string(exePath) + "/shaders/" + name;
    std::string s = ReadFile(candidate1.c_str());
    if (!s.empty()) return s;

    std::string candidate2 = std::string("shaders/") + name;
    s = ReadFile(candidate2.c_str());
    if (!s.empty()) return s;

    std::string candidate3 = std::string("GpuBench/shaders/") + name;
    s = ReadFile(candidate3.c_str());
    if (!s.empty()) return s;

    return ReadFile(name);
}

bool WritePPM(const char *path, const uint8_t *bgra, int w, int h, size_t rowBytes) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row(size_t(w) * 3);
    for (int y = 0; y < h; ++y) {
        const uint8_t *s = bgra + size_t(y) * rowBytes;
        for (int x = 0; x < w; ++x) {
            row[size_t(x) * 3 + 0] = s[size_t(x) * 4 + 0];  // R
            row[size_t(x) * 3 + 1] = s[size_t(x) * 4 + 1];  // G
            row[size_t(x) * 3 + 2] = s[size_t(x) * 4 + 2];  // B
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p * double(v.size() - 1);
    const size_t lo = size_t(std::floor(idx)), hi = size_t(std::ceil(idx));
    return v[lo] + (v[hi] - v[lo]) * (idx - double(lo));
}

ID3DBlob* CompileHLSL(const std::string &source, const char *entryPoint, const char *target) {
    ID3DBlob *code = nullptr;
    ID3DBlob *errors = nullptr;
    UINT flags = D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
    HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
                            entryPoint, target, flags, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors) {
            std::fprintf(stderr, "[GPUBENCH] Shader Compilation Error:\n%s\n",
                         (const char*)errors->GetBufferPointer());
            errors->Release();
        }
        Die("D3DCompile failed", hr);
    }
    if (errors) errors->Release();
    return code;
}

} // namespace

bool RunAlbedoWin(Scene &scene, const DeferredOptions &opt, const std::string &outPath) {
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    UINT createFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
                                   nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(hr)) Die("D3D11CreateDevice failed", hr);

    const int W = scene.xres, H = scene.yres;

    // Create Color Texture & RTV
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = W;
    texDesc.Height = H;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;

    ID3D11Texture2D *colorTex = nullptr;
    hr = device->CreateTexture2D(&texDesc, nullptr, &colorTex);
    if (FAILED(hr)) Die("CreateTexture2D colorTex failed", hr);

    ID3D11RenderTargetView *colorRTV = nullptr;
    hr = device->CreateRenderTargetView(colorTex, nullptr, &colorRTV);
    if (FAILED(hr)) Die("CreateRenderTargetView colorRTV failed", hr);

    // Create Depth Texture & DSV
    D3D11_TEXTURE2D_DESC depthDesc = texDesc;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    ID3D11Texture2D *depthTex = nullptr;
    hr = device->CreateTexture2D(&depthDesc, nullptr, &depthTex);
    if (FAILED(hr)) Die("CreateTexture2D depthTex failed", hr);

    ID3D11DepthStencilView *depthDSV = nullptr;
    hr = device->CreateDepthStencilView(depthTex, nullptr, &depthDSV);
    if (FAILED(hr)) Die("CreateDepthStencilView depthDSV failed", hr);

    // Staging texture for readback
    D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D *stagingTex = nullptr;
    hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
    if (FAILED(hr)) Die("CreateTexture2D stagingTex failed", hr);

    // Compile Shaders
    std::string hlslSource = FindShaderFile("albedo.hlsl");
    if (hlslSource.empty()) Die("Could not find albedo.hlsl shader file");

    ID3DBlob *vsBlob = CompileHLSL(hlslSource, "vs_albedo", "vs_5_0");
    ID3DBlob *psBlob = CompileHLSL(hlslSource, "ps_albedo", "ps_5_0");

    ID3D11VertexShader *vs = nullptr;
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    if (FAILED(hr)) Die("CreateVertexShader failed", hr);

    ID3D11PixelShader *ps = nullptr;
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
    if (FAILED(hr)) Die("CreatePixelShader failed", hr);

    // Input Layout
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, px), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, nx), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(Vertex, u),  D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    ID3D11InputLayout *layout = nullptr;
    hr = device->CreateInputLayout(layoutDesc, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);
    if (FAILED(hr)) Die("CreateInputLayout failed", hr);

    vsBlob->Release();
    psBlob->Release();

    // Create Vertex Buffer
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.ByteWidth = UINT(sizeof(Vertex) * scene.verts.size());
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbInitData = {};
    vbInitData.pSysMem = scene.verts.data();

    ID3D11Buffer *vb = nullptr;
    hr = device->CreateBuffer(&vbDesc, &vbInitData, &vb);
    if (FAILED(hr)) Die("CreateBuffer VB failed", hr);

    // Constant Buffers
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.ByteWidth = (UINT(sizeof(FrameUniforms)) + 15) & ~15;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    ID3D11Buffer *frameCB = nullptr;
    hr = device->CreateBuffer(&cbDesc, nullptr, &frameCB);
    if (FAILED(hr)) Die("CreateBuffer frameCB failed", hr);

    cbDesc.ByteWidth = (UINT(sizeof(BatchUniforms)) + 15) & ~15;
    ID3D11Buffer *batchCB = nullptr;
    hr = device->CreateBuffer(&cbDesc, nullptr, &batchCB);
    if (FAILED(hr)) Die("CreateBuffer batchCB failed", hr);

    // Load Textures
    std::vector<ID3D11ShaderResourceView*> texSRVs(scene.textures.size(), nullptr);
    for (size_t i = 0; i < scene.textures.size(); ++i) {
        const auto &ti = scene.textures[i];
        if (ti.rgba.empty()) continue;

        D3D11_TEXTURE2D_DESC tdesc = {};
        tdesc.Width = ti.w;
        tdesc.Height = ti.h;
        tdesc.MipLevels = 0;
        tdesc.ArraySize = 1;
        tdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tdesc.SampleDesc.Count = 1;
        tdesc.Usage = D3D11_USAGE_DEFAULT;
        tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        tdesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

        ID3D11Texture2D *tex = nullptr;
        hr = device->CreateTexture2D(&tdesc, nullptr, &tex);
        if (SUCCEEDED(hr)) {
            ID3D11ShaderResourceView *srv = nullptr;
            hr = device->CreateShaderResourceView(tex, nullptr, &srv);
            if (SUCCEEDED(hr)) {
                context->UpdateSubresource(tex, 0, nullptr, ti.rgba.data(), ti.w * 4, 0);
                context->GenerateMips(srv);
                texSRVs[i] = srv;
            }
            tex->Release();
        }
    }

    // Sampler State
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;

    ID3D11SamplerState *sampler = nullptr;
    hr = device->CreateSamplerState(&sampDesc, &sampler);
    if (FAILED(hr)) Die("CreateSamplerState failed", hr);

    // Query for timing
    D3D11_QUERY_DESC qDesc = {};
    qDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    ID3D11Query *disjointQuery = nullptr;
    device->CreateQuery(&qDesc, &disjointQuery);

    qDesc.Query = D3D11_QUERY_TIMESTAMP;
    ID3D11Query *tsStart = nullptr;
    ID3D11Query *tsEnd = nullptr;
    device->CreateQuery(&qDesc, &tsStart);
    device->CreateQuery(&qDesc, &tsEnd);

    // Setup Uniforms
    FrameUniforms fu = {};
    for (int c = 0; c < 3; ++c) {
        fu.camRow0[c] = scene.camera.rot[0][c];
        fu.camRow1[c] = scene.camera.rot[1][c];
        fu.camRow2[c] = scene.camera.rot[2][c];
        fu.camSrc[c]  = scene.camera.src[c];
    }
    fu.sx = 2.0f * scene.camera.perspX / W;
    fu.ox = 2.0f * scene.camera.cntrEX / W - 1.0f;
    fu.sy = 2.0f * scene.camera.perspY / H;
    fu.oy = 1.0f - 2.0f * scene.camera.cntrEY / H;

    const float n = scene.camera.nearZ, f = scene.camera.farZ;
    fu.dza = -n / (f - n);
    fu.dzb = n * f / (f - n);

    context->UpdateSubresource(frameCB, 0, nullptr, &fu, 0, 0);

    std::vector<BatchUniforms> bus(scene.batches.size());
    for (size_t i = 0; i < scene.batches.size(); ++i) {
        const auto &b = scene.batches[i];
        BatchUniforms &u = bus[i];
        for (int c = 0; c < 3; ++c) {
            u.rotRow0[c] = b.rot[0][c];
            u.rotRow1[c] = b.rot[1][c];
            u.rotRow2[c] = b.rot[2][c];
            u.objPos[c]   = b.pos[c];
            u.baseColor[c]= b.baseColor[c];
        }
        u.baseColor[3] = (b.textureIndex >= 0) ? 1.0f : 0.0f;
    }

    D3D11_VIEWPORT viewport = {};
    viewport.Width = (FLOAT)W;
    viewport.Height = (FLOAT)H;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    // Depth Stencil State for Reversed-Z (clearDepth = 0.0)
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;

    ID3D11DepthStencilState *dsState = nullptr;
    device->CreateDepthStencilState(&dsDesc, &dsState);

    // Rasterizer State
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.DepthClipEnable = TRUE;

    ID3D11RasterizerState *rsState = nullptr;
    device->CreateRasterizerState(&rsDesc, &rsState);

    auto renderFrame = [&](ID3D11Query *q1, ID3D11Query *q2, ID3D11Query *qDisjoint) {
        float clearColor[4] = { 0.02f, 0.02f, 0.04f, 1.0f };
        context->ClearRenderTargetView(colorRTV, clearColor);
        context->ClearDepthStencilView(depthDSV, D3D11_CLEAR_DEPTH, 0.0f, 0);

        context->RSSetState(rsState);
        context->OMSetDepthStencilState(dsState, 0);

        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, &colorRTV, depthDSV);

        context->IASetInputLayout(layout);
        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        context->VSSetShader(vs, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &frameCB);
        context->VSSetConstantBuffers(1, 1, &batchCB);

        context->PSSetShader(ps, nullptr, 0);
        context->PSSetConstantBuffers(1, 1, &batchCB);
        context->PSSetSamplers(0, 1, &sampler);

        if (q1) context->End(q1);

        for (size_t i = 0; i < scene.batches.size(); ++i) {
            const auto &b = scene.batches[i];
            context->UpdateSubresource(batchCB, 0, nullptr, &bus[i], 0, 0);

            if (b.textureIndex >= 0 && b.textureIndex < int(texSRVs.size()) && texSRVs[b.textureIndex]) {
                context->PSSetShaderResources(0, 1, &texSRVs[b.textureIndex]);
            } else if (!texSRVs.empty() && texSRVs[0]) {
                context->PSSetShaderResources(0, 1, &texSRVs[0]);
            }

            context->Draw(b.vertexCount, b.firstVertex);
        }

        if (q2) context->End(q2);
        if (qDisjoint) context->End(qDisjoint);
    };

    // Warmup
    std::fprintf(stderr, "[GPUBENCH] warmup %d frames…\n", opt.warmup);
    for (int i = 0; i < opt.warmup; ++i) {
        renderFrame(nullptr, nullptr, nullptr);
    }

    // Measure
    std::vector<double> gpuMs;
    gpuMs.reserve(size_t(opt.iters));
    for (int i = 0; i < opt.iters; ++i) {
        renderFrame(tsStart, tsEnd, disjointQuery);

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData;
        while (context->GetData(disjointQuery, &disjointData, sizeof(disjointData), 0) == S_FALSE) {}

        UINT64 t1 = 0, t2 = 0;
        while (context->GetData(tsStart, &t1, sizeof(t1), 0) == S_FALSE) {}
        while (context->GetData(tsEnd, &t2, sizeof(t2), 0) == S_FALSE) {}

        if (!disjointData.Disjoint && disjointData.Frequency > 0) {
            double ms = double(t2 - t1) * 1000.0 / double(disjointData.Frequency);
            gpuMs.push_back(ms);
        }
    }

    if (!gpuMs.empty()) {
        const double med = Percentile(gpuMs, 0.50);
        std::fprintf(stderr,
            "\n[GPUBENCH] ===== RESULT =====\n"
            "[GPUBENCH] scene=%s pose t=%d %dx%d Direct3D11\n"
            "[GPUBENCH] draws=%zu tris=%u gpuVerts=%zu textures=%u\n"
            "[GPUBENCH] GPU frame ms over %zu frames (after %d warmup):\n"
            "[GPUBENCH]   median %.4f   p5 %.4f   p95 %.4f   min %.4f   max %.4f\n"
            "[GPUBENCH]   => %.1f FPS equivalent\n",
            opt.outPath.c_str(), opt.iters, scene.xres, scene.yres,
            scene.batches.size(), scene.faceCount, scene.verts.size(), scene.texturesLoaded,
            gpuMs.size(), opt.warmup,
            med, Percentile(gpuMs, 0.05), Percentile(gpuMs, 0.95),
            *std::min_element(gpuMs.begin(), gpuMs.end()),
            *std::max_element(gpuMs.begin(), gpuMs.end()),
            med > 0.0 ? 1000.0 / med : 0.0);
    }

    // Readback
    if (!outPath.empty()) {
        context->CopyResource(stagingTex, colorTex);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
            if (WritePPM(outPath.c_str(), (const uint8_t*)mapped.pData, W, H, mapped.RowPitch))
                std::fprintf(stderr, "[GPUBENCH] wrote %s\n", outPath.c_str());
            context->Unmap(stagingTex, 0);
        }
    }

    // Cleanup
    for (auto srv : texSRVs) if (srv) srv->Release();
    sampler->Release();
    batchCB->Release();
    frameCB->Release();
    vb->Release();
    layout->Release();
    ps->Release();
    vs->Release();
    stagingTex->Release();
    depthDSV->Release();
    depthTex->Release();
    colorRTV->Release();
    colorTex->Release();
    if (disjointQuery) disjointQuery->Release();
    if (tsStart) tsStart->Release();
    if (tsEnd) tsEnd->Release();
    context->Release();
    device->Release();

    return true;
}

bool RunDeferredWin(Scene &scene, const DeferredOptions &opt,
                    const std::string &shaderPath, DeferredResult &out) {
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    UINT createFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;

    const int W = scene.xres, H = scene.yres;

    // Interactive SDL Window & DXGI SwapChain
    SDL_Window *sdlWindow = nullptr;
    IDXGISwapChain *swapChain = nullptr;
    ID3D11RenderTargetView *swapRTV = nullptr;

    if (opt.interactive) {
        if (SDL_Init(SDL_INIT_VIDEO) == 0) {
            sdlWindow = SDL_CreateWindow("GpuBench (Direct3D 11 Deferred)",
                                         SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                         W, H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
            if (sdlWindow) {
                SDL_SysWMinfo wmInfo;
                SDL_VERSION(&wmInfo.version);
                if (SDL_GetWindowWMInfo(sdlWindow, &wmInfo)) {
                    HWND hwnd = wmInfo.info.win.window;
                    DXGI_SWAP_CHAIN_DESC scd = {};
                    scd.BufferCount = 1;
                    scd.BufferDesc.Width = W;
                    scd.BufferDesc.Height = H;
                    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    scd.BufferDesc.RefreshRate.Numerator = 60;
                    scd.BufferDesc.RefreshRate.Denominator = 1;
                    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                    scd.OutputWindow = hwnd;
                    scd.SampleDesc.Count = 1;
                    scd.Windowed = TRUE;
                    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

                    HRESULT hrChain = D3D11CreateDeviceAndSwapChain(
                        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
                        nullptr, 0, D3D11_SDK_VERSION, &scd, &swapChain, &device, &featureLevel, &context);
                    if (SUCCEEDED(hrChain) && swapChain) {
                        ID3D11Texture2D *backBuf = nullptr;
                        swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuf);
                        if (backBuf) {
                            device->CreateRenderTargetView(backBuf, nullptr, &swapRTV);
                            backBuf->Release();
                        }
                    }
                }
            }
        }
    }

    if (!device) {
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
                                       nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context);
        if (FAILED(hr)) Die("D3D11CreateDevice failed", hr);
    }

    // G-Buffer Textures & RTVs (4 MRT targets: Albedo+AO, OctNormal, MaterialParams, Mirror+Metalness)
    D3D11_TEXTURE2D_DESC gbufDesc = {};
    gbufDesc.Width = W;
    gbufDesc.Height = H;
    gbufDesc.MipLevels = 1;
    gbufDesc.ArraySize = 1;
    gbufDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    gbufDesc.SampleDesc.Count = 1;
    gbufDesc.Usage = D3D11_USAGE_DEFAULT;
    gbufDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D *gbufAlbedo = nullptr;
    ID3D11Texture2D *gbufNormal = nullptr;
    ID3D11Texture2D *gbufMaterial = nullptr;
    ID3D11Texture2D *gbufMirror = nullptr;
    device->CreateTexture2D(&gbufDesc, nullptr, &gbufAlbedo);
    device->CreateTexture2D(&gbufDesc, nullptr, &gbufNormal);
    device->CreateTexture2D(&gbufDesc, nullptr, &gbufMaterial);
    device->CreateTexture2D(&gbufDesc, nullptr, &gbufMirror);

    ID3D11RenderTargetView *gbufRTVs[4] = {};
    device->CreateRenderTargetView(gbufAlbedo, nullptr, &gbufRTVs[0]);
    device->CreateRenderTargetView(gbufNormal, nullptr, &gbufRTVs[1]);
    device->CreateRenderTargetView(gbufMaterial, nullptr, &gbufRTVs[2]);
    device->CreateRenderTargetView(gbufMirror, nullptr, &gbufRTVs[3]);

    ID3D11ShaderResourceView *gbufSRVs[5] = {};
    device->CreateShaderResourceView(gbufAlbedo, nullptr, &gbufSRVs[0]);
    device->CreateShaderResourceView(gbufNormal, nullptr, &gbufSRVs[1]);
    device->CreateShaderResourceView(gbufMaterial, nullptr, &gbufSRVs[2]);
    device->CreateShaderResourceView(gbufMirror, nullptr, &gbufSRVs[3]);

    // G-Buffer Depth Texture & DSV
    D3D11_TEXTURE2D_DESC depthDesc = gbufDesc;
    depthDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D *depthTex = nullptr;
    device->CreateTexture2D(&depthDesc, nullptr, &depthTex);

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ID3D11DepthStencilView *depthDSV = nullptr;
    device->CreateDepthStencilView(depthTex, &dsvDesc, &depthDSV);

    D3D11_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
    depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depthSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(depthTex, &depthSrvDesc, &gbufSRVs[4]);

    // Offscreen Resolve Texture & RTV
    ID3D11Texture2D *resolveTex = nullptr;
    ID3D11RenderTargetView *resolveRTV = nullptr;
    device->CreateTexture2D(&gbufDesc, nullptr, &resolveTex);
    device->CreateRenderTargetView(resolveTex, nullptr, &resolveRTV);

    // Staging texture for PPM readback
    D3D11_TEXTURE2D_DESC stagingDesc = gbufDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *stagingTex = nullptr;
    device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);

    // Compile Deferred Shaders
    std::string hlslSource = FindShaderFile("deferred.hlsl");
    if (hlslSource.empty()) Die("Could not find deferred.hlsl shader file");

    ID3DBlob *vsGbufBlob = CompileHLSL(hlslSource, "vs_gbuffer", "vs_5_0");
    ID3DBlob *psGbufBlob = CompileHLSL(hlslSource, "ps_gbuffer", "ps_5_0");
    ID3DBlob *vsQuadBlob = CompileHLSL(hlslSource, "vs_quad", "vs_5_0");
    ID3DBlob *psResolveBlob = CompileHLSL(hlslSource, "ps_resolve", "ps_5_0");

    ID3D11VertexShader *vsGbuf = nullptr;
    ID3D11PixelShader *psGbuf = nullptr;
    ID3D11VertexShader *vsQuad = nullptr;
    ID3D11PixelShader *psResolve = nullptr;

    device->CreateVertexShader(vsGbufBlob->GetBufferPointer(), vsGbufBlob->GetBufferSize(), nullptr, &vsGbuf);
    device->CreatePixelShader(psGbufBlob->GetBufferPointer(), psGbufBlob->GetBufferSize(), nullptr, &psGbuf);
    device->CreateVertexShader(vsQuadBlob->GetBufferPointer(), vsQuadBlob->GetBufferSize(), nullptr, &vsQuad);
    device->CreatePixelShader(psResolveBlob->GetBufferPointer(), psResolveBlob->GetBufferSize(), nullptr, &psResolve);

    // Input Layout for G-Buffer pass
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(Vertex, px), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(Vertex, nx), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(Vertex, u),  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, tx), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *layout = nullptr;
    device->CreateInputLayout(layoutDesc, 4, vsGbufBlob->GetBufferPointer(), vsGbufBlob->GetBufferSize(), &layout);

    vsGbufBlob->Release();
    psGbufBlob->Release();
    vsQuadBlob->Release();
    psResolveBlob->Release();

    // Create Vertex Buffer
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.ByteWidth = UINT(sizeof(Vertex) * scene.verts.size());
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbInitData = {};
    vbInitData.pSysMem = scene.verts.data();

    ID3D11Buffer *vb = nullptr;
    device->CreateBuffer(&vbDesc, &vbInitData, &vb);

    struct GpuLightWin {
        float pos[4];        // xyz = pos, w = range
        float color[4];      // xyz = color * intensity / 255.0, w = invRange
        float dir[4];        // xyz = dir, w = isSpot
        float params[4];     // x = cosInner, y = cosOuter, z = shadowIndex, w = pad
        float sRow0[4];      // Shadow rotation matrix row 0
        float sRow1[4];      // Shadow rotation matrix row 1
        float sRow2[4];      // Shadow rotation matrix row 2
        float shadowNear, shadowFar;
        float pad[2];
    };

    struct LightUniformsWin {
        GpuLightWin lights[64];
    };

    // Constant Buffers
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.ByteWidth = (UINT(sizeof(FrameUniforms)) + 15) & ~15;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    ID3D11Buffer *frameCB = nullptr;
    device->CreateBuffer(&cbDesc, nullptr, &frameCB);

    cbDesc.ByteWidth = (UINT(sizeof(BatchUniforms)) + 15) & ~15;
    ID3D11Buffer *batchCB = nullptr;
    device->CreateBuffer(&cbDesc, nullptr, &batchCB);

    cbDesc.ByteWidth = sizeof(LightUniformsWin);
    ID3D11Buffer *lightCB = nullptr;
    device->CreateBuffer(&cbDesc, nullptr, &lightCB);

    // Load Textures
    std::vector<ID3D11ShaderResourceView*> texSRVs(scene.textures.size(), nullptr);
    for (size_t i = 0; i < scene.textures.size(); ++i) {
        const auto &ti = scene.textures[i];
        if (ti.rgba.empty()) continue;

        D3D11_TEXTURE2D_DESC tdesc = {};
        tdesc.Width = ti.w;
        tdesc.Height = ti.h;
        tdesc.MipLevels = 0;
        tdesc.ArraySize = 1;
        tdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tdesc.SampleDesc.Count = 1;
        tdesc.Usage = D3D11_USAGE_DEFAULT;
        tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        tdesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

        ID3D11Texture2D *tex = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&tdesc, nullptr, &tex))) {
            ID3D11ShaderResourceView *srv = nullptr;
            if (SUCCEEDED(device->CreateShaderResourceView(tex, nullptr, &srv))) {
                context->UpdateSubresource(tex, 0, nullptr, ti.rgba.data(), ti.w * 4, 0);
                context->GenerateMips(srv);
                texSRVs[i] = srv;
            }
            tex->Release();
        }
    }

    // Sampler State
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;

    ID3D11SamplerState *sampler = nullptr;
    device->CreateSamplerState(&sampDesc, &sampler);

    // Timing Queries
    D3D11_QUERY_DESC qDesc = {};
    qDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    ID3D11Query *disjointQuery = nullptr;
    device->CreateQuery(&qDesc, &disjointQuery);

    qDesc.Query = D3D11_QUERY_TIMESTAMP;
    ID3D11Query *tsStart = nullptr;
    ID3D11Query *tsEnd = nullptr;
    device->CreateQuery(&qDesc, &tsStart);
    device->CreateQuery(&qDesc, &tsEnd);

    // Uniform setup
    FrameUniforms fu = {};
    // Refresh LightUniforms for offscreen mode
    LightUniformsWin lu = {};
    size_t numActiveLights = 0;
    for (size_t i = 0; i < scene.lights.size() && numActiveLights < 64; ++i) {
        const auto &L = scene.lights[i];
        if (!std::isfinite(L.pos[0]) || L.range <= 0.0f) continue;
        GpuLightWin &g = lu.lights[numActiveLights++];
        g.pos[0] = L.pos[0];
        g.pos[1] = L.pos[1];
        g.pos[2] = L.pos[2];
        g.pos[3] = L.range;

        float c01[3] = { L.color[0] / 255.0f, L.color[1] / 255.0f, L.color[2] / 255.0f };
        g.color[0] = c01[0] * L.intensity;
        g.color[1] = c01[1] * L.intensity;
        g.color[2] = c01[2] * L.intensity;
        g.color[3] = 1.0f / std::max(L.range, 1e-4f);

        g.dir[0] = L.dir[0];
        g.dir[1] = L.dir[1];
        g.dir[2] = L.dir[2];
        g.dir[3] = L.isSpot ? 1.0f : 0.0f;

        g.params[0] = L.cosInner;
        g.params[1] = L.cosOuter;
        g.params[2] = L.shadowIndex;
        g.sRow0[0] = L.shadowRot[0][0]; g.sRow0[1] = L.shadowRot[0][1]; g.sRow0[2] = L.shadowRot[0][2];
        g.sRow1[0] = L.shadowRot[1][0]; g.sRow1[1] = L.shadowRot[1][1]; g.sRow1[2] = L.shadowRot[1][2];
        g.sRow2[0] = L.shadowRot[2][0]; g.sRow2[1] = L.shadowRot[2][1]; g.sRow2[2] = L.shadowRot[2][2];
        g.sRow0[3] = 1.0f / std::max(L.shadowTanHalfFov, 1e-4f);
        g.sRow1[3] = 0.0f;
        g.sRow2[3] = 0.0f;
        g.shadowNear = L.shadowNear;
        g.shadowFar = L.shadowFar;
        g.params[3] = 0.0f;
    }
    fu.numLights = (UINT)numActiveLights;

    for (int c = 0; c < 3; ++c) {
        fu.camRow0[c] = scene.camera.rot[0][c];
        fu.camRow1[c] = scene.camera.rot[1][c];
        fu.camRow2[c] = scene.camera.rot[2][c];
        fu.camSrc[c]  = scene.camera.src[c];
    }
    fu.sx = 2.0f * scene.camera.perspX / W;
    fu.ox = 2.0f * scene.camera.cntrEX / W - 1.0f;
    fu.sy = 2.0f * scene.camera.perspY / H;
    fu.oy = 1.0f - 2.0f * scene.camera.cntrEY / H;
    fu.exposure = opt.exposure;

    const float n = scene.camera.nearZ, f = scene.camera.farZ;
    fu.dza = -n / (f - n);
    fu.dzb = n * f / (f - n);
    fu.invSx = 1.0f / fu.sx;
    fu.invSy = 1.0f / fu.sy;
    fu.nearZ = n;
    fu.farZ = f;

    // Shading factors — must be non-zero or lighting is silenced
    fu.lightRangeScale = opt.lightRangeScale;   // default 1.0
    fu.diffuseFactor   = 1.0f;
    fu.specularFactor  = 1.0f;
    fu.ambientFactor   = 0.25f;                 // GREETS.CPP ~3005
    fu.vizLight        = opt.vizLight;

    // Ambient: if scene uses SH ambient, shader evaluates sky gradient;
    // otherwise use flat authored Scene::Ambient.
    fu.flatAmbient[0] = scene.ambient[0] * (1.0f / 255.0f);
    fu.flatAmbient[1] = scene.ambient[1] * (1.0f / 255.0f);
    fu.flatAmbient[2] = scene.ambient[2] * (1.0f / 255.0f);
    fu.flatAmbient[3] = scene.shAmbient ? 0.0f : 1.0f;

    // HDR mode: greets defaults hdrLinear = true
    fu.hdrMode[0] = opt.hdrLinear ? 1.0f : 0.0f;

    // Environment reflection gain
    fu.envReflGain = 1.0f;

    // Scene AABB for env probes
    for (int c = 0; c < 3; ++c) {
        fu.aabbMin[c] = scene.aabbMin[c];
        fu.aabbMax[c] = scene.aabbMax[c];
    }
    for (size_t i = 0; i < scene.envProbes.size() && i < 8; ++i)
        for (int c = 0; c < 3; ++c) fu.envProbePos[i][c] = scene.envProbes[i].pos[c];

    context->UpdateSubresource(frameCB, 0, nullptr, &fu, 0, 0);
    context->UpdateSubresource(lightCB, 0, nullptr, &lu, 0, 0);

    std::vector<BatchUniforms> bus(scene.batches.size());
    for (size_t i = 0; i < scene.batches.size(); ++i) {
        const auto &b = scene.batches[i];
        BatchUniforms &u = bus[i];
        for (int c = 0; c < 3; ++c) {
            u.rotRow0[c] = b.rot[0][c];
            u.rotRow1[c] = b.rot[1][c];
            u.rotRow2[c] = b.rot[2][c];
            u.objPos[c]   = b.pos[c];
            u.baseColor[c]= b.baseColor[c];
        }
        u.baseColor[3] = (b.textureIndex >= 0) ? 1.0f : 0.0f;

        u.matParams[0] = b.diffuse;
        u.matParams[1] = b.specular;
        u.matParams[2] = std::sqrt(2.0f / (float(b.glossiness) + 2.0f));
        u.matParams[3] = b.luminosity;

        u.mapFlags[0] = (b.normalTexIndex >= 0) ? 1.0f : 0.0f;
        u.mapFlags[1] = (b.roughTexIndex >= 0) ? 1.0f : 0.0f;
        u.mapFlags[2] = b.aoInAlpha ? 1.0f : 0.0f;
        u.mapFlags[3] = b.parallaxScale;

        u.misc[0] = 0.0f;
        u.misc[1] = (b.aoTexIndex >= 0) ? 1.0f : 0.0f;
        u.misc[2] = (b.metalTexIndex >= 0) ? 1.0f : 0.0f;
        u.misc[3] = 2.0f * b.aoStrength;

        u.misc2[0] = float(b.envProbe);
        u.misc2[1] = b.reflection;
        u.misc2[2] = b.xparBlendAlpha;
        u.misc2[3] = 0.5f;

        u.xpar[0] = b.luminosity;
        u.xpar[1] = b.specular;
        u.xpar[2] = float(b.glossiness);
        u.xpar[3] = b.additive ? 1.0f : 0.0f;
    }

    D3D11_VIEWPORT viewport = {};
    viewport.Width = (FLOAT)W;
    viewport.Height = (FLOAT)H;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    // Depth Stencil State for Reversed-Z (clearDepth = 0.0)
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;

    ID3D11DepthStencilState *dsState = nullptr;
    device->CreateDepthStencilState(&dsDesc, &dsState);

    // Disabled Depth Stencil State for Resolve Quad pass
    D3D11_DEPTH_STENCIL_DESC dsOffDesc = {};
    dsOffDesc.DepthEnable = FALSE;
    dsOffDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;

    ID3D11DepthStencilState *dsOffState = nullptr;
    device->CreateDepthStencilState(&dsOffDesc, &dsOffState);

    // Rasterizer State
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.DepthClipEnable = TRUE;

    ID3D11RasterizerState *rsState = nullptr;
    device->CreateRasterizerState(&rsDesc, &rsState);


    // --- Shadow Allocations ---
    std::string vsShadowSource = FindShaderFile("vs_shadow.hlsl");
    if (vsShadowSource.empty()) Die("Could not find vs_shadow.hlsl shader file");
    ID3DBlob *vsShadowBlob = CompileHLSL(vsShadowSource, "vs_shadow", "vs_5_0");
    ID3D11VertexShader *vsShadow = nullptr;
    device->CreateVertexShader(vsShadowBlob->GetBufferPointer(), vsShadowBlob->GetBufferSize(), nullptr, &vsShadow);

    D3D11_INPUT_ELEMENT_DESC layoutShadowDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, px), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *layoutShadow = nullptr;
    device->CreateInputLayout(layoutShadowDesc, 1, vsShadowBlob->GetBufferPointer(), vsShadowBlob->GetBufferSize(), &layoutShadow);
    vsShadowBlob->Release();

    struct ShadowUniformsWin {
        float row0[4];
        float row1[4];
        float row2[4];
        float lightPos[4];
        float sNearFar[4]; // x=dza, y=dzb, z=projScale, w=pad
    };
    D3D11_BUFFER_DESC scbDesc = {};
    scbDesc.Usage = D3D11_USAGE_DEFAULT;
    scbDesc.ByteWidth = (UINT(sizeof(ShadowUniformsWin)) + 15) & ~15;
    scbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer *shadowCB = nullptr;
    device->CreateBuffer(&scbDesc, nullptr, &shadowCB);

    const int shadowDim = 1024;
    ID3D11Texture2D *shadowSpotTex[16] = { nullptr };
    ID3D11DepthStencilView *shadowSpotDSVs[16] = { nullptr };
    ID3D11ShaderResourceView *shadowSpotSRVs[16] = { nullptr };
    for (int i = 0; i < 16; ++i) {
        D3D11_TEXTURE2D_DESC sDesc = {};
        sDesc.Width = shadowDim;
        sDesc.Height = shadowDim;
        sDesc.MipLevels = 1;
        sDesc.ArraySize = 1;
        sDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        sDesc.SampleDesc.Count = 1;
        sDesc.Usage = D3D11_USAGE_DEFAULT;
        sDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(device->CreateTexture2D(&sDesc, nullptr, &shadowSpotTex[i]))) {
            D3D11_DEPTH_STENCIL_VIEW_DESC sDsvDesc = {};
            sDsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
            sDsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            device->CreateDepthStencilView(shadowSpotTex[i], &sDsvDesc, &shadowSpotDSVs[i]);
            
            D3D11_SHADER_RESOURCE_VIEW_DESC sSrvDesc = {};
            sSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            sSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sSrvDesc.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(shadowSpotTex[i], &sSrvDesc, &shadowSpotSRVs[i]);
        }
    }

    ID3D11Texture2D *shadowCubeTex[16] = { nullptr };
    ID3D11DepthStencilView *shadowCubeDSVs[16][6] = { { nullptr } };
    ID3D11ShaderResourceView *shadowCubeSRVs[16] = { nullptr };
    for (int i = 0; i < 16; ++i) {
        D3D11_TEXTURE2D_DESC cDesc = {};
        cDesc.Width = shadowDim;
        cDesc.Height = shadowDim;
        cDesc.MipLevels = 1;
        cDesc.ArraySize = 6;
        cDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        cDesc.SampleDesc.Count = 1;
        cDesc.Usage = D3D11_USAGE_DEFAULT;
        cDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        cDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        if (SUCCEEDED(device->CreateTexture2D(&cDesc, nullptr, &shadowCubeTex[i]))) {
            for (int face = 0; face < 6; ++face) {
                D3D11_DEPTH_STENCIL_VIEW_DESC cDsvDesc = {};
                cDsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
                cDsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                cDsvDesc.Texture2DArray.FirstArraySlice = face;
                cDsvDesc.Texture2DArray.ArraySize = 1;
                device->CreateDepthStencilView(shadowCubeTex[i], &cDsvDesc, &shadowCubeDSVs[i][face]);
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC cSrvDesc = {};
            cSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            cSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            cSrvDesc.TextureCube.MipLevels = 1;
            device->CreateShaderResourceView(shadowCubeTex[i], &cSrvDesc, &shadowCubeSRVs[i]);
        }
    }

    D3D11_SAMPLER_DESC sSampDesc = {};
    sSampDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sSampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sSampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sSampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sSampDesc.ComparisonFunc = D3D11_COMPARISON_LESS;
    ID3D11SamplerState *shadowSampler = nullptr;
    device->CreateSamplerState(&sSampDesc, &shadowSampler);

    // --- Mirror map allocations ---
    const int kMaxMirrors = 4;
    const int nMirrors = std::min<int>((int)scene.mirrors.size(), kMaxMirrors);
    ID3D11Texture2D *mirrorTex = nullptr;
    ID3D11ShaderResourceView *mirrorTexSRV = nullptr;
    ID3D11RenderTargetView *mirrorTexRTVs[4] = {};
    if (nMirrors > 0) {
        D3D11_TEXTURE2D_DESC mDesc = {};
        mDesc.Width = W;
        mDesc.Height = H;
        mDesc.MipLevels = 1;
        mDesc.ArraySize = kMaxMirrors;
        mDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        mDesc.SampleDesc.Count = 1;
        mDesc.Usage = D3D11_USAGE_DEFAULT;
        mDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(device->CreateTexture2D(&mDesc, nullptr, &mirrorTex))) {
            D3D11_SHADER_RESOURCE_VIEW_DESC mSrvDesc = {};
            mSrvDesc.Format = mDesc.Format;
            mSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            mSrvDesc.Texture2DArray.ArraySize = kMaxMirrors;
            mSrvDesc.Texture2DArray.FirstArraySlice = 0;
            mSrvDesc.Texture2DArray.MipLevels = 1;
            device->CreateShaderResourceView(mirrorTex, &mSrvDesc, &mirrorTexSRV);
            
            for (int i = 0; i < kMaxMirrors; ++i) {
                D3D11_RENDER_TARGET_VIEW_DESC mRtvDesc = {};
                mRtvDesc.Format = mDesc.Format;
                mRtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                mRtvDesc.Texture2DArray.FirstArraySlice = i;
                mRtvDesc.Texture2DArray.ArraySize = 1;
                device->CreateRenderTargetView(mirrorTex, &mRtvDesc, &mirrorTexRTVs[i]);
            }
        }
    }
    auto renderDeferredFrame = [&](ID3D11Query *q1, ID3D11Query *q2, ID3D11Query *qDisjoint) {

        
        // --- 0. SHADOW PASS ---
        if (vsShadow) {
            context->IASetInputLayout(layoutShadow);
            context->VSSetShader(vsShadow, nullptr, 0);
            context->PSSetShader(nullptr, nullptr, 0); // pure depth pass
            
            // Clear all active spot shadow maps
            for (size_t i = 0; i < scene.lights.size() && i < 16; ++i) {
                const auto &L = scene.lights[i];
                if (L.shadowIndex < 0 || !L.isSpot) continue;
                context->ClearDepthStencilView(shadowSpotDSVs[L.shadowIndex], D3D11_CLEAR_DEPTH, 1.0f, 0);
            }

            for (size_t i = 0; i < scene.lights.size() && i < 16; ++i) {
                const auto &L = scene.lights[i];
                if (L.shadowIndex < 0 || !L.isSpot) continue;
                int sIdx = L.shadowIndex;
                
                ShadowUniformsWin su = {};
                for(int c=0; c<3; ++c) {
                    su.row0[c] = L.shadowRot[0][c];
                    su.row1[c] = L.shadowRot[1][c];
                    su.row2[c] = L.shadowRot[2][c];
                    su.lightPos[c] = L.pos[c];
                }
                float sn = L.shadowNear, sf = L.shadowFar;
                su.sNearFar[0] = -sn / (sf - sn); // dza
                su.sNearFar[1] = sn * sf / (sf - sn); // dzb
                su.sNearFar[2] = 1.0f / std::max(L.shadowTanHalfFov, 1e-4f); // projScale
                
                context->UpdateSubresource(shadowCB, 0, nullptr, &su, 0, 0);
                context->VSSetConstantBuffers(2, 1, &shadowCB);
                
                context->OMSetRenderTargets(0, nullptr, shadowSpotDSVs[sIdx]);
                D3D11_VIEWPORT vp = {0, 0, (float)shadowDim, (float)shadowDim, 0.0f, 1.0f};
                context->RSSetViewports(1, &vp);
                
                for (size_t bidx = 0; bidx < scene.batches.size(); ++bidx) {
                    if (scene.batches[bidx].transparent) continue;
                    context->VSSetConstantBuffers(1, 1, &batchCB); // Assuming batchCB is correctly updated
                    // Wait, batchCB is updated per batch in the G-Buffer loop!
                    // In the previous version, I used `bus[bidx]`!
                    context->UpdateSubresource(batchCB, 0, nullptr, &bus[bidx], 0, 0);
                    context->Draw(scene.batches[bidx].vertexCount, scene.batches[bidx].firstVertex);
                }
            }
            
            // Clear OM state
            ID3D11RenderTargetView* nullRTV = nullptr;
            context->OMSetRenderTargets(1, &nullRTV, nullptr);
        }
        // --- END SHADOW PASS ---

        // --- Pass 1: G-Buffer ---
        float clearGbuf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < 4; ++i) context->ClearRenderTargetView(gbufRTVs[i], clearGbuf);
        context->ClearDepthStencilView(depthDSV, D3D11_CLEAR_DEPTH, 0.0f, 0);

        context->RSSetState(rsState);
        context->OMSetDepthStencilState(dsState, 0);

        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(4, gbufRTVs, depthDSV);

        context->IASetInputLayout(layout);
        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        context->VSSetShader(vsGbuf, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &frameCB);
        context->VSSetConstantBuffers(1, 1, &batchCB);

        context->PSSetShader(psGbuf, nullptr, 0);
        context->PSSetConstantBuffers(1, 1, &batchCB);
        context->PSSetSamplers(0, 1, &sampler);

        if (q1) context->End(q1);

        for (size_t i = 0; i < scene.batches.size(); ++i) {
            const auto &b = scene.batches[i];
            context->UpdateSubresource(batchCB, 0, nullptr, &bus[i], 0, 0);

            ID3D11ShaderResourceView *batchSRVs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
            if (b.textureIndex >= 0 && b.textureIndex < int(texSRVs.size()))
                batchSRVs[0] = texSRVs[b.textureIndex];
            else if (!texSRVs.empty())
                batchSRVs[0] = texSRVs[0];

            if (b.normalTexIndex >= 0 && b.normalTexIndex < int(texSRVs.size()))
                batchSRVs[1] = texSRVs[b.normalTexIndex];
            if (b.roughTexIndex >= 0 && b.roughTexIndex < int(texSRVs.size()))
                batchSRVs[2] = texSRVs[b.roughTexIndex];
            if (b.aoTexIndex >= 0 && b.aoTexIndex < int(texSRVs.size()))
                batchSRVs[3] = texSRVs[b.aoTexIndex];
            if (b.metalTexIndex >= 0 && b.metalTexIndex < int(texSRVs.size()))
                batchSRVs[4] = texSRVs[b.metalTexIndex];

            context->PSSetShaderResources(0, 5, batchSRVs);

            context->Draw(b.vertexCount, b.firstVertex);
        }


        // --- Mirror Lighting/Resolve Pass ---
        for (int mIdx = 0; mIdx < nMirrors; ++mIdx) {
            auto &mirror = scene.mirrors[mIdx];
            
            FrameUniforms mfu = fu; // Copy frame uniforms
            // Create reflection matrix
            float R[3][3] = {
                {1.0f - 2.0f * mirror.n[0] * mirror.n[0], -2.0f * mirror.n[0] * mirror.n[1], -2.0f * mirror.n[0] * mirror.n[2]},
                {-2.0f * mirror.n[1] * mirror.n[0], 1.0f - 2.0f * mirror.n[1] * mirror.n[1], -2.0f * mirror.n[1] * mirror.n[2]},
                {-2.0f * mirror.n[2] * mirror.n[0], -2.0f * mirror.n[2] * mirror.n[1], 1.0f - 2.0f * mirror.n[2] * mirror.n[2]}
            };
            
            float newRot[3][3];
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    newRot[r][c] = fu.camRow0[c] * R[0][r] + fu.camRow1[c] * R[1][r] + fu.camRow2[c] * R[2][r];
                }
            }
            for(int c=0; c<3; ++c) {
                mfu.camRow0[c] = newRot[0][c];
                mfu.camRow1[c] = newRot[1][c];
                mfu.camRow2[c] = newRot[2][c];
            }
            float d = mirror.d;
            float dotP = fu.camSrc[0]*mirror.n[0] + fu.camSrc[1]*mirror.n[1] + fu.camSrc[2]*mirror.n[2];
            for(int c=0; c<3; ++c) mfu.camSrc[c] -= 2.0f * (dotP + d) * mirror.n[c];
            
            mfu.clipPlane[0] = mirror.n[0];
            mfu.clipPlane[1] = mirror.n[1];
            mfu.clipPlane[2] = mirror.n[2];
            mfu.clipPlane[3] = d;
            
            context->UpdateSubresource(frameCB, 0, nullptr, &mfu, 0, 0);
            context->VSSetConstantBuffers(0, 1, &frameCB);
            context->PSSetConstantBuffers(0, 1, &frameCB);
            
            float clearGbufM[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (int i = 0; i < 4; ++i) context->ClearRenderTargetView(gbufRTVs[i], clearGbufM);
            context->ClearDepthStencilView(depthDSV, D3D11_CLEAR_DEPTH, 0.0f, 0);
            
            context->RSSetState(rsState);
            context->OMSetDepthStencilState(dsState, 0);
            context->RSSetViewports(1, &viewport);
            context->OMSetRenderTargets(4, gbufRTVs, depthDSV);
            context->IASetInputLayout(layout);
            UINT strideM = sizeof(Vertex);
            UINT offsetM = 0;
            context->IASetVertexBuffers(0, 1, &vb, &strideM, &offsetM);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vsGbuf, nullptr, 0);
            context->PSSetShader(psGbuf, nullptr, 0);
    
            for (size_t bidx = 0; bidx < scene.batches.size(); ++bidx) {
                const auto &b = scene.batches[bidx];
                context->UpdateSubresource(batchCB, 0, nullptr, &bus[bidx], 0, 0);
                context->VSSetConstantBuffers(1, 1, &batchCB);
                context->PSSetConstantBuffers(1, 1, &batchCB);
                
                ID3D11ShaderResourceView *batchSRVs[5] = { nullptr };
                if (b.textureIndex >= 0 && b.textureIndex < (int)texSRVs.size()) batchSRVs[0] = texSRVs[b.textureIndex];
                else if (!texSRVs.empty()) batchSRVs[0] = texSRVs[0];
                if (b.normalTexIndex >= 0 && b.normalTexIndex < (int)texSRVs.size()) batchSRVs[1] = texSRVs[b.normalTexIndex];
                if (b.roughTexIndex >= 0 && b.roughTexIndex < (int)texSRVs.size()) batchSRVs[2] = texSRVs[b.roughTexIndex];
                if (b.aoTexIndex >= 0 && b.aoTexIndex < (int)texSRVs.size()) batchSRVs[3] = texSRVs[b.aoTexIndex];
                if (b.metalTexIndex >= 0 && b.metalTexIndex < (int)texSRVs.size()) batchSRVs[4] = texSRVs[b.metalTexIndex];
                context->PSSetShaderResources(0, 5, batchSRVs);
                context->Draw(b.vertexCount, b.firstVertex);
            }
            
            // Mirror Resolve
            ID3D11RenderTargetView *nullRTVsM[4] = { nullptr };
            context->OMSetRenderTargets(4, nullRTVsM, nullptr);
            float clearBlackM[4] = { 0.02f, 0.02f, 0.04f, 1.0f };
            context->ClearRenderTargetView(mirrorTexRTVs[mIdx], clearBlackM);
            context->OMSetDepthStencilState(dsOffState, 0);
            context->OMSetRenderTargets(1, &mirrorTexRTVs[mIdx], nullptr);
            
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vsQuad, nullptr, 0);
            context->PSSetShader(psResolve, nullptr, 0);
            
            context->PSSetConstantBuffers(0, 1, &frameCB);
            context->PSSetConstantBuffers(2, 1, &lightCB);
            context->PSSetShaderResources(0, 5, gbufSRVs);
            context->PSSetShaderResources(7, 16, shadowCubeSRVs);
            context->PSSetShaderResources(23, 16, shadowSpotSRVs);
            ID3D11SamplerState* samplers[2] = { sampler, shadowSampler };
            context->PSSetSamplers(0, 2, samplers);
            
            context->Draw(3, 0);
            
            ID3D11ShaderResourceView *nullSRVs[39] = { nullptr };
            context->PSSetShaderResources(0, 39, nullSRVs);
        }
        
        // Restore Main Frame Uniforms
        fu.mirrorCount = nMirrors;
        context->UpdateSubresource(frameCB, 0, nullptr, &fu, 0, 0);
        context->VSSetConstantBuffers(0, 1, &frameCB);
        context->PSSetConstantBuffers(0, 1, &frameCB);
        // --- END MIRROR PASS ---

        // --- Pass 2: Lighting / Resolve ---
        // 1. Unbind G-Buffer RTVs & DSV to resolve D3D11 resource hazards
        ID3D11RenderTargetView *nullRTVs[4] = { nullptr, nullptr, nullptr, nullptr };
        context->OMSetRenderTargets(4, nullRTVs, nullptr);

        // 2. Clear & Bind target (swapchain backbuffer or resolveRTV)
        ID3D11RenderTargetView *targetRTV = swapRTV ? swapRTV : resolveRTV;
        float clearBlack[4] = { 0.02f, 0.02f, 0.04f, 1.0f };
        context->ClearRenderTargetView(targetRTV, clearBlack);
        context->OMSetDepthStencilState(dsOffState, 0);
        context->OMSetRenderTargets(1, &targetRTV, nullptr);

        // 3. Bind G-Buffer SRVs to Pixel Shader
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        context->VSSetShader(vsQuad, nullptr, 0);
        context->PSSetShader(psResolve, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &frameCB);
        context->PSSetConstantBuffers(2, 1, &lightCB);
        context->PSSetShaderResources(0, 5, gbufSRVs);
        context->PSSetShaderResources(6, 1, &mirrorTexSRV);
        context->PSSetShaderResources(7, 16, shadowCubeSRVs);
        context->PSSetShaderResources(23, 16, shadowSpotSRVs);
        ID3D11SamplerState* samplersMain[2] = { sampler, shadowSampler };
        context->PSSetSamplers(0, 2, samplersMain);

        context->Draw(3, 0);

        // 4. Unbind SRVs & RTV after draw
        ID3D11ShaderResourceView *nullSRVs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
        context->PSSetShaderResources(0, 5, nullSRVs);
        context->OMSetRenderTargets(1, nullRTVs, nullptr);

        if (q2) context->End(q2);
        if (qDisjoint) context->End(qDisjoint);
    };

    if (opt.interactive && sdlWindow) {
        std::fprintf(stderr, "[GPUBENCH] Interactive window running (%dx%d).\n"
                             "[GPUBENCH] Controls: WASD/Arrows/Mouse to move, TAB to toggle spline, SPACE pause, ESC quit.\n", W, H);
        FreeCamInit(scene);
        bool freeFly = false, paused = false, mouseLook = false, running = true;
        float demoT = scene.resolvedDemoT;
        uint64_t prevTick = SDL_GetPerformanceCounter();
        const double freq = double(SDL_GetPerformanceFrequency());

        LoadOptions lo;

        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) running = false;
                else if (ev.type == SDL_KEYDOWN) {
                    switch (ev.key.keysym.sym) {
                        case SDLK_ESCAPE: case SDLK_BACKSPACE: running = false; break;
                        case SDLK_TAB: freeFly = !freeFly; break;
                        case SDLK_SPACE: paused = !paused; break;
                        case SDLK_LEFTBRACKET: demoT -= 100.0f; break;
                        case SDLK_RIGHTBRACKET: demoT += 100.0f; break;
                        default: break;
                    }
                } else if (ev.type == SDL_MOUSEBUTTONDOWN) mouseLook = true;
                else if (ev.type == SDL_MOUSEBUTTONUP)   mouseLook = false;
                else if (ev.type == SDL_MOUSEMOTION && mouseLook) {
                    if (!freeFly) { FreeCamSyncFromScene(scene); freeFly = true; }
                    FreeCamMouseLook(scene, -float(ev.motion.xrel) * 0.004f,
                                            -float(ev.motion.yrel) * 0.004f);
                }
            }

            const uint64_t now = SDL_GetPerformanceCounter();
            const float dt = float(double(now - prevTick) / freq);
            prevTick = now;

            if (!paused) demoT += 100.0f * dt;

            Reanimate(scene, lo, demoT);

            if (freeFly) {
                const uint8_t *k = SDL_GetKeyboardState(nullptr);
                FreeCamInput in;
                in.fwd   = k[SDL_SCANCODE_W];
                in.back  = k[SDL_SCANCODE_S] || k[SDL_SCANCODE_Z];
                in.left  = k[SDL_SCANCODE_A] || k[SDL_SCANCODE_END];
                in.right = k[SDL_SCANCODE_D] || k[SDL_SCANCODE_PAGEDOWN];
                in.up    = k[SDL_SCANCODE_Q] || k[SDL_SCANCODE_KP_PLUS];
                in.down  = k[SDL_SCANCODE_E] || k[SDL_SCANCODE_KP_MINUS];
                in.yawLeft   = k[SDL_SCANCODE_LEFT];
                in.yawRight  = k[SDL_SCANCODE_RIGHT];
                in.pitchUp   = k[SDL_SCANCODE_UP];
                in.pitchDown = k[SDL_SCANCODE_DOWN];
                in.rollLeft  = k[SDL_SCANCODE_HOME];
                in.rollRight = k[SDL_SCANCODE_PAGEUP];
                in.slower    = k[SDL_SCANCODE_COMMA];
                in.faster    = k[SDL_SCANCODE_PERIOD];
                in.rotSlower = k[SDL_SCANCODE_K];
                in.rotFaster = k[SDL_SCANCODE_L];
                FreeCamStep(scene, in, dt);
            } else {
                FreeCamSyncFromScene(scene);
            }

            // Refresh LightUniforms
            LightUniformsWin lu = {};
            size_t numActiveLights = 0;
            for (size_t i = 0; i < scene.lights.size() && numActiveLights < 64; ++i) {
                const auto &L = scene.lights[i];
                if (!std::isfinite(L.pos[0]) || L.range <= 0.0f) continue;
                GpuLightWin &g = lu.lights[numActiveLights++];
                g.pos[0] = L.pos[0];
                g.pos[1] = L.pos[1];
                g.pos[2] = L.pos[2];
                g.pos[3] = L.range;

                float c01[3] = { L.color[0] / 255.0f, L.color[1] / 255.0f, L.color[2] / 255.0f };
                g.color[0] = c01[0] * L.intensity;
                g.color[1] = c01[1] * L.intensity;
                g.color[2] = c01[2] * L.intensity;
                g.color[3] = 1.0f / std::max(L.range, 1e-4f);

                g.dir[0] = L.dir[0];
                g.dir[1] = L.dir[1];
                g.dir[2] = L.dir[2];
                g.dir[3] = L.isSpot ? 1.0f : 0.0f;

                g.params[0] = L.cosInner;
                g.params[1] = L.cosOuter;
                g.params[2] = L.shadowIndex;
                g.sRow0[0] = L.shadowRot[0][0]; g.sRow0[1] = L.shadowRot[0][1]; g.sRow0[2] = L.shadowRot[0][2];
                g.sRow1[0] = L.shadowRot[1][0]; g.sRow1[1] = L.shadowRot[1][1]; g.sRow1[2] = L.shadowRot[1][2];
                g.sRow2[0] = L.shadowRot[2][0]; g.sRow2[1] = L.shadowRot[2][1]; g.sRow2[2] = L.shadowRot[2][2];
                g.sRow0[3] = 1.0f / std::max(L.shadowTanHalfFov, 1e-4f);
                g.sRow1[3] = 0.0f;
                g.sRow2[3] = 0.0f;
                g.shadowNear = L.shadowNear;
                g.shadowFar = L.shadowFar;
                g.params[3] = 0.0f;
            }
            fu.numLights = (UINT)numActiveLights;

            // Refresh FrameUniforms
            for (int c = 0; c < 3; ++c) {
                fu.camRow0[c] = scene.camera.rot[0][c];
                fu.camRow1[c] = scene.camera.rot[1][c];
                fu.camRow2[c] = scene.camera.rot[2][c];
                fu.camSrc[c]  = scene.camera.src[c];
            }
            fu.sx = 2.0f * scene.camera.perspX / W;
            fu.ox = 2.0f * scene.camera.cntrEX / W - 1.0f;
            fu.sy = 2.0f * scene.camera.perspY / H;
            fu.oy = 1.0f - 2.0f * scene.camera.cntrEY / H;
            fu.exposure = opt.exposure;

            const float n = scene.camera.nearZ, f = scene.camera.farZ;
            fu.dza = -n / (f - n);
            fu.dzb = n * f / (f - n);
            fu.invSx = 1.0f / fu.sx;
            fu.invSy = 1.0f / fu.sy;
            fu.nearZ = n;
            fu.farZ = f;

            fu.lightRangeScale = opt.lightRangeScale;
            fu.diffuseFactor   = 1.0f;
            fu.specularFactor  = 1.0f;
            fu.ambientFactor   = 0.25f;
            fu.vizLight        = opt.vizLight;

            fu.flatAmbient[0] = scene.ambient[0] * (1.0f / 255.0f);
            fu.flatAmbient[1] = scene.ambient[1] * (1.0f / 255.0f);
            fu.flatAmbient[2] = scene.ambient[2] * (1.0f / 255.0f);
            fu.flatAmbient[3] = scene.shAmbient ? 0.0f : 1.0f;

            fu.hdrMode[0] = opt.hdrLinear ? 1.0f : 0.0f;
            fu.envReflGain = 1.0f;

            for (int c = 0; c < 3; ++c) {
                fu.aabbMin[c] = scene.aabbMin[c];
                fu.aabbMax[c] = scene.aabbMax[c];
            }
            for (size_t i = 0; i < scene.envProbes.size() && i < 8; ++i)
                for (int c = 0; c < 3; ++c) fu.envProbePos[i][c] = scene.envProbes[i].pos[c];
            context->UpdateSubresource(frameCB, 0, nullptr, &fu, 0, 0);
            context->UpdateSubresource(lightCB, 0, nullptr, &lu, 0, 0);

            // Refresh BatchUniforms
            if (bus.size() < scene.batches.size()) bus.resize(scene.batches.size());
            for (size_t i = 0; i < scene.batches.size(); ++i) {
                const auto &b = scene.batches[i];
                BatchUniforms &u = bus[i];
                for (int c = 0; c < 3; ++c) {
                    u.rotRow0[c] = b.rot[0][c];
                    u.rotRow1[c] = b.rot[1][c];
                    u.rotRow2[c] = b.rot[2][c];
                    u.objPos[c]   = b.pos[c];
                    u.baseColor[c]= b.baseColor[c];
                }
                u.baseColor[3] = (b.textureIndex >= 0) ? 1.0f : 0.0f;

                u.matParams[0] = b.diffuse;
                u.matParams[1] = b.specular;
                u.matParams[2] = std::sqrt(2.0f / (float(b.glossiness) + 2.0f));
                u.matParams[3] = b.luminosity;

                u.mapFlags[0] = (b.normalTexIndex >= 0) ? 1.0f : 0.0f;
                u.mapFlags[1] = (b.roughTexIndex >= 0) ? 1.0f : 0.0f;
                u.mapFlags[2] = b.aoInAlpha ? 1.0f : 0.0f;
                u.mapFlags[3] = b.parallaxScale;

                u.misc[0] = 0.0f;
                u.misc[1] = (b.aoTexIndex >= 0) ? 1.0f : 0.0f;
                u.misc[2] = (b.metalTexIndex >= 0) ? 1.0f : 0.0f;
                u.misc[3] = 2.0f * b.aoStrength;

                u.misc2[0] = float(b.envProbe);
                u.misc2[1] = b.reflection;
                u.misc2[2] = b.xparBlendAlpha;
                u.misc2[3] = 0.5f;

                u.xpar[0] = b.luminosity;
                u.xpar[1] = b.specular;
                u.xpar[2] = float(b.glossiness);
                u.xpar[3] = b.additive ? 1.0f : 0.0f;
            }

            renderDeferredFrame(nullptr, nullptr, nullptr);
            if (swapChain) swapChain->Present(1, 0);
        }
    } else {
        std::fprintf(stderr, "[GPUBENCH] warmup %d frames…\n", opt.warmup);
        for (int i = 0; i < opt.warmup; ++i) {
            renderDeferredFrame(nullptr, nullptr, nullptr);
        }

        std::vector<double> gpuMs;
        gpuMs.reserve(size_t(opt.iters));
        for (int i = 0; i < opt.iters; ++i) {
            renderDeferredFrame(tsStart, tsEnd, disjointQuery);

            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData;
            while (context->GetData(disjointQuery, &disjointData, sizeof(disjointData), 0) == S_FALSE) {}

            UINT64 t1 = 0, t2 = 0;
            while (context->GetData(tsStart, &t1, sizeof(t1), 0) == S_FALSE) {}
            while (context->GetData(tsEnd, &t2, sizeof(t2), 0) == S_FALSE) {}

            if (!disjointData.Disjoint && disjointData.Frequency > 0) {
                double ms = double(t2 - t1) * 1000.0 / double(disjointData.Frequency);
                gpuMs.push_back(ms);
            }
        }

        if (!gpuMs.empty()) {
            const double med = Percentile(gpuMs, 0.50);
            std::fprintf(stderr,
                "\n[GPUBENCH] ===== RESULT =====\n"
                "[GPUBENCH] scene=%s pose t=%d %dx%d Direct3D11 (Deferred)\n"
                "[GPUBENCH] draws=%zu tris=%u gpuVerts=%zu textures=%u\n"
                "[GPUBENCH] GPU frame ms over %zu frames (after %d warmup):\n"
                "[GPUBENCH]   median %.4f   p5 %.4f   p95 %.4f   min %.4f   max %.4f\n"
                "[GPUBENCH]   => %.1f FPS equivalent\n",
                opt.outPath.c_str(), opt.iters, scene.xres, scene.yres,
                scene.batches.size(), scene.faceCount, scene.verts.size(), scene.texturesLoaded,
                gpuMs.size(), opt.warmup,
                med, Percentile(gpuMs, 0.05), Percentile(gpuMs, 0.95),
                *std::min_element(gpuMs.begin(), gpuMs.end()),
                *std::max_element(gpuMs.begin(), gpuMs.end()),
                med > 0.0 ? 1000.0 / med : 0.0);
        }

        if (!opt.outPath.empty()) {
            context->CopyResource(stagingTex, resolveTex);
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
                if (WritePPM(opt.outPath.c_str(), (const uint8_t*)mapped.pData, W, H, mapped.RowPitch))
                    std::fprintf(stderr, "[GPUBENCH] wrote %s\n", opt.outPath.c_str());
                context->Unmap(stagingTex, 0);
            }
        }
    }

    // Cleanup
    for (auto srv : gbufSRVs) if (srv) srv->Release();
    for (auto rtv : gbufRTVs) if (rtv) rtv->Release();
    if (gbufAlbedo) gbufAlbedo->Release();
    if (gbufNormal) gbufNormal->Release();
    if (gbufMaterial) gbufMaterial->Release();
    if (depthDSV) depthDSV->Release();
    if (depthTex) depthTex->Release();
    if (resolveRTV) resolveRTV->Release();
    if (resolveTex) resolveTex->Release();
    if (stagingTex) stagingTex->Release();
    if (swapRTV) swapRTV->Release();
    if (swapChain) swapChain->Release();
    if (sdlWindow) SDL_DestroyWindow(sdlWindow);

    if (vsShadow) vsShadow->Release();
    if (layoutShadow) layoutShadow->Release();
    if (shadowCB) shadowCB->Release();
    if (shadowSampler) shadowSampler->Release();
    for (int i=0; i<16; ++i) {
        if(shadowSpotTex[i]) shadowSpotTex[i]->Release();
        if(shadowSpotDSVs[i]) shadowSpotDSVs[i]->Release();
        if(shadowSpotSRVs[i]) shadowSpotSRVs[i]->Release();
        if(shadowCubeTex[i]) shadowCubeTex[i]->Release();
        for(int f=0; f<6; ++f) if(shadowCubeDSVs[i][f]) shadowCubeDSVs[i][f]->Release();
        if(shadowCubeSRVs[i]) shadowCubeSRVs[i]->Release();
    }
    if (mirrorTex) mirrorTex->Release();
    if (mirrorTexSRV) mirrorTexSRV->Release();
    for (int i = 0; i < kMaxMirrors; ++i) {
        if (mirrorTexRTVs[i]) mirrorTexRTVs[i]->Release();
    }
    for (auto srv : texSRVs) if (srv) srv->Release();
    if (sampler) sampler->Release();
    if (batchCB) batchCB->Release();
    if (frameCB) frameCB->Release();
    if (vb) vb->Release();
    if (layout) layout->Release();
    if (psResolve) psResolve->Release();
    if (vsQuad) vsQuad->Release();
    if (psGbuf) psGbuf->Release();
    if (vsGbuf) vsGbuf->Release();
    if (disjointQuery) disjointQuery->Release();
    if (tsStart) tsStart->Release();
    if (tsEnd) tsEnd->Release();
    context->Release();
    device->Release();

    return true;
}

}  // namespace gpubench
