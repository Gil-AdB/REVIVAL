// Perspective frustrum
void UFP1(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2));
void UFP2(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2));
void UFP3(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2));
void UFP4(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2));

void Bivex_Frustrum_Perspective(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
  Vertex *T;
  Vertex Vtx1;
  float L,R;

  if (A->TPos.Z>B->TPos.Z) {T=A; A=B; B=T;}

  if (B->TPos.Z<1) return;
  if (A->TPos.Z<1)
  {
    L=(A->TPos.Z-1)/(A->TPos.Z-B->TPos.Z);
    R=1-L;
    Vtx1.PX = CntrX+FOVX*(A->TPos.X*R + B->TPos.X*L);
    Vtx1.PY = CntrY-FOVY*(A->TPos.Y*R + B->TPos.Y*L);

    Vtx1.LR = A->LR*R + B->LR*L;

    UFP1(&Vtx1,B,Filler);
    return;
  }
  UFP1(A,B,Filler);
}

// heh heh, ZOL.
void UFP1(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
  Vertex *T;
  Vertex Vtx1;
  float Y1,Y2;
  float L,R;
  float FDiv;

  if (A->PX>B->PX) {T=A; A=B; B=T;}
  if (B->PX<0) return;
  if (A->PX<0)
  {
    L = A->PX/(A->PX-B->PX);   //ZOL
    if (L<-0.01||L>1.01) {
      Deinitialize_VESA();
      printf("UFC1:Abnormal Interpolation rate (A=(%f,%f,%f),B=(%f,%f,%f),Y1=%f,Y2=%f,L=%f)",A->TPos.X,A->TPos.Z,A->TPos.Y,B->TPos.X,B->TPos.Z,B->TPos.Y,Y1,Y2,L);
      exit(1);
    }
    R = 1-L;
    Vtx1.PX = 0; // CntrX + FOVX*Vtx1.TPos.X/Vtx1.TPos.Z;
    Vtx1.PY = A->PY*R + B->PY*L;
    Vtx1.LR = A->LR*R + B->LR*L;

    UFP2(&Vtx1,B,Filler);
    return;
  }
  UFP2(A,B,Filler);
}

void UFP2(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
  Vertex *T;
  Vertex Vtx1;
  float Y1,Y2;
  float L,R;
  float XClip = XRes_1;

  if (A->PX>XClip) return;
  if (B->PX>XClip)
  {
    R = (B->PX-XClip)/(B->PX-A->PX);  // ZOL
    if (R<-0.01||R>1.01) {
      Deinitialize_VESA();
      printf("UFC2:Abnormal Interpolation rate (A=(%f,%f,%f),B=(%f,%f,%f),Y1=%f,Y2=%f,R=%f)",A->TPos.X,A->TPos.Z,A->TPos.Y,B->TPos.X,B->TPos.Z,B->TPos.Y,Y1,Y2,R);
      exit(1);
    }
    L = 1-R;
    Vtx1.PX = XClip; // CntrX + FOVX*Vtx1.TPos.X/Vtx1.TPos.Z;
    Vtx1.PY = A->PY*R + B->PY*L; //Vtx1.TPos.Y/Vtx1.TPos.Z;
    Vtx1.LR = A->LR*R + B->LR*L;

    UFP3(A,&Vtx1,Filler);
    return;
  }
  UFP3(A,B,Filler);
}

void UFP3(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
  Vertex *T;
  Vertex Vtx1;
  float Y1,Y2;
  float L,R;
  float FDiv;

  if (A->PY>B->PY) {T=A; A=B; B=T;}
  if (B->PY<0) return;
  if (A->PY<0)
  {
    L = A->PY/(A->PY-B->PY);   //  ZOL
    if (L<-0.01||L>1.01) {
      Deinitialize_VESA();
      printf("UFC3:Abnormal Interpolation rate (A=(%f,%f,%f)|(%f,%f),B=(%f,%f,%f)|(%f,%f),Y1=%f,Y2=%f,L=%f)",A->TPos.X,A->TPos.Z,A->TPos.Y,A->PX,A->PY,B->TPos.X,B->TPos.Z,B->TPos.Y,B->PX,B->PY,Y1,Y2,L);
      exit(1);
    }
    R = 1-L;
    Vtx1.PX = A->PX*R + B->PX*L;//Vtx1.TPos.X/Vtx1.TPos.Z;
    Vtx1.PY = 0;//CntrY - FOVY*Vtx1.TPos.Y/Vtx1.TPos.Z;

    Vtx1.LR = A->LR*R + B->LR*L;

    UFP4(&Vtx1,B,Filler);
    return;
  }
  UFP4(A,B,Filler);
}

void UFP4(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
  Vertex *T;
  Vertex Vtx1;
  float Y1,Y2;
  float L,R;
  float YClip = YRes_1;

  if (A->PY>YClip) return;
  if (B->PY>YClip)
  {
    R = (B->PY-YClip)/(B->PY-A->PY);   //ZOL
    if (R<-0.01||R>1.01) {
      Deinitialize_VESA();
      printf("UFC4:Abnormal Interpolation rate (A=(%f,%f,%f)|(%f,%f),B=(%f,%f,%f)|(%f,%f),Y1=%f,Y2=%f,R=%f)",A->TPos.X,A->TPos.Z,A->TPos.Y,A->PX,A->PY,B->TPos.X,B->TPos.Z,B->TPos.Y,B->PX,B->PY,Y1,Y2,R);
      exit(1);
    }
    L = 1-R;
    Vtx1.PX = A->PX*R + B->PX*L;
    Vtx1.PY = YClip;// CntrY - FOVY*Vtx1.TPos.Y/Vtx1.TPos.Z;

    Vtx1.LR = A->LR*R + B->LR*L;

    Filler(A,&Vtx1);
    return;
  }
  Filler(A,B);
}
