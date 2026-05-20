#include "SkyCube.h"
#include "Base/Object.h"
#include "Base/TriMesh.h"
#include "Base/Scene.h"
#include "Base/FeatureFlags.h"

#include <vector>

// Linear (row-major, ARGB8888) copies of the 6 sky-cube face textures,
// captured in InitSkyCube *before* Sachletz tile-shuffles them. The
// deferred-skybox pass samples from these directly. ~6 × 1024² × 4
// bytes ≈ 24 MB worst case; smaller for lower-res sky textures.
namespace {
    struct SkyboxFaceMip {
        int width = 0, height = 0;
        std::vector<dword> data;   // row-major ARGB8888
    };
    struct SkyboxFace {
        std::vector<SkyboxFaceMip> mips;  // mips[0] = full res, halves each step to 1×1
    };
    SkyboxFace s_skyboxFaces[6];

    // 2× box-filter downsample (4 ARGB texels → 1 averaged texel).
    // Per-channel averaging on uint8 components.
    void boxDownsample(const dword *src, int sw, int sh,
                       std::vector<dword> &dst, int &dw, int &dh)
    {
        dw = std::max(1, sw / 2);
        dh = std::max(1, sh / 2);
        dst.assign(size_t(dw) * size_t(dh), 0);
        for (int y = 0; y < dh; ++y) {
            const int sy0 = std::min(sh - 1, y * 2);
            const int sy1 = std::min(sh - 1, sy0 + 1);
            const dword *r0 = src + size_t(sy0) * size_t(sw);
            const dword *r1 = src + size_t(sy1) * size_t(sw);
            dword *out = dst.data() + size_t(y) * size_t(dw);
            for (int x = 0; x < dw; ++x) {
                const int sx0 = std::min(sw - 1, x * 2);
                const int sx1 = std::min(sw - 1, sx0 + 1);
                const dword a = r0[sx0], b = r0[sx1], c = r1[sx0], d = r1[sx1];
                const unsigned aB = (a      ) & 0xFFu, aG = (a >> 8) & 0xFFu,
                               aR = (a >> 16) & 0xFFu, aA = (a >> 24) & 0xFFu;
                const unsigned bB = (b      ) & 0xFFu, bG = (b >> 8) & 0xFFu,
                               bR = (b >> 16) & 0xFFu, bA = (b >> 24) & 0xFFu;
                const unsigned cB = (c      ) & 0xFFu, cG = (c >> 8) & 0xFFu,
                               cR = (c >> 16) & 0xFFu, cA = (c >> 24) & 0xFFu;
                const unsigned dB = (d      ) & 0xFFu, dG = (d >> 8) & 0xFFu,
                               dR = (d >> 16) & 0xFFu, dA = (d >> 24) & 0xFFu;
                const unsigned avgB = (aB + bB + cB + dB) >> 2;
                const unsigned avgG = (aG + bG + cG + dG) >> 2;
                const unsigned avgR = (aR + bR + cR + dR) >> 2;
                const unsigned avgA = (aA + bA + cA + dA) >> 2;
                out[x] = (avgA << 24) | (avgR << 16) | (avgG << 8) | avgB;
            }
        }
    }

    void buildMipPyramid(SkyboxFace &face) {
        if (face.mips.empty()) return;
        while (face.mips.back().width > 1 || face.mips.back().height > 1) {
            const SkyboxFaceMip &src = face.mips.back();
            SkyboxFaceMip dst;
            boxDownsample(src.data.data(), src.width, src.height,
                          dst.data, dst.width, dst.height);
            face.mips.push_back(std::move(dst));
        }
    }
}

const dword *SkyCube_GetFaceMip(int face, int mip, int &outW, int &outH)
{
    if (face < 0 || face >= 6) { outW = outH = 0; return nullptr; }
    const SkyboxFace &f = s_skyboxFaces[face];
    if (mip < 0 || size_t(mip) >= f.mips.size()) { outW = outH = 0; return nullptr; }
    const SkyboxFaceMip &m = f.mips[mip];
    outW = m.width;
    outH = m.height;
    return m.data.empty() ? nullptr : m.data.data();
}

int SkyCube_NumMips()
{
    return int(s_skyboxFaces[0].mips.size());
}

static void GenerateSkyTexture(Texture *Tx, int32_t numStars)
{
/*	Vector V;

	dword *Txd = (dword *)Tx->Data;

	const float K = 0.01;
	float y =-PI/4.0f;
	dword i, size;

	// f'(x) = 1 / cos f(x).

	for(i=0;y<PI/4.0f;i++)
	{
		y += K / cos(y);
	}

	size = i;
	float *F = new float [size+1];
	y =-PI/4.0f;
	for(i=0;i<=size; i++)
	{
		F[i] = y;
		y += K / cos(y);
	}

	while (numStars--)
	{
		float x = RAND_15() * (size-1) / 32768.0;

		int ix = x;
		float fx = x-ix;
		float beta = F[ix] * (1.0-fx) + F[ix+1] * fx;

		Vector V;
		V.x = 1.0;
		V.y = tan(beta);
		V.z = (RAND_15()-16384) / 16384.0;

		// texture is placed at X=1 plane
		if (fabs(V.y) >= V.x || fabs(V.z) >= V.x)
		{
			int kaka = 1;
		}
		V.y /= V.x;
		V.z /= V.x;

		int32_t TX = V.z * 127.99 + 128.0;
		int32_t TY = V.y * 127.99 + 128.0; 
		Txd[TX + TY*256] = 0x00FFFFFF;

	}

	delete [] F;
	int banana = 1;*/
	Image Im;
	New_Image(&Im, 256, 256);
	memset(Im.Data, 0, 256*256*sizeof(dword));
	Generate_Plasma(&Im, 1000, 3851);
	Convert_Image2Texture(&Im, Tx);
}

Scene * CreateSkyCube(dword skyType)
{
	Scene *Sc = (Scene *)getAlignedBlock(sizeof(Scene), 16);
	memset(Sc, 0, sizeof(Scene));
	Sc->NZP = 20.0;
	Sc->FZP = 200.0;

	TriMesh *T = (TriMesh *)getAlignedBlock(sizeof(TriMesh), 16);
	memset(T,0,sizeof(TriMesh));

	Object *Obj = new Object;
	memset(Obj, 0, sizeof(Object));
	Obj->Name = strdup("Skycube");

	Obj->Next = Sc->ObjectHead;
	if (Sc->ObjectHead) Sc->ObjectHead->Prev = Obj;
	Sc->ObjectHead = Obj;

	Obj->Data = T;
	Obj->Type = Obj_TriMesh;

	T->Next = Sc->TriMeshHead;
	if (Sc->TriMeshHead) Sc->TriMeshHead->Prev = T;
	Sc->TriMeshHead = T;

	// set rotation/position ptrs
	Obj->Rot =&T->RotMat;
	Obj->Pos =&T->IPos;
	Vector_Form(&Obj->Pivot, 0, 0, 0);

	T->VIndex = 24;
	T->Verts = new Vertex[T->VIndex];
	memset(T->Verts, 0, sizeof(Vertex) * T->VIndex);

	T->FIndex = 12;
	T->Faces = new Face [T->FIndex];
	memset(T->Faces, 0, sizeof(Face) * T->FIndex);

	// the normals to each of the cube's faces
	Vector N[6] = {
		Vector( 0.0f, 0.0f, 1.0f),
		Vector(-1.0f, 0.0f, 0.0f),
		Vector( 0.0f, 0.0f,-1.0f),
		Vector( 1.0f, 0.0f, 0.0f),
		Vector( 0.0f, 1.0f, 0.0f),
		Vector( 0.0f,-1.0f, 0.0f)
	};

	// Vertex positions
	Vector P[8] = {
		Vector(-1.0f,-1.0f,-1.0f),
		Vector( 1.0f,-1.0f,-1.0f),
		Vector(-1.0f, 1.0f,-1.0f),
		Vector( 1.0f, 1.0f,-1.0f),
		Vector(-1.0f,-1.0f, 1.0f),
		Vector( 1.0f,-1.0f, 1.0f),
		Vector(-1.0f, 1.0f, 1.0f),
		Vector( 1.0f, 1.0f, 1.0f),
	};

	dword Order[4*6] = {
		7,6,4,5,		
		3,7,5,1,
		2,3,1,0,
		6,2,0,4,
		0,1,5,4,
		6,7,3,2
	};

	Vertex *SV = T->Verts;
	dword *O = Order,o;
	dword i;
	for(i=0;i<6;i++)
	{
		o = *O++;
		SV->LR = SV->LG = SV->LB = 255.0;
		SV->Pos.x = P[o].x;
		SV->Pos.y = P[o].y;
		SV->Pos.z = P[o].z;
		SV->U = 1.0f/1024.0f;
		SV->V = 1.0f/1024.0f;
		SV->i = o + i * 16;
		SV++;
		o = *O++;
		SV->LR = SV->LG = SV->LB = 255.0;
		SV->Pos.x = P[o].x;
		SV->Pos.y = P[o].y;
		SV->Pos.z = P[o].z;
		SV->U = 1023.0f/ 1024.0f;
		SV->V = 1.0f/ 1024.0f;
		SV->i = o + i * 16;
		SV++;
		o = *O++;
		SV->LR = SV->LG = SV->LB = 255.0;
		SV->Pos.x = P[o].x;
		SV->Pos.y = P[o].y;
		SV->Pos.z = P[o].z;
		SV->U = 1023.0f / 1024.0f;
		SV->V = 1023.0f/ 1024.0f;
		SV->i = o + i*16;
		SV++;
		o = *O++;
		SV->LR = SV->LG = SV->LB = 255.0;
		SV->Pos.x = P[o].x;
		SV->Pos.y = P[o].y;
		SV->Pos.z = P[o].z;
		SV->U = 1.0f / 1024.0f;
		SV->V = 1023.0f / 1024.0f;
		SV->i = o + i * 16;
		SV++;
	}

	Material *M[6];
	Texture *Tx[6];

	const char* names[6] = { "TEXTURES/SBBK.JPG", "TEXTURES/SBRT.JPG", "TEXTURES/SBFT.JPG", "TEXTURES/SBLF.JPG", "TEXTURES/SBDN.JPG", "TEXTURES/SBUP.JPG" };

	//DWord* TempBuf = new DWord[65536];
	for (i = 0; i < 6; i++)
	{
		M[i] = getAlignedType<Material>(16); //(Material*)getAlignedBlock(sizeof(Material), 16);
		//memset(M[i], 0, sizeof(Material));

		Tx[i] = new Texture;
		//memset(Tx[i], 0, sizeof(Texture));

		Tx[i]->FileName = strdup(names[i]);
		Load_Texture(Tx[i]);

		M[i]->Flags = Mat_TwoSided | Mat_RGBInterp;
		M[i]->Txtr = Tx[i];

		//		Tx[i]->BPP = 32;
		//		dword *data = new dword [256*256];
		//		Tx[i]->Data = (byte *)data;
		//		memset(Tx[i]->Data, 0, 256*256*4);
		Tx[i]->Flags |= Txtr_Nomip | Txtr_Tiled;
		Tx[i]->Mipmap[0] = (byte*)Tx[i]->Data;
		Tx[i]->numMipmaps = 1;

		// Snapshot the linear/row-major data *before* Sachletz tile-
		// shuffles Tx[i]->Data in place. Deferred-skybox samples from
		// this copy; forward path keeps using the Sachletz layout.
		// Build a box-filtered mip pyramid for free here so the
		// deferred pass can pick a sensible LOD per pixel.
		{
			SkyboxFace &face = s_skyboxFaces[i];
			face.mips.clear();
			SkyboxFaceMip m0;
			m0.width  = Tx[i]->SizeX;
			m0.height = Tx[i]->SizeY;
			const size_t n = size_t(m0.width) * size_t(m0.height);
			m0.data.assign((dword *)Tx[i]->Data, (dword *)Tx[i]->Data + n);
			face.mips.push_back(std::move(m0));
			buildMipPyramid(face);
		}
		//for (int y = 0; y < 1024; y++) {
		//	for (int x = 0; x < 1024; x++) {
		//		((DWord *)Tx[i]->Data)[y * 1024 + x] =  ((x^y) & 32) ? 0xffffffff: 0;
		//	}
		//}
	

		Sachletz((DWord *)Tx[i]->Data, Tx[i]->SizeX, Tx[i]->SizeY);
	//	GenerateSkyTexture(Tx[i], 200);

		//memcpy(TempBuf, Tx[i]->Data, 65536 * 4);
		//dword* writePtr = (dword *)Tx[i]->Data;
		//for (dword X = 0; X < 64; X++)
		//	for (dword Y = 0; Y < 64; Y++)
		//	{
		//		dword* blockPtr = TempBuf + ((X + (Y << 8)) << 2);
		//		for (dword y = 0; y < 4; y++)
		//			for (dword x = 0; x < 4; x++)
		//				*writePtr++ = blockPtr[x + (y << 8)];
		//	}

		//Tx[i]->OptClass = 0;
		//Tx[i]->SizeX = 256;
		//Tx[i]->SizeY = 256;
	}

	//delete[]TempBuf;
	T->FIndex = 12;
	Face *F = T->Faces;
	for(i=0; i<6; i++)
	{
		int ii = i;
		F->A = T->Verts + ii*4;
		F->B = T->Verts + ii*4 + 1;
		F->C = T->Verts + ii*4 + 2;
		F->N = N[i];
		F->NormProd = -Dot_Product(&F->A->Pos, &F->N);
		F->Txtr = M[ii];
		F->U1 = F->A->U;
		F->U2 = F->B->U;
		F->U3 = F->C->U;
		F->V1 = F->A->V;
		F->V2 = F->B->V;
		F->V3 = F->C->V;
		F++;

		F->A = T->Verts + ii*4;
		F->B = T->Verts + ii*4 + 2;
		F->C = T->Verts + ii*4 + 3;
		F->N = N[i];
		F->NormProd = -Dot_Product(&F->A->Pos, &F->N);
		F->Txtr = M[ii];
		F->U1 = F->A->U;
		F->U2 = F->B->U;
		F->U3 = F->C->U;
		F->V1 = F->A->V;
		F->V2 = F->B->V;
		F->V3 = F->C->V;
		F++;
	}

	T->Pos.CurKey = 0;
	T->Pos.NumKeys = 1;
	T->Pos.Keys = new SplineKey[1];
	memset(T->Pos.Keys, 0, sizeof(SplineKey));
	Quaternion_Form(&T->Pos.Keys->Pos,0.0,0.0,0.0,0.0f);
	//T->IPos = Vortex_Center;
	T->Scale.CurKey = 0;
	T->Scale.NumKeys = 1;
	T->Scale.Keys = new SplineKey[1];
	Quaternion_Form(&T->Scale.Keys->Pos,100.0f,100.0f,100.0f,0.0f);
	Vector_Form(&T->IScale, 100.0,100.0, 100.0);
	T->Rotate.CurKey = 0;
	T->Rotate.NumKeys = 1;
	T->Rotate.Keys = new SplineKey[1];
	Quaternion_Form(&T->Rotate.Keys->Pos,0.0f,0.0f,0.0f,1.0f);
	Vector_Form(&T->BSphereCtr,0.0f,0.0f,0.0f);
	T->BSphereRad = 10000.0f;

	Matrix_Identity(T->RotMat);

	//Preprocess_Scene(Sc);
	T->Flags = HTrack_Visible;
	Assign_Fillers(Sc);
	return Sc;
}


void RenderSkyCube(Scene *Sc, Camera *Cm, bool SkipCameraAnimation)
{
	// When the deferred skybox pass is on, it paints sky pixels from
	// the G-buffer — skip the overdrawn forward draw entirely. Both
	// running would have the forward draw write Z, then the deferred
	// pass would either overwrite reflective windows (if it checked
	// mat32) or skip everything (if it checked zEnc).
	if (fds::FeatureFlags::deferred_skybox()) return;
	Scene *PrevCurScene = CurScene;
	Camera *PrevView = View;
	View = Cm;
	SetCurrentScene(Sc);

	Animate_Objects(Sc, SkipCameraAnimation);

	Vector PrevViewPos = View->ISource;
	Vector_Zero(&View->ISource);

	Transform_Objects(Sc, fds::g_mainCamera, fds::g_mainFaces);
	if (CAll)
	{
		Radix_Sort(FList,SList,CAll);
		// Force forward render for the sky cube — the cube faces are
		// authored with vertex-stored LR/LG/LB=255 (CreateSkyCube), which
		// is what the forward fillers consume to pass texels through at
		// near-full brightness. Deferred lighting would re-shade against
		// Mat->Diffuse / Sc->Ambient (both effectively zero on sky-cube
		// materials) and produce a black backdrop.
		Render(RenderPath::ForceForward);
		FastWrite((byte *)ZPage16, 0, (XRes * YRes * sizeof(word)) >> 2);
	}
	View->ISource = PrevViewPos;

	View = PrevView;
	SetCurrentScene(PrevCurScene);
}
