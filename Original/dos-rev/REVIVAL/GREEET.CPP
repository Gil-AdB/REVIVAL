#include "Rev.H"

#include "Greets.H"

static Scene *GreetSc;

static long GRPartTime = 5.0f*100;

void Initialize_Greets()
{
	GreetSc = new Scene; memset(GreetSc,0,sizeof(Scene));
	LoadFLD(GreetSc,"Scenes\\GREETS.FLD");
//	GRPartTime = 100.0*(GreetSc->EndFrame-GreetSc->StartFrame)/30.0f;

//	for(Material *M=MatLib;M;M=M->Next)
//		M->Flags|=Mat_RGBInterp;

	printf("FLD-loaded MEM = %d\n",DPMI_Free_Memory());
	Preprocess_Scene(GreetSc);
	printf("Scene-Proc MEM = %d\n",DPMI_Free_Memory());
	GreetSc->FZP = 25000.0f;
	GreetSc->Flags |= Scn_Fogged;
	// also make the appropriate Layer 2 fillers,
}

void rTransform_Objects(Scene *Sc)
{
	TriMesh *T;
	Omni *O;
	Matrix M,IM;
	float M34[3][4];
	Vector AP,S,U,V,*W=(Vector *)(&M),*W2,*Scl;
	float L1,L2,L3;
	Vertex *Vtx,*VEnd;
	Face *F,*FEnd;
	float PX=FOVX,PY=FOVY,Temp;
	float dz;
	long *pdz = (long *)(&dz);
	long I;
	Face **Ins = FList;
	float *f = (float *)(&M);
	float *fv;

	for (T=Sc->TriMeshHead;T;T=T->Next)
	{
		if (!(T->Flags&HTrack_Visible)) {T->Flags|=Tri_Invisible; continue;}
		MatrixXMatrix(View->Mat,T->RotMat,M);
		Matrix_Copy(IM,M);
		// Advanced Matrix...(watch this)
		Vector_Scale(W,PX,W);
		Vector_Scale(W+1,-PY,W+1);
		Vector_Scale(W+2,CntrEX,&V);
		Vector_SelfAdd(W,&V);
		Vector_Scale(W+2,CntrEY,&V);
		Vector_SelfAdd(W+1,&V);
		// Supermatrix ready.

		// postrioric Offset Vector.
		Vector_Sub(&T->IPos,&View->ISource,&U);
		MatrixXVector(View->Mat,&U,&S);
		V.X = CntrEX*S.Z+PX*S.X;
		V.Y = CntrEY*S.Z-PY*S.Y;
		V.Z = S.Z;
		// make a corrected sphere center vector
		MatrixXVector(IM,&T->BSphereCtr,&AP);
		Vector_SelfAdd(&S,&AP);

//    Vector_Copy(&V,&S);
		// ready
		// 4x3 AFFINE XFORM
		M34[0][0] = M[0][0]; M34[0][1] = M[0][1]; M34[0][2] = M[0][2]; M34[0][3] = V.X;
		M34[1][0] = M[1][0]; M34[1][1] = M[1][1]; M34[1][2] = M[1][2]; M34[1][3] = V.Y;
		M34[2][0] = M[2][0]; M34[2][1] = M[2][1]; M34[2][2] = M[2][2]; M34[2][3] = V.Z;
		// ready


		// aprioric Offset Vector.
		MatrixTXVector(T->RotMat,&U,&AP);
		// ready
		// Bounding Sphere Elimination test Begins.
		W2 = (Vector *)(&T->RotMat);
		L2 = Dot_Product(W2,W2);
		if ((L1 = Dot_Product(W2+1,W2+1))>L2) L2=L1;
		if ((L1 = Dot_Product(W2+2,W2+2))>L2) L2=L1;

		T->Flags&=0xFFFFFFFF-Tri_Invisible-Tri_Ahead-Tri_Inside;
		// Out by depth
		if (S.Z*S.Z>L2*T->BSphereRad)
		{
			if (S.Z<0)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
			T->Flags |= Tri_Ahead;
		}

		dz = S.Z - Sc->FZP;
		if (dz*dz>L2*T->BSphereRad)
		{
			if (dz>0.0f)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
		}
		// Out by left/right
		S.X=fabs(S.X);
		L1 = PX*S.X - CntrEX*S.Z;
		if (L1*L1>L2*T->BSphereRad*(PX*PX+CntrEX*CntrEX))
		{
			if (S.X*PX>S.Z*CntrEX)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
			if (T->Flags&Tri_Ahead) T->Flags |= Tri_Inside;
		}
		// Out by up/down
		S.Y = fabs(S.Y);
		L1 = PY*S.Y - CntrEY*S.Z;
		if (L1*L1>L2*T->BSphereRad*(PY*PY+CntrEY*CntrEY))
		{
			if (S.Y*PY>S.Z*CntrEY)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
		} else T->Flags&=0xFFFFFFFF-Tri_Inside;
		VEnd=T->Verts+T->VIndex;

/*    FEnd=T->Face+T->NumOfFaces;
		for (F=T->Face;F<FEnd;F++)
			if (!(F->Txtr->Flags&Mat_TwoSided))
				F->Flags = (AP.X*F->N.X + AP.Y*F->N.Y + AP.Z*F->N.Z>=F->NormProd);*/


//    Main vertex loop,in case no restrictions apply.
		if (!(T->Flags&Tri_Phong))
		{
			if (!(T->Flags&Tri_Inside))
			{
				if (!(T->Flags&Tri_Ahead))
					goto Regular;
				else goto Ahead;
			}
			// Intel inside...this rulez,all object completely inside frustrum.
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
//        MatrixXVector(M,&Vtx->Pos,&U);
//        Vector_Add(&U,&V,&Vtx->TPos);
				// 4x3 xform
				Vtx->TPos.X = M34[0][0]*Vtx->Pos.X+M34[0][1]*Vtx->Pos.Y+M34[0][2]*Vtx->Pos.Z+M34[0][3];
				Vtx->TPos.Y = M34[1][0]*Vtx->Pos.X+M34[1][1]*Vtx->Pos.Y+M34[1][2]*Vtx->Pos.Z+M34[1][3];
				Vtx->TPos.Z = M34[2][0]*Vtx->Pos.X+M34[2][1]*Vtx->Pos.Y+M34[2][2]*Vtx->Pos.Z+M34[2][3];

				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				Vtx->RZ=1.0/Vtx->TPos.Z;
				Vtx->PX=Vtx->TPos.X*Vtx->RZ;
				Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//        Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//        Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//        Vtx->RU=Vtx->U*Vtx->RZ;
//        Vtx->RV=Vtx->V*Vtx->RZ;
				if (Vtx->TPos.Z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
			}

			goto OUT;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
			Ahead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
	//    if (!Vtx->FRem) continue;
//        MatrixXVector(M,&Vtx->Pos,&U);
//        Vector_Add(&U,&V,&Vtx->TPos);
				Vtx->TPos.X = M34[0][0]*Vtx->Pos.X+M34[0][1]*Vtx->Pos.Y+M34[0][2]*Vtx->Pos.Z+M34[0][3];
				Vtx->TPos.Y = M34[1][0]*Vtx->Pos.X+M34[1][1]*Vtx->Pos.Y+M34[1][2]*Vtx->Pos.Z+M34[1][3];
				Vtx->TPos.Z = M34[2][0]*Vtx->Pos.X+M34[2][1]*Vtx->Pos.Y+M34[2][2]*Vtx->Pos.Z+M34[2][3];

        Vtx->RZ=1.0/Vtx->TPos.Z;
				Vtx->PX=Vtx->TPos.X*Vtx->RZ;
				Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//        Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//        Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//        Vtx->RU=Vtx->U*Vtx->RZ;
//        Vtx->RV=Vtx->V*Vtx->RZ;
				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
				if (Vtx->PX>XRes_1) Vtx->Flags|=Vtx_VisRight;
				if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
				if (Vtx->PY>YRes_1) Vtx->Flags|=Vtx_VisDown;
				if (Vtx->TPos.Z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
			}
	//    printf("Ahead VGA/Wizard.\n");
			goto OUT;
			Regular:
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
      {
  //    if (!Vtx->FRem) continue;
//        MatrixXVector(M,&Vtx->Pos,&U);
//        Vector_Add(&U,&V,&Vtx->TPos);
				Vtx->TPos.X = M34[0][0]*Vtx->Pos.X+M34[0][1]*Vtx->Pos.Y+M34[0][2]*Vtx->Pos.Z+M34[0][3];
        Vtx->TPos.Y = M34[1][0]*Vtx->Pos.X+M34[1][1]*Vtx->Pos.Y+M34[1][2]*Vtx->Pos.Z+M34[1][3];
        Vtx->TPos.Z = M34[2][0]*Vtx->Pos.X+M34[2][1]*Vtx->Pos.Y+M34[2][2]*Vtx->Pos.Z+M34[2][3];

        Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
  //      if (*(long *)(&Vtx->TPos.Z)>0x3F800000) // 1.0 in floating point rep.
				if (Vtx->TPos.Z>1.0)
				{
					Vtx->RZ=1.0/Vtx->TPos.Z;
					Vtx->PX=Vtx->TPos.X*Vtx->RZ;
					Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//          Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//          Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//          Vtx->RU=Vtx->U*Vtx->RZ;
//          Vtx->RV=Vtx->V*Vtx->RZ;
					if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
					if (Vtx->PX>XRes_1) Vtx->Flags|=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
					if (Vtx->PY>YRes_1) Vtx->Flags|=Vtx_VisDown;
				} else Vtx->Flags|=Vtx_VisNear;
				if (Vtx->TPos.Z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
	//      printf("Regular shit!\n");
			}
		} else {
			// instead of all of these complications, I've decided to
			// make the face have void (*Clipper), that will do whatever it needs
			// in one call. the pre-filler will call the asm rasterizers twice
			// if necessary. back to the good old Avatar engine techniques ;)
			// at this section, the code also calculates environment mapping
			// coordinates to (EU,EV) by rotating the v. normals accordingly.
			if (!(T->Flags&Tri_Inside))
			{
				if (!(T->Flags&Tri_Ahead))
					goto ERegular;
				else goto EAhead;
			}
			// Intel inside...this rulez,all object completely inside frustrum.
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
        MatrixXVector(M,&Vtx->Pos,&U);
				Vector_Add(&U,&V,&Vtx->TPos);

				Vtx->Flags=0;
				Vtx->RZ=1.0/Vtx->TPos.Z;
        Vtx->PX=Vtx->TPos.X*Vtx->RZ;
        Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//        Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//        Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
        Vtx->EU=128.0+127.0*(Vtx->N.X*IM[0][0]+Vtx->N.Y*IM[0][1]+Vtx->N.Z*IM[0][2]);
        Vtx->REU=Vtx->EU*Vtx->RZ;
        Vtx->EV=128.0+127.0*(Vtx->N.X*IM[1][0]+Vtx->N.Y*IM[1][1]+Vtx->N.Z*IM[1][2]);
        Vtx->REV=Vtx->EV*Vtx->RZ;
//        Vtx->RU=Vtx->U*Vtx->RZ;
//        Vtx->RV=Vtx->V*Vtx->RZ;
      }
      goto OUT;
      // This is in case 100% of trimesh AHEAD of camera. this saves some chks
      EAhead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
  //    if (!Vtx->FRem) continue;
        MatrixXVector(M,&Vtx->Pos,&U);
        Vector_Add(&U,&V,&Vtx->TPos);

        Vtx->RZ=1.0/Vtx->TPos.Z;
        Vtx->EU=128.0+127.0*(Vtx->N.X*IM[0][0]+Vtx->N.Y*IM[0][1]+Vtx->N.Z*IM[0][2]);
        Vtx->REU=Vtx->EU*Vtx->RZ;
        Vtx->EV=128.0+127.0*(Vtx->N.X*IM[1][0]+Vtx->N.Y*IM[1][1]+Vtx->N.Z*IM[1][2]);
        Vtx->REV=Vtx->EV*Vtx->RZ;

        Vtx->PX=Vtx->TPos.X*Vtx->RZ;
				Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//        Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//        Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//        Vtx->RU=Vtx->U*Vtx->RZ;
//        Vtx->RV=Vtx->V*Vtx->RZ;
        if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft; else Vtx->Flags=0;
        if (Vtx->PX>XRes_1) Vtx->Flags+=Vtx_VisRight;
				if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
        if (Vtx->PY>YRes_1) Vtx->Flags+=Vtx_VisDown;
      }
  //    printf("Ahead VGA/Wizard.\n");
      goto OUT;
			ERegular:
      for (Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
  //    if (!Vtx->FRem) continue;
        MatrixXVector(M,&Vtx->Pos,&U);
        Vector_Add(&U,&V,&Vtx->TPos);

				Vtx->Flags = 0;
  //      if (*(long *)(&Vtx->TPos.Z)>0x3F800000) // 1.0 in floating point rep.
        Vtx->EU=128.0+127.0*(Vtx->N.X*IM[0][0]+Vtx->N.Y*IM[0][1]+Vtx->N.Z*IM[0][2]);
        Vtx->EV=128.0+127.0*(Vtx->N.X*IM[1][0]+Vtx->N.Y*IM[1][1]+Vtx->N.Z*IM[1][2]);

        if (Vtx->TPos.Z>1.0)
        {
          Vtx->RZ=1.0/Vtx->TPos.Z;
					Vtx->PX=Vtx->TPos.X*Vtx->RZ;
          Vtx->PY=Vtx->TPos.Y*Vtx->RZ;
//          Vtx->PX=CntrEX+PX*Vtx->TPos.X*Vtx->RZ;
//          Vtx->PY=CntrEY-PY*Vtx->TPos.Y*Vtx->RZ;
//          Vtx->RU=Vtx->U*Vtx->RZ;
//          Vtx->RV=Vtx->V*Vtx->RZ;
          Vtx->REU=Vtx->EU*Vtx->RZ;
          Vtx->REV=Vtx->EV*Vtx->RZ;
          if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft;
          if (Vtx->PX>XRes_1) Vtx->Flags+=Vtx_VisRight;
          if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
					if (Vtx->PY>YRes_1) Vtx->Flags+=Vtx_VisDown;
        } else Vtx->Flags=Vtx_VisNear;
	//      printf("Regular shit!\n");
      }

    }
    OUT:FEnd=T->Faces+T->FIndex;
    for (F=T->Faces;F<FEnd;F++)
      if ((!(F->A->Flags&F->B->Flags&F->C->Flags&Vtx_Visible))
					 &&((F->Txtr->Flags&Mat_TwoSided)||(AP.X*F->N.X + AP.Y*F->N.Y + AP.Z*F->N.Z<F->NormProd)))
      {
        *Ins++ = F;
        dz = F->A->TPos.Z;
//        if (*(long *)(&F->B->TPos.Z)>*pdz) dz=F->B->TPos.Z;
//        if (*(long *)(&F->C->TPos.Z)>*pdz) dz=F->C->TPos.Z;
				if (F->B->TPos.Z>dz) dz=F->B->TPos.Z;
        if (F->C->TPos.Z>dz) dz=F->C->TPos.Z;
//        dz*=-16384;
//        dz+=0x7FFFFFFF;
//        RoundToInt(&F->SortZ.DW,dz);
        F->SortZ.F = 100000.0-dz;
      }
  }
  CPolys = Ins-FList;
  for(O=Sc->OmniHead;O;O=O->Next)
  {
    Vtx=&O->V;
    Vector_Sub(&O->IPos,&View->ISource,&V);
		MatrixXVector(View->Mat,&V,&Vtx->TPos);
		if (Vtx->TPos.Z>1)
    {
      Vtx->RZ=1.0/Vtx->TPos.Z;
      Vtx->PX=CntrEX+Vtx->TPos.X*PX*Vtx->RZ;
      Vtx->PY=CntrEY-Vtx->TPos.Y*PY*Vtx->RZ;
			// Insert to List
      dz = Vtx->TPos.Z;
      //dz *=-16384;
      //dz +=0x7FFFFFFF;
			//RoundToInt(&O->Face.SortZ.DW,dz);
      O->F.SortZ.F = 100000.0-dz;
      *Ins++ = &O->F;
		}
  }
  COmnies = (Ins-FList)-CPolys;
	for(I=0;I<Sc->NumOfParticles;I++)
		if (Sc->Pcl[I].Flags&Particle_Active)
			if ((dz=Sc->Pcl[I].V.TPos.Z)>=1.0) {
				F = &Sc->Pcl[I].F;
			//dz *=-16384;
			//dz +=0x7FFFFFFF;
			//RoundToInt(&F->SortZ.DW,dz);
				F->SortZ.F = 100000.0-dz;
				*Ins++ = F;
			}
	CAll = Ins-FList;
  CPcls = CAll-COmnies-CPolys;
}


void Run_Greets()
{
	long Polys = 0;
	long TTrd;
	TriMesh *T;
	Omni *O;

	CurScene = GreetSc;
	for(T = GreetSc->TriMeshHead;T;T=T->Next)
		Polys+=T->FIndex;
	for(O = GreetSc->OmniHead;O;O=O->Next)
		Polys++;
	FList = new Face * [Polys];
	SList = new Face * [Polys];

	View = GreetSc->CameraHead;
//	View = &FC;

	C_FZP = GreetSc->FZP;
	C_rFZP = 1.0f/C_FZP;

	TTrd = Timer;

	Ambient_Factor = 16.0f;
	Diffusive_Factor = 2000.0f;
	ImageSize = 3500;
	while ((!Keyboard[ScESC])&&Timer<GRPartTime)
	{
		dTime = Timer-TTrd;
		TTrd = Timer;
//		Dynamic_Camera();

		memset(VPage,0,PageSize);

		CurFrame = GreetSc->StartFrame;// + (GreetSc->EndFrame-GreetSc->StartFrame) * (float)Timer / (float)GRPartTime;

		Animate_Objects(GreetSc);

		Transform_Objects(GreetSc);

		Lighting(GreetSc);

		if (!CAll) continue;
		Radix_SortingASM(FList,SList,CAll);

		Render();

		Flip(Screen);

	} Timer-=GRPartTime;
	delete FList;
	delete SList;
	Destroy_Scene(GreetSc);
}