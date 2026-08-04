// TESTMAP.CPP — deterministic mapper/clipper reproduction harness.
//
// Built only under -dFDS_MAPTEST. REV.CPP calls Run_MapTest() right after FDS init
// (before the demo) and exits. It feeds CONTROLLED triangles through the SAME clipper +
// ASM texture mapper the demo uses and validates the output against analytic expectation,
// so "the mapper produces bad results" becomes a numeric pass/fail instead of an eyeball.
//
// Texture is coordinate-encoding: texel(u,v) = R:u  G:v  B:0x80. A pixel that samples
// texel(u,v) therefore SHOWS (u,v,128) — any mis-map is both visible (PPM) and checkable.
//
// The discriminator is TEST A1: a fully on-screen, constant-depth (affine) triangle that
// needs NO clipping, called straight into the filler. If A1 fails, the ASM MAPPER is wrong.
// If A1 passes but the clip tests (B*/C*/D*) fail, the CLIPPER (or near-clip interp) is wrong.

#include <StdIO.H>
#include <StdLib.H>
#include <String.H>
#include <Math.H>

#include "FDS_Defs.H"
#include "FDS_Vars.H"
#include "FDS_Decs.H"

#ifdef FDS_MAPTEST

static Material MT_Mat;
static Material MT_MatG;   // fused texture+gouraud material (The_BiGMMXTrue path)
static Material MT_MatX;   // transparent material (The_BiTTrue / 50-50 blend, test-no-write z)
static Texture  MT_Tex;
static Texture  MT_TexW;   // solid-white texture for the transparent quad (blend is visible)
static DWord   *MT_Til = 0;
static DWord   *MT_TilW = 0;

// ---- coordinate-encoding swizzled texture: texel(u,v) = 0x00 u v 80 (R=u,G=v,B=0x80) ----
static void MT_BuildTexture(void)
{
	long u,v;
	MT_Til = (DWord *)new char[262144];
	for(v=0;v<256;v++) for(u=0;u<256;u++)
		MT_Til[((u>>3)<<11)|(v<<3)|(u&7)] = ((DWord)u<<16)|((DWord)v<<8)|0x80;  // swizzled layout
	memset(&MT_Tex,0,sizeof(MT_Tex));
	MT_Tex.BPP = 32;
	MT_Tex.Data = (char *)MT_Til;
	MT_Tex.TiledData = (char *)MT_Til;   // TiledData==Data => TEX_IS_TILED true (tiled fetch)
	memset(&MT_Mat,0,sizeof(MT_Mat));
	MT_Mat.Txtr = &MT_Tex;
	MT_Mat.Flags = 0;                    // opaque, non-gouraud -> The_BiTrue / Frust_Set_UV
	memset(&MT_MatG,0,sizeof(MT_MatG));
	MT_MatG.Txtr = &MT_Tex;
	MT_MatG.Flags = Mat_RGBInterp;       // textured + gouraud -> The_BiGMMXTrue / Frust_Set_UVL
	// solid-white texture for the transparent quad (so the 50/50 blend is visible against
	// the coordinate-textured opaque background).
	MT_TilW = (DWord *)new char[262144];
	{ long i; for(i=0;i<65536;i++) MT_TilW[i]=0x00FFFFFF; }
	memset(&MT_TexW,0,sizeof(MT_TexW));
	MT_TexW.BPP=32; MT_TexW.Data=(char *)MT_TilW; MT_TexW.TiledData=(char *)MT_TilW;
	memset(&MT_MatX,0,sizeof(MT_MatX));
	MT_MatX.Txtr=&MT_TexW; MT_MatX.Flags=Mat_Transparent;   // -> The_BiTTrue / Frust_Set_UV
}

// A vertex whose screen position + perspective attrs are set directly (mapper-only tests).
static void MT_SetVtxScreen(Vertex *V, float px, float py, float rz, float u, float v)
{
	memset(V,0,sizeof(Vertex));
	V->PX = px;  V->PY = py;  V->RZ = rz;
	V->U  = u;   V->V  = v;
	V->RU = u*rz; V->RV = v*rz;          // mapper recovers U = RU/RZ
	V->LR = V->LG = V->LB = 128.0f;
	V->Flags = 0;                         // no Vtx_Visible bits -> filler direct, no clip
}

// A vertex from a VIEW-SPACE position (vx,vy,vz) + UV, projected exactly as Transform_Objects
// does (TPos = perspective-scaled homogeneous, PX = TPos.X/TPos.Z). Used by the clip tests;
// set forceVis so Render()/our dispatch routes it through Universal_Frustrum_Elite.
static void MT_SetVtxView(Vertex *V, float vx, float vy, float vz, float u, float v, int forceVis)
{
	memset(V,0,sizeof(Vertex));
	V->TPos.X = FOVX*vx + CntrEX*vz;      // supermatrix: FOVX*X + CntrEX*Z   (see Transform_Objects)
	V->TPos.Y = CntrEY*vz - FOVY*vy;
	V->TPos.Z = vz;
	V->U = u; V->V = v;
	if (vz>0.001f) {
		V->RZ = 1.0f/vz;
		V->PX = V->TPos.X*V->RZ;
		V->PY = V->TPos.Y*V->RZ;
		V->RU = u*V->RZ; V->RV = v*V->RZ;
	}
	V->LR = V->LG = V->LB = 128.0f;
	V->Flags = forceVis ? Vtx_Visible : 0;
}

static void MT_MakeFace(Face *F, Vertex *A, Vertex *B, Vertex *C)
{
	memset(F,0,sizeof(Face));
	F->A=A; F->B=B; F->C=C;
	F->U1=A->U; F->V1=A->V; F->U2=B->U; F->V2=B->V; F->U3=C->U; F->V3=C->V;
	F->Filler  = &The_BiTrue;             // plain perspective texture (tiled fetch under swizzle)
	F->Clipper = &Frust_Set_UV;
	F->Txtr = &MT_Mat;
	F->N.X=0; F->N.Y=0; F->N.Z=-1; F->NormProd=0;
}

// Fused texture+gouraud face — the filler the mech/buildings actually use (The_BiGMMXTrue,
// TEXTGOUR.ASM). This is the mapper I modified most (ownership mask, first-wins z-write).
static void MT_MakeFaceFused(Face *F, Vertex *A, Vertex *B, Vertex *C)
{
	memset(F,0,sizeof(Face));
	F->A=A; F->B=B; F->C=C;
	F->U1=A->U; F->V1=A->V; F->U2=B->U; F->V2=B->V; F->U3=C->U; F->V3=C->V;
	F->Filler  = &The_BiGMMXTrue;
	F->Clipper = &Frust_Set_UVL;          // carries U/V AND light through the clipper
	F->Txtr = &MT_MatG;
	F->N.X=0; F->N.Y=0; F->N.Z=-1; F->NormProd=0;
}

// Transparent face (The_BiTTrue, 50/50 blend, TEST-NO-WRITE z) — the water/xpar path.
static void MT_MakeFaceXpar(Face *F, Vertex *A, Vertex *B, Vertex *C)
{
	memset(F,0,sizeof(Face));
	F->A=A; F->B=B; F->C=C;
	F->U1=A->U; F->V1=A->V; F->U2=B->U; F->V2=B->V; F->U3=C->U; F->V3=C->V;
	F->Filler  = &The_BiTTrue;
	F->Clipper = &Frust_Set_UV;
	F->Txtr = &MT_MatX;
	F->N.X=0; F->N.Y=0; F->N.Z=-1; F->NormProd=0;
}

// ---- dump VPage to a PPM on C: (read off the runtime image after the run) ----
static void MT_DumpPPM(const char *name)
{
	FILE *pf = fopen(name,"wb");
	unsigned char *px = (unsigned char *)VPage;
	long i,n=(long)XRes*(long)YRes;
	if (!pf) return;
	fprintf(pf,"P6\n%d %d\n255\n",(int)XRes,(int)YRes);
	for(i=0;i<n;i++){ fputc(px[i*4+2],pf); fputc(px[i*4+1],pf); fputc(px[i*4+0],pf); }
	fclose(pf);
}

static void MT_Clear(void)
{
	memset(VPage,0,PageSize);
	if (ZBuf) memset(ZBuf,0,(long)XRes*(long)YRes*2);
}

// covered = non-black pixels; also min/max bbox of coverage
static long MT_Coverage(long *bx0,long *by0,long *bx1,long *by1)
{
	unsigned char *px=(unsigned char *)VPage;
	long x,y,n=0; long x0=99999,y0=99999,x1=-1,y1=-1;
	for(y=0;y<YRes;y++) for(x=0;x<XRes;x++){
		long o=(y*XRes+x)*4;
		if (px[o]||px[o+1]||px[o+2]){ n++;
			if(x<x0)x0=x; if(y<y0)y0=y; if(x>x1)x1=x; if(y>y1)y1=y; }
	}
	if(bx0){*bx0=x0;*by0=y0;*bx1=x1;*by1=y1;}
	return n;
}

// A1 validation: affine quad mapping screen rect [rx0,rx1]x[ry0,ry1] -> UV [0,255]^2.
// For pixels well INSIDE the rect, expected R=u=(x-rx0)/(rx1-rx0)*255, G=v likewise.
// Returns count of pixels whose |dR|>tol or |dG|>tol; *maxerr = worst channel error.
static long MT_ValidateAffine(long rx0,long ry0,long rx1,long ry1,int tol,long *maxerr,long *checked)
{
	unsigned char *px=(unsigned char *)VPage;
	long x,y,bad=0,chk=0,me=0;
	for(y=ry0+3;y<=ry1-3;y++) for(x=rx0+3;x<=rx1-3;x++){
		long o=(y*XRes+x)*4;
		if(!(px[o]||px[o+1]||px[o+2])) continue;         // not covered (edge rounding) - skip
		long eu=(long)((float)(x-rx0)/(float)(rx1-rx0)*255.0f+0.5f);
		long ev=(long)((float)(y-ry0)/(float)(ry1-ry0)*255.0f+0.5f);
		long dr=(long)px[o+2]-eu; if(dr<0)dr=-dr;
		long dg=(long)px[o+1]-ev; if(dg<0)dg=-dg;
		chk++;
		if(dr>me)me=dr; if(dg>me)me=dg;
		if(dr>tol||dg>tol) bad++;
	}
	if(maxerr)*maxerr=me; if(checked)*checked=chk;
	return bad;
}

static void MT_Log(const char *s){ fprintf(LogFile,"%s",s); fflush(LogFile); }

void Run_MapTest(void)
{
	Vertex A,B,C,D;
	Face   F;
	long cov,bx0,by0,bx1,by1,bad,me,chk,i;

	MT_BuildTexture();
	// FList/SList are allocated per-scene by FList_Allocate — but the test runs BEFORE any
	// scene init, so they may be NULL/garbage/aliased (which would corrupt the radix as a
	// TEST artifact). Allocate distinct buffers here so the sort tests are valid.
	fprintf(LogFile,"MAPTEST: pre-alloc FList=%p SList=%p (alias=%s)\n",
	        (void*)FList,(void*)SList,(FList==SList)?"YES-BUG":"no");
	FList = new Face *[2048];
	SList = new Face *[2048];
	fprintf(LogFile,"MAPTEST: XRes=%ld YRes=%ld CntrEX=%.1f FOVX=%.2f FOVY=%.2f ZScaleF=%.0f FList=%p SList=%p\n",
	        (long)XRes,(long)YRes,(double)CntrEX,(double)FOVX,(double)FOVY,(double)ZScaleF,(void*)FList,(void*)SList);
	fflush(LogFile);
	ZScaleF = 49152.0f;   // fixed scale for the tests (deterministic z codes)
	// The far-plane clip reads C_FZP/C_rFZP (per-scene, unset without a scene). Give it a
	// city-like far plane so the far tests exercise the real clip, and match ZLinScale to it.
	{ extern float C_FZP,C_rFZP,C_NZP,C_rNZP; C_FZP = 2500.0f; C_rFZP = 1.0f/2500.0f;
	  C_NZP = 20.0f; C_rNZP = 1.0f/20.0f;
	  ZLinScale = 65280.0f/(2500.0f*1.1f); }

	// ============ A1: MAPPER ONLY — on-screen, constant depth (affine), analytic ============
	// Two triangles forming screen rect [40,280]x[20,220], UV [0,255]^2, RZ=1 (affine).
	MT_Clear();
	MT_SetVtxScreen(&A, 40.0f, 20.0f, 1.0f,   0.0f,   0.0f);   // TL uv(0,0)
	MT_SetVtxScreen(&B,280.0f, 20.0f, 1.0f, 255.0f,   0.0f);   // TR uv(255,0)
	MT_SetVtxScreen(&C,280.0f,220.0f, 1.0f, 255.0f, 255.0f);   // BR uv(255,255)
	MT_SetVtxScreen(&D, 40.0f,220.0f, 1.0f,   0.0f, 255.0f);   // BL uv(0,255)
	MT_MakeFace(&F,&A,&B,&C); DoFace=&F; F.Filler(F.A,F.B,F.C);      // tri 1: TL,TR,BR
	MT_MakeFace(&F,&A,&C,&D); DoFace=&F; F.Filler(F.A,F.B,F.C);      // tri 2: TL,BR,BL
	cov=MT_Coverage(&bx0,&by0,&bx1,&by1);
	bad=MT_ValidateAffine(40,20,280,220,4,&me,&chk);
	fprintf(LogFile,"A1 mapper-affine: cov=%ld bbox=(%ld,%ld)-(%ld,%ld) checked=%ld BAD=%ld maxerr=%ld  %s\n",
	        cov,bx0,by0,bx1,by1,chk,bad,me,(bad==0)?"PASS":"*** FAIL ***");
	fflush(LogFile);
	MT_DumpPPM("MT_A1.PPM");

	// ============ A2: MAPPER — perspective (far edge deeper) ============
	// Same screen rect, but the right edge is 3x farther (RZ 1.0 left, 0.333 right).
	MT_Clear();
	MT_SetVtxScreen(&A, 40.0f, 20.0f, 1.0f,     0.0f,   0.0f);
	MT_SetVtxScreen(&B,280.0f, 20.0f, 0.3333f, 255.0f,   0.0f);
	MT_SetVtxScreen(&C,280.0f,220.0f, 0.3333f, 255.0f, 255.0f);
	MT_SetVtxScreen(&D, 40.0f,220.0f, 1.0f,     0.0f, 255.0f);
	MT_MakeFace(&F,&A,&B,&C); DoFace=&F; F.Filler(F.A,F.B,F.C);
	MT_MakeFace(&F,&A,&C,&D); DoFace=&F; F.Filler(F.A,F.B,F.C);
	cov=MT_Coverage(&bx0,&by0,&bx1,&by1);
	fprintf(LogFile,"A2 mapper-persp: cov=%ld bbox=(%ld,%ld)-(%ld,%ld) (visual: R must rise L->R, no wrap)\n",
	        cov,bx0,by0,bx1,by1);
	fflush(LogFile);
	MT_DumpPPM("MT_A2.PPM");

	// ============ A3: MAPPER — thin oblique sliver (mech-like) ============
	MT_Clear();
	MT_SetVtxScreen(&A, 30.0f, 40.0f, 1.0f,   0.0f,   0.0f);
	MT_SetVtxScreen(&B,300.0f,120.0f, 1.0f, 255.0f, 128.0f);
	MT_SetVtxScreen(&C, 60.0f,130.0f, 1.0f,  20.0f, 255.0f);
	MT_MakeFace(&F,&A,&B,&C); DoFace=&F; F.Filler(F.A,F.B,F.C);
	cov=MT_Coverage(&bx0,&by0,&bx1,&by1);
	fprintf(LogFile,"A3 mapper-sliver: cov=%ld bbox=(%ld,%ld)-(%ld,%ld) (must be ~triangle, no stray px)\n",
	        cov,bx0,by0,bx1,by1);
	fflush(LogFile);
	MT_DumpPPM("MT_A3.PPM");

	// ============ A4: MAPPER — sub-pixel sweep (FLICKER probe) ============
	// Slide a fixed triangle across a pixel boundary in 0.1px steps; coverage must change
	// SMOOTHLY. A jump/dropout at a boundary is exactly the "flickering faces" signature.
	fprintf(LogFile,"A4 mapper-sweep (coverage vs sub-pixel x offset; watch for jumps):\n");
	for(i=0;i<=20;i++){
		float dx=(float)i*0.1f;
		MT_Clear();
		MT_SetVtxScreen(&A, 80.0f+dx, 60.0f, 1.0f,   0.0f,   0.0f);
		MT_SetVtxScreen(&B,200.0f+dx, 70.0f, 1.0f, 255.0f,   0.0f);
		MT_SetVtxScreen(&C,140.0f+dx,180.0f, 1.0f, 128.0f, 255.0f);
		MT_MakeFace(&F,&A,&B,&C); DoFace=&F; F.Filler(F.A,F.B,F.C);
		cov=MT_Coverage(0,0,0,0);
		fprintf(LogFile,"  dx=%.1f cov=%ld\n",(double)dx,cov);
	}
	fflush(LogFile);

	// ============ A5: MAPPER — SUB-TEXEL accuracy (analytic, wrap-aware) ============
	// An affine (constant-RZ) face has an EXACT texture coordinate that is a plane in screen
	// space, so every covered pixel has an analytic expected (u,v) and the error is measurable
	// in texels rather than judged by eye. The triangle sits at FRACTIONAL screen positions
	// (real geometry does) and carries ~4 texels/pixel of gradient, so a one-pixel sampling
	// offset shows up as ~4 texels of error. Two known sources feed this number:
	//   - Bi_Triangle truncates vertex X/Y to int (FILLERS.CPP:659), snapping the triangle to
	//     the pixel grid: the texture is then correct for the SNAPPED triangle, not the real one.
	//   - no X prestep: a span's interpolants are the values at the exact fractional left edge,
	//     but the first pixel drawn is at floor(Left_X), up to a pixel to its left.
	// Swept over sub-pixel offsets so the vertex-snap error is exercised at every phase.
	{
		unsigned char *pxb;
		double sumU=0.0,sumV=0.0,sgnU=0.0,sgnV=0.0,ua,ub,uc,va,vb,vc,det;
		long nAll=0,maxU=0,maxV=0,x,y,o,ei,ej,d1,d2;
		float ax,ay,bx,by,cx,cy;
		const float au=0.0f,av=0.0f,bu=1020.0f,bv=120.0f,cu=60.0f,cv=1000.0f;
		for(i=0;i<=8;i++)
		{
			// i==8 repeats the triangle at EXACTLY INTEGER vertex positions. Bi_Triangle
			// truncates X/Y to int, so that pass has no vertex-snap error at all and whatever
			// error remains belongs to the span setup (missing X prestep + the XWidth-based
			// gradient). Comparing the two isolates the two defects.
			ax=60.5f +(float)i*0.125f; ay=30.25f +(float)i*0.0625f;
			bx=290.75f+(float)i*0.125f; by=90.5f  +(float)i*0.0625f;
			cx=45.25f +(float)i*0.125f; cy=200.75f+(float)i*0.0625f;
			if (i==8) { ax=60.0f; ay=30.0f; bx=290.0f; by=90.0f; cx=45.0f; cy=200.0f; }
			if (i==8) { fprintf(LogFile,"A5 sub-texel (fractional verts): px=%ld meanU=%.3f meanV=%.3f biasU=%+.3f biasV=%+.3f maxU=%ld maxV=%ld\n",
			                    nAll,nAll?sumU/(double)nAll:0.0,nAll?sumV/(double)nAll:0.0,
			                    nAll?sgnU/(double)nAll:0.0,nAll?sgnV/(double)nAll:0.0,maxU,maxV);
			            sumU=sumV=0.0; sgnU=sgnV=0.0; nAll=0; maxU=maxV=0; }
			MT_Clear();
			MT_SetVtxScreen(&A,ax,ay,1.0f,au,av);
			MT_SetVtxScreen(&B,bx,by,1.0f,bu,bv);
			MT_SetVtxScreen(&C,cx,cy,1.0f,cu,cv);
			MT_MakeFace(&F,&A,&B,&C); DoFace=&F; F.Filler(F.A,F.B,F.C);
			// plane through the three FLOAT vertices:  u = ua*x + ub*y + uc
			det=(double)(bx-ax)*(double)(cy-ay)-(double)(cx-ax)*(double)(by-ay);
			if (det==0.0) continue;
			ua=((double)(bu-au)*(double)(cy-ay)-(double)(cu-au)*(double)(by-ay))/det;
			ub=((double)(bx-ax)*(double)(cu-au)-(double)(cx-ax)*(double)(bu-au))/det;
			uc=(double)au-ua*(double)ax-ub*(double)ay;
			va=((double)(bv-av)*(double)(cy-ay)-(double)(cv-av)*(double)(by-ay))/det;
			vb=((double)(bx-ax)*(double)(cv-av)-(double)(cx-ax)*(double)(bv-av))/det;
			vc=(double)av-va*(double)ax-vb*(double)ay;
			pxb=(unsigned char *)VPage;
			for(y=1;y<YRes-1;y++) for(x=1;x<XRes-1;x++)
			{
				o=(y*XRes+x)*4;
				if(!pxb[o]) continue;                       // B=0x80 marks a covered pixel
				if(!pxb[o-4]||!pxb[o+4]||!pxb[o-XRes*4]||!pxb[o+XRes*4]) continue;  // skip edges
				ei=((long)floor(ua*(double)x+ub*(double)y+uc))&255;
				ej=((long)floor(va*(double)x+vb*(double)y+vc))&255;
				d1=((long)pxb[o+2]-ei)&255; if(d1>128)d1=256-d1;   // wrap-aware
				d2=((long)pxb[o+1]-ej)&255; if(d2>128)d2=256-d2;
				sgnU+=(double)(((long)pxb[o+2]-ei+128)&255)-128.0;   // signed, wrap-aware
				sgnV+=(double)(((long)pxb[o+1]-ej+128)&255)-128.0;
				sumU+=(double)d1; sumV+=(double)d2; nAll++;
				if(d1>maxU)maxU=d1; if(d2>maxV)maxV=d2;
			}
		}
		fprintf(LogFile,"A5 sub-texel (INTEGER verts): px=%ld meanU=%.3f meanV=%.3f biasU=%+.3f biasV=%+.3f maxU=%ld maxV=%ld "
		                "(texels; gradient ~4 texel/px, so 1px of sampling error ~= 4)\n",
		        nAll,nAll?sumU/(double)nAll:0.0,nAll?sumV/(double)nAll:0.0,
		        nAll?sgnU/(double)nAll:0.0,nAll?sgnV/(double)nAll:0.0,maxU,maxV);
		fflush(LogFile);
		MT_DumpPPM("MT_A5.PPM");
	}

	// ============ A6: XPAR filler on city-water-shaped input (SUBTEX repro) ============
	// The city water is what faults under -dFDS_SUBTEX. Measured with FDS_XPARVIZ, its faces
	// carry a vertex pair straddling the near plane (PX=PY=0, RZ=0 - the projection is skipped
	// for Z<=0) alongside an off-screen one, plus a fully DEGENERATE face (all three vertices
	// identical). Feed those shapes straight at the transparent filler so the failure repros
	// here in 45s instead of a 4-minute demo run.
	{
		long covA,covB,covC;
		MT_Clear();
		MT_SetVtxScreen(&A,   0.0f,   0.0f, 0.0f,   0.0f,   0.0f);   // near-plane: P(0,0) RZ=0
		MT_SetVtxScreen(&B, 625.65f,-10.79f, 2.5e-5f, 255.0f,  0.0f); // off-screen right/above
		MT_SetVtxScreen(&C, -44.29f,-14.67f, 1.47e-5f, 128.0f,255.0f);
		MT_MakeFaceXpar(&F,&A,&B,&C); DoFace=&F; F.Filler(F.A,F.B,F.C);
		covA=MT_Coverage(0,0,0,0);
		MT_Clear();
		MT_SetVtxScreen(&A, 6.88f, 9.85f, 6.3e-4f, 10.0f, 20.0f);    // fully degenerate face
		MT_SetVtxScreen(&B, 6.88f, 9.85f, 6.3e-4f, 10.0f, 20.0f);
		MT_SetVtxScreen(&C, 6.88f, 9.85f, 6.3e-4f, 10.0f, 20.0f);
		MT_MakeFaceXpar(&F,&A,&B,&C); DoFace=&F; F.Filler(F.A,F.B,F.C);
		covB=MT_Coverage(0,0,0,0);
		MT_Clear();
		MT_SetVtxScreen(&A, 40.5f, 30.25f, 1.0f,   0.0f,   0.0f);    // two vertices sharing a
		MT_SetVtxScreen(&B,200.5f, 30.75f, 1.0f, 255.0f,   0.0f);    // scanline but not a Y
		MT_SetVtxScreen(&C,120.5f,120.5f,  1.0f, 128.0f, 255.0f);
		MT_MakeFaceXpar(&F,&A,&B,&C); DoFace=&F; F.Filler(F.A,F.B,F.C);
		covC=MT_Coverage(0,0,0,0);
		fprintf(LogFile,"A6 xpar water-shapes: nearstraddle=%ld degenerate=%ld sameScanline=%ld "
		                "(reached here = no fault/hang in the transparent filler)\n",covA,covB,covC);
		fflush(LogFile);
	}

	// The view-space clip tests project through FOVX/FOVY, which are 0 until Animate_Objects
	// runs CalcPersp. Set a realistic focal length so triangles land on-screen with real
	// width (any positive value works; the clipper interpolates the projected PX/PY).
	FOVX = 250.0f; FOVY = 250.0f;
	fprintf(LogFile,"MAPTEST: clip tests use FOVX=FOVY=%.0f\n",(double)FOVX); fflush(LogFile);

	// ============ B0: CLIPPER PASS-THROUGH control — all in front, all on-screen ============
	// Same triangle rendered (a) direct filler, (b) forced through UFE. UFE should NOT clip
	// it (fully in front + on-screen) so both must produce the SAME coverage. A mismatch
	// means the clipper corrupts even non-clipped faces (isolates clipper vs near-interp).
	{
		long covDirect, covClip;
		MT_Clear();
		MT_SetVtxView(&A, -0.3f,  0.3f, 1.5f,   0.0f,   0.0f, 0);   // forceVis=0: direct
		MT_SetVtxView(&B,  0.4f,  0.3f, 1.5f, 255.0f,   0.0f, 0);
		MT_SetVtxView(&C,  0.0f, -0.4f, 1.5f, 128.0f, 255.0f, 0);
		MT_MakeFace(&F,&A,&B,&C); DoFace=&F; F.Filler(F.A,F.B,F.C);
		covDirect=MT_Coverage(&bx0,&by0,&bx1,&by1);
		MT_DumpPPM("MT_B0d.PPM");
		MT_Clear();
		MT_SetVtxView(&A, -0.3f,  0.3f, 1.5f,   0.0f,   0.0f, 1);   // forceVis=1: through UFE
		MT_SetVtxView(&B,  0.4f,  0.3f, 1.5f, 255.0f,   0.0f, 1);
		MT_SetVtxView(&C,  0.0f, -0.4f, 1.5f, 128.0f, 255.0f, 1);
		MT_MakeFace(&F,&A,&B,&C); DoFace=&F; Universal_Frustrum_Elite();
		covClip=MT_Coverage(0,0,0,0);
		MT_DumpPPM("MT_B0c.PPM");
		fprintf(LogFile,"B0 clip-passthrough: direct=%ld clip=%ld  %s\n",
		        covDirect,covClip,(labs(covDirect-covClip)<=covDirect/50+2)?"PASS":"*** FAIL ***");
		fflush(LogFile);
	}

	// ============ B1: NEAR-PLANE CLIP — one vertex behind the near plane ============
	// View-space triangle with vertex A behind Z=C_NZP; force through the clipper.
	MT_Clear();
	MT_SetVtxView(&A, -0.4f,  0.3f, 0.05f,   0.0f,   0.0f, 1);   // Z=0.05 < NEARZ(0.1): behind
	MT_SetVtxView(&B,  0.5f,  0.3f, 2.0f,  255.0f,   0.0f, 1);
	MT_SetVtxView(&C,  0.0f, -0.4f, 2.0f,  128.0f, 255.0f, 1);
	MT_MakeFace(&F,&A,&B,&C); DoFace=&F;
	Universal_Frustrum_Elite();
	cov=MT_Coverage(&bx0,&by0,&bx1,&by1);
	fprintf(LogFile,"B1 near-clip(1 behind): cov=%ld bbox=(%ld,%ld)-(%ld,%ld) (in-bounds, no garbage span)\n",
	        cov,bx0,by0,bx1,by1);
	fflush(LogFile);
	MT_DumpPPM("MT_B1.PPM");

	// ============ B2: NEAR-PLANE CLIP — two vertices behind ============
	MT_Clear();
	MT_SetVtxView(&A, -0.4f,  0.3f, 0.05f,   0.0f,   0.0f, 1);
	MT_SetVtxView(&B,  0.5f,  0.3f, 0.05f, 255.0f,   0.0f, 1);
	MT_SetVtxView(&C,  0.0f, -0.4f, 2.0f,  128.0f, 255.0f, 1);
	MT_MakeFace(&F,&A,&B,&C); DoFace=&F;
	Universal_Frustrum_Elite();
	cov=MT_Coverage(&bx0,&by0,&bx1,&by1);
	fprintf(LogFile,"B2 near-clip(2 behind): cov=%ld bbox=(%ld,%ld)-(%ld,%ld)\n",cov,bx0,by0,bx1,by1);
	fflush(LogFile);
	MT_DumpPPM("MT_B2.PPM");

	// ============ C1: SCREEN-EDGE CLIP — crosses the left edge ============
	MT_Clear();
	MT_SetVtxView(&A, -2.0f,  0.5f, 1.0f,   0.0f,   0.0f, 1);   // far left, off-screen
	MT_SetVtxView(&B,  0.6f,  0.5f, 1.0f, 255.0f,   0.0f, 1);
	MT_SetVtxView(&C,  0.2f, -0.6f, 1.0f, 128.0f, 255.0f, 1);
	MT_MakeFace(&F,&A,&B,&C); DoFace=&F;
	Universal_Frustrum_Elite();
	cov=MT_Coverage(&bx0,&by0,&bx1,&by1);
	fprintf(LogFile,"C1 edge-clip(left): cov=%ld bbox=(%ld,%ld)-(%ld,%ld) (x0 must be ~0, not negative wrap)\n",
	        cov,bx0,by0,bx1,by1);
	fflush(LogFile);
	MT_DumpPPM("MT_C1.PPM");

	// ============ D1: robot reconstruction — oblique tri passing close, through near plane ==
	MT_Clear();
	MT_SetVtxView(&A, -0.2f,  0.6f, 0.06f,   0.0f,   0.0f, 1);   // top, very close (behind near)
	MT_SetVtxView(&B,  0.3f,  0.5f, 0.5f,  255.0f,   0.0f, 1);
	MT_SetVtxView(&C, -0.1f, -0.7f, 3.0f,  128.0f, 255.0f, 1);   // bottom, far
	MT_MakeFace(&F,&A,&B,&C); DoFace=&F;
	Universal_Frustrum_Elite();
	cov=MT_Coverage(&bx0,&by0,&bx1,&by1);
	fprintf(LogFile,"D1 robot-near-oblique: cov=%ld bbox=(%ld,%ld)-(%ld,%ld)\n",cov,bx0,by0,bx1,by1);
	fflush(LogFile);
	MT_DumpPPM("MT_D1.PPM");

	// ============ E1: FUSED mapper (The_BiGMMXTrue) — affine, light=128 pass-through ========
	// The mech/buildings' actual filler. With light=128 the gouraud sweep is a no-op, so the
	// output must MATCH A1 exactly. Any deviation = the fused mapper corrupts texels.
	MT_Clear();
	MT_SetVtxScreen(&A, 40.0f, 20.0f, 1.0f,   0.0f,   0.0f);
	MT_SetVtxScreen(&B,280.0f, 20.0f, 1.0f, 255.0f,   0.0f);
	MT_SetVtxScreen(&C,280.0f,220.0f, 1.0f, 255.0f, 255.0f);
	MT_SetVtxScreen(&D, 40.0f,220.0f, 1.0f,   0.0f, 255.0f);
	MT_MakeFaceFused(&F,&A,&B,&C); DoFace=&F; F.Filler(F.A,F.B,F.C);
	MT_MakeFaceFused(&F,&A,&C,&D); DoFace=&F; F.Filler(F.A,F.B,F.C);
	cov=MT_Coverage(&bx0,&by0,&bx1,&by1);
	bad=MT_ValidateAffine(40,20,280,220,6,&me,&chk);
	fprintf(LogFile,"E1 FUSED-affine: cov=%ld checked=%ld BAD=%ld maxerr=%ld  %s\n",
	        cov,chk,bad,me,(bad==0)?"PASS":"*** FAIL ***");
	fflush(LogFile);
	MT_DumpPPM("MT_E1.PPM");

	// ============ E2: FUSED mapper — near-plane clip (Frust_Set_UVL near-interp) ============
	MT_Clear();
	MT_SetVtxView(&A, -0.2f,  0.6f, 0.06f,   0.0f,   0.0f, 1);
	MT_SetVtxView(&B,  0.3f,  0.5f, 0.5f,  255.0f,   0.0f, 1);
	MT_SetVtxView(&C, -0.1f, -0.7f, 3.0f,  128.0f, 255.0f, 1);
	MT_MakeFaceFused(&F,&A,&B,&C); DoFace=&F; Universal_Frustrum_Elite();
	cov=MT_Coverage(&bx0,&by0,&bx1,&by1);
	fprintf(LogFile,"E2 FUSED-nearclip: cov=%ld bbox=(%ld,%ld)-(%ld,%ld)\n",cov,bx0,by0,bx1,by1);
	fflush(LogFile);
	MT_DumpPPM("MT_E2.PPM");

	// ============ E3: MULTI-FACE overlap through Render() — z-buffer + ownership mask =======
	// The real pipeline: FList of overlapping fused faces, sorted, Render() (clears Z, sorted
	// walk, first-wins z-write + gouraud ownership sweep). NEAR face (light=128, full bright)
	// occludes FAR face (light=64, half). If the ownership mask / first-wins z is correct, the
	// overlap region shows the NEAR face at FULL bright. If broken, the FAR face's gouraud
	// sweep re-darkens the near pixels (the "missing/weird/flickering" family).
	{
		static Vertex fa[3], na[3];
		static Face   FF, NF;
		MT_Clear();
		// FAR background triangle: fills most of screen, deep (RZ=0.3), light=64 (half)
		MT_SetVtxScreen(&fa[0],  10.0f,  10.0f, 0.3f,   0.0f,   0.0f);
		MT_SetVtxScreen(&fa[1], 310.0f,  10.0f, 0.3f, 255.0f,   0.0f);
		MT_SetVtxScreen(&fa[2], 160.0f, 230.0f, 0.3f, 128.0f, 255.0f);
		fa[0].LR=fa[0].LG=fa[0].LB=64.0f; fa[1].LR=fa[1].LG=fa[1].LB=64.0f; fa[2].LR=fa[2].LG=fa[2].LB=64.0f;
		MT_MakeFaceFused(&FF,&fa[0],&fa[1],&fa[2]); FF.SortZ.F=3.3333f;   // dz=1/0.3
		// NEAR triangle: centered, near (RZ=1.0), light=128 (full bright)
		MT_SetVtxScreen(&na[0], 110.0f,  60.0f, 1.0f,   0.0f,   0.0f);
		MT_SetVtxScreen(&na[1], 210.0f,  60.0f, 1.0f, 255.0f,   0.0f);
		MT_SetVtxScreen(&na[2], 160.0f, 170.0f, 1.0f, 128.0f, 255.0f);
		MT_MakeFaceFused(&NF,&na[0],&na[1],&na[2]); NF.SortZ.F=1.0f;      // dz=1/1.0, nearer
		// Explicit-order manual render (bypass sort) with a z-probe after EACH face, to see
		// exactly which write wins. Render() copies UV->vert + computes RU/RV; replicate that.
		#define MT_PREP(F) { Vertex *a=(F).A,*b=(F).B,*c=(F).C; \
			a->U=(F).U1;a->V=(F).V1;b->U=(F).U2;b->V=(F).V2;c->U=(F).U3;c->V=(F).V3; \
			a->RU=a->U*a->RZ;a->RV=a->V*a->RZ;b->RU=b->U*b->RZ;b->RV=b->V*b->RZ;c->RU=c->U*c->RZ;c->RV=c->V*c->RZ; }
		MT_Clear();
		MT_PREP(NF); DoFace=&NF; NF.Filler(NF.A,NF.B,NF.C);   // NEAR first
		fprintf(LogFile,"E3x after NEAR: ZBuf(160,115)=%u (expect 49152)\n",(unsigned)(ZBuf?ZBuf[115*XRes+160]:0));
		MT_PREP(FF); DoFace=&FF; FF.Filler(FF.A,FF.B,FF.C);   // FAR second (must be occluded)
		fprintf(LogFile,"E3x after FAR:  ZBuf(160,115)=%u (must STAY 49152; 14746=far wrongly wrote)\n",(unsigned)(ZBuf?ZBuf[115*XRes+160]:0));
		fflush(LogFile);
		MT_DumpPPM("MT_E3x.PPM");

		FList[0]=&FF; FList[1]=&NF; CAll=2;   // deliberately WRONG input order (far, near)
		fprintf(LogFile,"E3 pre-sort:  FList[0]=%s(%.2f) FList[1]=%s(%.2f)\n",
		        (FList[0]==&NF)?"NEAR":"FAR",(double)FList[0]->SortZ.F,
		        (FList[1]==&NF)?"NEAR":"FAR",(double)FList[1]->SortZ.F);
		Radix_SortingASM(FList,SList,CAll);   // should sort ascending: NEAR(1.0) before FAR(3.33)
		fprintf(LogFile,"E3 post-sort: FList[0]=%s(%.2f) FList[1]=%s(%.2f)  (want NEAR then FAR)\n",
		        (FList[0]==&NF)?"NEAR":"FAR",(double)FList[0]->SortZ.F,
		        (FList[1]==&NF)?"NEAR":"FAR",(double)FList[1]->SortZ.F);
		fflush(LogFile);
		Render();                              // clears Z, walks sorted, first-wins z + sweep
		cov=MT_Coverage(&bx0,&by0,&bx1,&by1);
		// Probe overlap center + z-buffer. Near code should be RZ*ZScaleF = 1.0*49152 = 49152;
		// far = 0.3*49152 = 14745 (larger=nearer). If ZBuf[center]==far code, the far face
		// overwrote the near face's z (occlusion inverted). If ==near, z is right & sweep is
		// the culprit. Also re-render near-only and far-only to prove each draws alone.
		{ unsigned char *px=(unsigned char *)VPage; long o=(115*XRes+160)*4;
		  Word zc = ZBuf ? ZBuf[115*XRes+160] : 0;
		  fprintf(LogFile,"E3 multiface: cov=%ld bbox=(%ld,%ld)-(%ld,%ld) center(160,115)=R%d G%d B%d ZBuf=%u "
		          "(near code=49152 expected; far=14745=OCCLUSION BUG)\n",
		          cov,bx0,by0,bx1,by1,(int)px[o+2],(int)px[o+1],(int)px[o],(unsigned)zc); }
		fflush(LogFile);
		MT_DumpPPM("MT_E3.PPM");
		// Near-only control (single face through Render): must show the bright near triangle.
		MT_Clear();
		MT_SetVtxScreen(&na[0], 110.0f,  60.0f, 1.0f,   0.0f,   0.0f);
		MT_SetVtxScreen(&na[1], 210.0f,  60.0f, 1.0f, 255.0f,   0.0f);
		MT_SetVtxScreen(&na[2], 160.0f, 170.0f, 1.0f, 128.0f, 255.0f);
		MT_MakeFaceFused(&NF,&na[0],&na[1],&na[2]); NF.SortZ.F=1.0f;
		FList[0]=&NF; CAll=1; Radix_SortingASM(FList,SList,CAll); Render();
		{ unsigned char *px=(unsigned char *)VPage; long o=(115*XRes+160)*4;
		  Word zc = ZBuf ? ZBuf[115*XRes+160] : 0;
		  fprintf(LogFile,"E3b near-ONLY: center(160,115)=R%d G%d B%d ZBuf=%u (near tri alone must show)\n",
		          (int)px[o+2],(int)px[o+1],(int)px[o],(unsigned)zc); }
		fflush(LogFile);
		MT_DumpPPM("MT_E3b.PPM");
	}

	// ============ E4: SORT in isolation — 6 distinct keys, check permutation + order =======
	// Radix_SortingASM must leave FList a sorted PERMUTATION of the input (no drops/dups).
	{
		static Face sf[6];
		float keys[6]; long k;
		keys[0]=5.0f; keys[1]=2.0f; keys[2]=8.0f; keys[3]=1.0f; keys[4]=9.0f; keys[5]=3.0f;
		for(k=0;k<6;k++){ memset(&sf[k],0,sizeof(Face)); sf[k].SortZ.F=keys[k]; FList[k]=&sf[k]; }
		CAll=6;
		fprintf(LogFile,"E4 sort input : ");
		for(k=0;k<6;k++) fprintf(LogFile,"%.0f ",(double)FList[k]->SortZ.F);
		fprintf(LogFile,"\n");
		Radix_SortingASM(FList,SList,CAll);
		fprintf(LogFile,"E4 sort output: ");
		for(k=0;k<6;k++) fprintf(LogFile,"%.0f ",(double)FList[k]->SortZ.F);
		{ // permutation check: every input face pointer must appear exactly once
		  long seen=0,dups=0,miss=0,m;
		  for(m=0;m<6;m++){ long cnt=0,n; for(n=0;n<6;n++) if(FList[n]==&sf[m]) cnt++;
		    if(cnt==1)seen++; else if(cnt>1)dups++; else miss++; }
		  fprintf(LogFile," | present=%ld dups=%ld missing=%ld %s\n",
		          seen,dups,miss,(seen==6)?"PASS":"*** LIST CORRUPTED ***"); }
		fflush(LogFile);
	}

	// ============ E5: NEAR-CLIP Z-SATURATION — the mech hypothesis ==========================
	// Two overlapping fused faces, BOTH straddling the near plane, at clearly different depths
	// (front face nearer). Adaptive scale is active here (matches the demo). If near-clipped
	// vertices saturate the 16-bit z-code to 65534, both faces tie at max and the nearer one
	// won't reliably occlude the farther -> "weird faces / flicker". Probe: z-code in the
	// clipped region + whether the FRONT face's texel wins over the BACK face's.
	{
		static Vertex fv[3], bv[3];
		static Face   FFa, BFa;
		float zs;
		// Simulate the demo's adaptive scale for a scene whose nearest real vertex Z ~= 1:
		//   zWNear = passMinZ*0.5 clamped [1,256] -> ~0.5..1 ; ZScaleF = 49152*zWNear.
		ZScaleF = 49152.0f;   // representative
		MT_Clear();
		// BACK face: straddles near plane, sits behind the front where they overlap.
		MT_SetVtxView(&bv[0], -0.30f,  0.40f, 0.05f,   0.0f,   0.0f, 1);  // behind near plane
		MT_SetVtxView(&bv[1],  0.35f,  0.40f, 1.20f, 255.0f,   0.0f, 1);
		MT_SetVtxView(&bv[2],  0.02f, -0.45f, 1.20f, 128.0f, 255.0f, 1);
		bv[0].LR=bv[0].LG=bv[0].LB=64.0f; bv[1].LR=bv[1].LG=bv[1].LB=64.0f; bv[2].LR=bv[2].LG=bv[2].LB=64.0f;
		MT_MakeFaceFused(&BFa,&bv[0],&bv[1],&bv[2]); BFa.SortZ.F=1.20f;
		// FRONT face: straddles near plane, NEARER (smaller Z) where they overlap.
		MT_SetVtxView(&fv[0], -0.28f,  0.38f, 0.04f,   0.0f,   0.0f, 1);  // behind near plane, nearer
		MT_SetVtxView(&fv[1],  0.33f,  0.38f, 0.60f, 255.0f,   0.0f, 1);  // nearer than back's 1.20
		MT_SetVtxView(&fv[2],  0.00f, -0.43f, 0.60f, 128.0f, 255.0f, 1);
		MT_MakeFaceFused(&FFa,&fv[0],&fv[1],&fv[2]); FFa.SortZ.F=0.60f;   // nearer -> sorts first
		FList[0]=&BFa; FList[1]=&FFa; CAll=2;
		Radix_SortingASM(FList,SList,CAll);
		Render();
		{ long xx=160,yy=120; unsigned char *px=(unsigned char *)VPage; long o=(yy*XRes+xx)*4;
		  Word zc = ZBuf?ZBuf[yy*XRes+xx]:0;
		  fprintf(LogFile,"E5 nearclip-zsat: center(%ld,%ld)=R%d G%d B%d ZBuf=%u  "
		          "(front light=128 full-bright wins if z OK; ~half=back showing=Z SATURATION BUG; "
		          "ZBuf=65534=clamped)\n",xx,yy,(int)px[o+2],(int)px[o+1],(int)px[o],(unsigned)zc); }
		fflush(LogFile);
		MT_DumpPPM("MT_E5.PPM");
	}

	// ============ TX: TRANSPARENT mapper (The_BiTTrue, test-no-write) — the water path ======
	// White transparent quad over an opaque coordinate-textured background. When the xpar is
	// IN FRONT it must blend (brighten toward white); when BEHIND the opaque it must be
	// occluded (background unchanged). Opaque drawn first (writes z); xpar tests, never writes.
	ZScaleF = 49152.0f;
	{
		static Vertex ov[3], ov2[3], xv[3];
		static Face   OF, OF2, XF;
		long o; unsigned char *px;
		// --- TX1: xpar IN FRONT of opaque -> must brighten ---
		MT_Clear();
		// opaque background (coordinate texture), FAR (RZ=0.4)
		MT_SetVtxScreen(&ov[0],  20.0f,  20.0f, 0.4f,   0.0f,   0.0f);
		MT_SetVtxScreen(&ov[1], 300.0f,  20.0f, 0.4f, 255.0f,   0.0f);
		MT_SetVtxScreen(&ov[2], 160.0f, 220.0f, 0.4f, 128.0f, 255.0f);
		MT_MakeFace(&OF,&ov[0],&ov[1],&ov[2]); DoFace=&OF; OF.Filler(OF.A,OF.B,OF.C);
		{ px=(unsigned char *)VPage; o=(120*XRes+160)*4;
		  fprintf(LogFile,"TX1 opaque-only center(160,120)=R%d G%d B%d\n",(int)px[o+2],(int)px[o+1],(int)px[o]); }
		// white transparent quad, NEAR (RZ=1.0), centered -> in front
		MT_SetVtxScreen(&xv[0], 110.0f,  70.0f, 1.0f,   0.0f,   0.0f);
		MT_SetVtxScreen(&xv[1], 210.0f,  70.0f, 1.0f, 255.0f,   0.0f);
		MT_SetVtxScreen(&xv[2], 160.0f, 170.0f, 1.0f, 128.0f, 255.0f);
		MT_MakeFaceXpar(&XF,&xv[0],&xv[1],&xv[2]); DoFace=&XF; XF.Filler(XF.A,XF.B,XF.C);
		{ px=(unsigned char *)VPage; o=(120*XRes+160)*4;
		  fprintf(LogFile,"TX1 xpar-in-front center(160,120)=R%d G%d B%d (must be BRIGHTER than opaque-only = blended white)\n",
		          (int)px[o+2],(int)px[o+1],(int)px[o]); }
		fflush(LogFile);
		MT_DumpPPM("MT_TX1.PPM");

		// --- TX2: xpar BEHIND opaque -> must be occluded (unchanged) ---
		MT_Clear();
		// opaque, NEAR (RZ=1.0), centered (writes high z)
		MT_SetVtxScreen(&ov2[0], 110.0f,  70.0f, 1.0f,   0.0f,   0.0f);
		MT_SetVtxScreen(&ov2[1], 210.0f,  70.0f, 1.0f, 255.0f,   0.0f);
		MT_SetVtxScreen(&ov2[2], 160.0f, 170.0f, 1.0f, 128.0f, 255.0f);
		MT_MakeFace(&OF2,&ov2[0],&ov2[1],&ov2[2]); DoFace=&OF2; OF2.Filler(OF2.A,OF2.B,OF2.C);
		{ px=(unsigned char *)VPage; o=(120*XRes+160)*4;
		  fprintf(LogFile,"TX2 opaque-only center(160,120)=R%d G%d B%d\n",(int)px[o+2],(int)px[o+1],(int)px[o]); }
		// white transparent quad, FAR (RZ=0.4), same area -> behind the opaque
		MT_SetVtxScreen(&xv[0], 110.0f,  70.0f, 0.4f,   0.0f,   0.0f);
		MT_SetVtxScreen(&xv[1], 210.0f,  70.0f, 0.4f, 255.0f,   0.0f);
		MT_SetVtxScreen(&xv[2], 160.0f, 170.0f, 0.4f, 128.0f, 255.0f);
		MT_MakeFaceXpar(&XF,&xv[0],&xv[1],&xv[2]); DoFace=&XF; XF.Filler(XF.A,XF.B,XF.C);
		{ px=(unsigned char *)VPage; o=(120*XRes+160)*4;
		  fprintf(LogFile,"TX2 xpar-behind center(160,120)=R%d G%d B%d (must EQUAL opaque-only = occluded, NOT brightened)\n",
		          (int)px[o+2],(int)px[o+1],(int)px[o]); }
		fflush(LogFile);
		MT_DumpPPM("MT_TX2.PPM");
	}

	// ============ TX3: TILTED transparent over an opaque wall — backwards-z probe ============
	// The city-water case: a transparent quad that SPANS a depth range crossing the opaque.
	// Opaque wall at constant Z=2 (RZ=0.5). White xpar quad tilted: TOP near (Z=1, RZ=1.0),
	// BOTTOM far (Z=3, RZ=0.333). Correct: TOP half of the xpar is IN FRONT of the wall -> it
	// blends (brightens); BOTTOM half is BEHIND -> occluded (wall unchanged). If the z test or
	// the per-pixel z interpolation runs BACKWARDS, top/bottom invert.
	{
		static Vertex wv[3], wv2[3], xt[4];
		static Face   WF, WF2, XT;
		long o; unsigned char *px;
		MT_Clear();
		// opaque wall, constant Z=2 (RZ=0.5), coordinate texture, covers the middle
		MT_SetVtxScreen(&wv[0],  60.0f,  40.0f, 0.5f,   0.0f,   0.0f);
		MT_SetVtxScreen(&wv[1], 260.0f,  40.0f, 0.5f, 255.0f,   0.0f);
		MT_SetVtxScreen(&wv[2], 260.0f, 200.0f, 0.5f, 255.0f, 255.0f);
		MT_MakeFace(&WF,&wv[0],&wv[1],&wv[2]); DoFace=&WF; WF.Filler(WF.A,WF.B,WF.C);
		MT_SetVtxScreen(&wv2[0], 60.0f,  40.0f, 0.5f,   0.0f,   0.0f);
		MT_SetVtxScreen(&wv2[1],260.0f, 200.0f, 0.5f, 255.0f, 255.0f);
		MT_SetVtxScreen(&wv2[2], 60.0f, 200.0f, 0.5f,   0.0f, 255.0f);
		MT_MakeFace(&WF2,&wv2[0],&wv2[1],&wv2[2]); DoFace=&WF2; WF2.Filler(WF2.A,WF2.B,WF2.C);
		{ px=(unsigned char *)VPage;
		  fprintf(LogFile,"TX3 wall top(160,60)=R%d  bottom(160,180)=R%d (baseline)\n",
		          (int)px[(60*XRes+160)*4+2],(int)px[(180*XRes+160)*4+2]); }
		// white xpar quad, TILTED in depth: top near (RZ=1.0), bottom far (RZ=0.333)
		MT_SetVtxScreen(&xt[0], 100.0f,  50.0f, 1.0f,     0.0f,   0.0f);   // top-left  NEAR
		MT_SetVtxScreen(&xt[1], 220.0f,  50.0f, 1.0f,   255.0f,   0.0f);   // top-right NEAR
		MT_SetVtxScreen(&xt[2], 220.0f, 190.0f, 0.333f, 255.0f, 255.0f);   // bot-right FAR
		MT_SetVtxScreen(&xt[3], 100.0f, 190.0f, 0.333f,   0.0f, 255.0f);   // bot-left  FAR
		MT_MakeFaceXpar(&XT,&xt[0],&xt[1],&xt[2]); DoFace=&XT; XT.Filler(XT.A,XT.B,XT.C);
		MT_MakeFaceXpar(&XT,&xt[0],&xt[2],&xt[3]); DoFace=&XT; XT.Filler(XT.A,XT.B,XT.C);
		{ px=(unsigned char *)VPage;
		  int rtop=(int)px[(60*XRes+160)*4+2], rbot=(int)px[(180*XRes+160)*4+2];
		  fprintf(LogFile,"TX3 after-xpar top(160,60)=R%d bottom(160,180)=R%d  "
		          "(TOP must BRIGHTEN=xpar-in-front, BOTTOM must stay=xpar-occluded; inverted=BACKWARDS-Z)\n",
		          rtop,rbot); }
		fflush(LogFile);
		MT_DumpPPM("MT_TX3.PPM");
	}

	// ============ E6: FAR-WRAP — geometry past 1.1*FZP must not appear NEAR ==================
	// enc = 0FF80h - ZLinScale*Z goes negative past Z ~= 0FF80h/ZLinScale; stored as a 16-bit
	// word that WRAPS to a large (false-near) value -> a distant wall would occlude near
	// buildings (the "farther buildings hiding closer ones" bug). ZLinScale=24 here => wrap at
	// Z ~= 2725. NEAR face Z=1, FAR face Z=2730 (just past wrap) over the same area. With the
	// clamp, the FAR face stays behind; without it, the FAR face punches through.
	{
		static Vertex nf[3], ff[3];
		static Face   NFa, FFa;
		long o; unsigned char *px;
		MT_Clear();
		// NEAR face (Z=1), draw first (writes enc)
		MT_SetVtxScreen(&nf[0], 100.0f,  60.0f, 1.0f,   0.0f,   0.0f);
		MT_SetVtxScreen(&nf[1], 220.0f,  60.0f, 1.0f, 255.0f,   0.0f);
		MT_SetVtxScreen(&nf[2], 160.0f, 170.0f, 1.0f, 128.0f, 255.0f);
		MT_MakeFace(&NFa,&nf[0],&nf[1],&nf[2]); DoFace=&NFa; NFa.Filler(NFa.A,NFa.B,NFa.C);
		{ px=(unsigned char *)VPage; o=(115*XRes+160)*4;
		  fprintf(LogFile,"E6 near-only center(160,115)=R%d G%d\n",(int)px[o+2],(int)px[o+1]); }
		// FAR face (Z=2730, just past the wrap), same area, drawn second
		MT_SetVtxScreen(&ff[0], 100.0f,  60.0f, 1.0f/2730.0f,   0.0f,   0.0f);
		MT_SetVtxScreen(&ff[1], 220.0f,  60.0f, 1.0f/2730.0f, 255.0f,   0.0f);
		MT_SetVtxScreen(&ff[2], 160.0f, 170.0f, 1.0f/2730.0f, 128.0f, 255.0f);
		MT_MakeFace(&FFa,&ff[0],&ff[1],&ff[2]); DoFace=&FFa; FFa.Filler(FFa.A,FFa.B,FFa.C);
		{ px=(unsigned char *)VPage; o=(115*XRes+160)*4;
		  Word zc = ZBuf?ZBuf[115*XRes+160]:0;
		  fprintf(LogFile,"E6 after-far center(160,115)=R%d G%d ZBuf=%u  "
		          "(must EQUAL near-only = far stayed behind; changed = FAR-WRAP occluding near)\n",
		          (int)px[o+2],(int)px[o+1],(unsigned)zc); }
		fflush(LogFile);
		MT_DumpPPM("MT_E6.PPM");
	}

	fprintf(LogFile,"MAPTEST: done\n"); fflush(LogFile);
}

#endif // FDS_MAPTEST
