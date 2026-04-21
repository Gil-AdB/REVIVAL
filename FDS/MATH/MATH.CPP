// Silvatar's Mathematics Unit, Version 0.5
// Next : Fix Quaternion Splines (AGAIN)
// did it! at least for small angles,and possibly improve the deriviation
// for better results than 3ds...but then again, i still need to find
// a better interpolator than SQUAD.
// Real next ;) : Add Euler angle spline interpolation. (WAITING: LWS Reader)
// done. this code was left alone for 6+ months. arrgh.

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "simde/x86/sse4.1.h"

//Externals
float SinTab[1440];
float CosTab[1440];

Vector ZeroVector;
Matrix Mat_ID;
Quaternion Quat_ID;

// Functions.
// Generates Sine/Cosine Lookup Tables, at 1/4 a Degree accuracy.
void Generate_SCTabs()
{
	int32_t I;
	SinTab[0]=0.0;
	CosTab[0]=1.0;
	SinTab[1]=sin(PI/720);
	CosTab[1]=cos(PI/720);
	for(I=2;I<1440;I++)
	{
		SinTab[I]=SinTab[I-1]*CosTab[1]+CosTab[I-1]*SinTab[1];
		CosTab[I]=CosTab[I-1]*CosTab[1]-SinTab[I-1]*SinTab[1];
	}
}

void Init_Identities()
{
	Vector_Form(&ZeroVector,0,0,0);
	Matrix_Form(Mat_ID,1,0,0, 0,1,0, 0,0,1);
	Quaternion_Form(&Quat_ID,0,0,0,1);
}

// Puts values (X,Y,Z) into the Vector V.
void Vector_Form(Vector *V,float X,float Y,float Z)
{
	V->x = X;
	V->y = Y;
	V->z = Z;
}

Vector Vector_Make(float X,float Y,float Z)
{
	Vector V;
	V.x = X;
	V.y = Y;
	V.z = Z;
	return V;
}

void Vector_Zero(Vector *U)
{
	//memset(U,0,sizeof(Vector));
	U->x = U->y = U->z = 0.0;
}

// Copies V to U. Trashes previous U value.
void Vector_Copy(Vector *U,Vector *V)
{
	//  memcpy(U,V,sizeof(Vector));
	U->x = V->x;
	U->y = V->y;
	U->z = V->z;
}

// Scales Vector V by S into itself.
void Vector_SelfScale(Vector *V,float S)
{
	V->x *= S;
	V->y *= S;
	V->z *= S;
}

// Returns Vector length of Vector V.
float Vector_Length(Vector *V)
{
	return sqrt(V->x * V->x + V->y * V->y + V->z * V->z);
}


//// Returns the Vector length of Vector V, squared.
//// Equivelent of Dot_Product(V,V)
//float Vector_SelfDot(Vector *V)
//{
//	return V->x * V->x + V->y * V->y + V->z * V->z;
//}

// Normalizes Vector V.
void Vector_Norm(Vector *V)
{
	Vector_Scale(V,1.0/Vector_Length(V),V);
}


// Normalizes Vector V.
//void Vector_Norm(XMMVector *V)
//{
//	__m128 v = _mm_load_ps((float *)V);
////	v = _mm_and_ps(v, mask);
//	__m128 inverse_norm = _mm_rsqrt_ps(_mm_dp_ps(v, v, 0x77));
//	v = _mm_mul_ps(v, inverse_norm);
//	V->x = v.m128_f32[0];
//	V->y = v.m128_f32[1];
//	V->z = v.m128_f32[2];
//	//	Vector_Scale(V,1.0/Vector_Length(V),V);
//}

// Adds Vectors U+V into W.
/*void Vector_Add(Vector *U,Vector *V,Vector *W)
{
W->x = U->x + V->x;
W->y = U->y + V->y;
W->z = U->z + V->z;
} */

//void Vector_SelfAdd(Vector *U,Vector *V)
//{
//	U->x += V->x;
//	U->y += V->y;
//	U->z += V->z;
//}
//
//void Vector_SelfAdd(UAVector *U, UAVector *V)
//{
//	U->x += V->x;
//	U->y += V->y;
//	U->z += V->z;
//}

// Subtracts Vectors U-V into W.
/*void Vector_Sub(Vector *U,Vector *V,Vector *W)
{
W->x = U->x - V->x;
W->y = U->y - V->y;
W->z = U->z - V->z;
} */

void Vector_SelfSub(Vector *U,Vector *V)
{
	U->x -= V->x;
	U->y -= V->y;
	U->z -= V->z;
}
// Performs U += (V*S), a scaled addition operation.
void Vector_SAdd(Vector *U,float S,Vector *V)
{
	U->x += S * V->x;
	U->y += S * V->y;
	U->z += S * V->z;
}

// Performs W := A*U + B*V.
void Vector_LComb(Vector *U,Vector *V,float A,float B,Vector *W)
{
	W->x = A * U->x + B * V->x;
	W->y = A * U->y + B * V->y;
	W->z = A * U->z + B * V->z;
}

// Linear Interpolation between U and V at the rate T ==> W
void Vector_LERP(Vector *U,Vector *V,float T,Vector *W)
{
	Vector_LComb(U,V,1-T,T,W);
}

//// Returns Dot Product of U and V.
//float Dot_Product(Vector *U,Vector *V)
//{
//	return U->x * V->x + U->y * V->y + U->z * V->z;
//}

// Computes Cross product of UxV into W.
void Cross_Product(Vector *U,Vector *V,Vector *W)
{
	W->x = U->y * V->z - U->z * V->y;
	W->y = U->z * V->x - U->x * V->z;
	W->z = U->x * V->y - U->y * V->x;
}

// Returns Cos(Angle between U and V)
float Vector_CosAngle(Vector *U,Vector *V)
{
	return Dot_Product(U,V)/(Vector_Length(U)*Vector_Length(V));
}

// Returns the Angle between U and V.
float Vector_Angle(Vector *U,Vector *V)
{
	return acos(Vector_CosAngle(U,V));
}

// Calculates Eucleadian Distance from V to U, (= ||V-U||)
float Distance(Vector *V,Vector *U)
{
	Vector W;
	Vector_Sub(V,U,&W);
	return Vector_Length(&W);
}


// Puts Values AA...CC into Matrix M.
void Matrix_Form(Matrix M,float AA,float AB,float AC,float BA,float BB,float BC,float CA,float CB,float CC)
{
	M[0][0]=AA;
	M[0][1]=AB;
	M[0][2]=AC;
	M[1][0]=BA;
	M[1][1]=BB;
	M[1][2]=BC;
	M[2][0]=CA;
	M[2][1]=CB;
	M[2][2]=CC;
}

// Copies contents of Matrix B into A. Trashes previous value of A.
void Matrix_Copy(Matrix A,Matrix B)
{
	memcpy(A,B,sizeof(Matrix));
}

// Scales Matrix M by S.
void Matrix_Scale(Matrix M,float S)
{
	M[0][0]*=S; M[0][1]*=S; M[0][2]*=S;
	M[1][0]*=S; M[1][1]*=S; M[1][2]*=S;
	M[2][0]*=S; M[2][1]*=S; M[2][2]*=S;
}

// Returns the Determinant of the Matrix A.
float Determinant(Matrix A)
{
	return(A[0][0]*(A[1][1]*A[2][2]-A[1][2]*A[2][1])
		-A[0][1]*(A[1][0]*A[2][2]-A[1][2]*A[2][0])
		+A[0][2]*(A[1][0]*A[2][1]-A[1][1]*A[2][0]));
}

// Transposes A.
void Matrix_Transpose(Matrix A)
{
	Matrix_Form(A,A[0][0],A[1][0],A[2][0],A[0][1],A[1][1],A[2][1],A[0][2],A[1][2],A[2][2]);
}

// Normalizes a given matrix A.
void Matrix_Norm(Matrix A)
{
	Vector_Norm((Vector *)(&A[0][0]));
	Vector_Norm((Vector *)(&A[1][0]));
	Vector_Norm((Vector *)(&A[2][0]));
}

// Inverse Scales a given matrix A.
void Matrix_InvScale(Matrix A)
{
	Vector *V = (Vector *)A;
	
	Vector_SelfScale(V,1.0/Dot_Product(V,V)); V++;
	Vector_SelfScale(V,1.0/Dot_Product(V,V)); V++;
	Vector_SelfScale(V,1.0/Dot_Product(V,V));
}

// Re-Orthogonalizes A using it's Third Row as a base.
void Orthogonalize(Matrix A)
{
	Vector *W = (Vector *)A;
	Vector_Norm(W+2);
	Cross_Product(W+1,W+2,W);
	Vector_Norm(W);
	Cross_Product(W+2,W,W+1);
	Vector_Norm(W+1);
}

// Multiplies Matrix M by Vector U to Vector V. (V:=MxU)
/*void MatrixXVector(Matrix M,Vector *U,Vector *V)
{
V->x = M[0][0]*U->x + M[0][1]*U->y + M[0][2]*U->z;
V->y = M[1][0]*U->x + M[1][1]*U->y + M[1][2]*U->z;
V->z = M[2][0]*U->x + M[2][1]*U->y + M[2][2]*U->z;
} */

// Multiplies Matrix M transposed by Vector U to Vector V. (V:=MtxU)
void MatrixTXVector(Matrix M,Vector *U,Vector *V)
{
	V->x = M[0][0]*U->x + M[1][0]*U->y + M[2][0]*U->z;
	V->y = M[0][1]*U->x + M[1][1]*U->y + M[2][1]*U->z;
	V->z = M[0][2]*U->x + M[1][2]*U->y + M[2][2]*U->z;
}

// Multiplies Matrices A and B into C.

void MatrixXMatrix(Matrix A,Matrix B,Matrix C)
{
	C[0][0] = A[0][0]*B[0][0] + A[0][1]*B[1][0] + A[0][2]*B[2][0];
	C[0][1] = A[0][0]*B[0][1] + A[0][1]*B[1][1] + A[0][2]*B[2][1];
	C[0][2] = A[0][0]*B[0][2] + A[0][1]*B[1][2] + A[0][2]*B[2][2];
	
	C[1][0] = A[1][0]*B[0][0] + A[1][1]*B[1][0] + A[1][2]*B[2][0];
	C[1][1] = A[1][0]*B[0][1] + A[1][1]*B[1][1] + A[1][2]*B[2][1];
	C[1][2] = A[1][0]*B[0][2] + A[1][1]*B[1][2] + A[1][2]*B[2][2];
	
	C[2][0] = A[2][0]*B[0][0] + A[2][1]*B[1][0] + A[2][2]*B[2][0];
	C[2][1] = A[2][0]*B[0][1] + A[2][1]*B[1][1] + A[2][2]*B[2][1];
	C[2][2] = A[2][0]*B[0][2] + A[2][1]*B[1][2] + A[2][2]*B[2][2];
}

// Multiplies A transposed by Matrix B into C.
void MatrixTXMatrix(Matrix A,Matrix B,Matrix C)
{
	C[0][0] = A[0][0]*B[0][0] + A[1][0]*B[1][0] + A[2][0]*B[2][0];
	C[0][1] = A[0][0]*B[0][1] + A[1][0]*B[1][1] + A[2][0]*B[2][1];
	C[0][2] = A[0][0]*B[0][2] + A[1][0]*B[1][2] + A[2][0]*B[2][2];
	
	C[1][0] = A[0][1]*B[0][0] + A[1][1]*B[1][0] + A[2][1]*B[2][0];
	C[1][1] = A[0][1]*B[0][1] + A[1][1]*B[1][1] + A[2][1]*B[2][1];
	C[1][2] = A[0][1]*B[0][2] + A[1][1]*B[1][2] + A[2][1]*B[2][2];
	
	C[2][0] = A[0][2]*B[0][0] + A[1][2]*B[1][0] + A[2][2]*B[2][0];
	C[2][1] = A[0][2]*B[0][1] + A[1][2]*B[1][1] + A[2][2]*B[2][1];
	C[2][2] = A[0][2]*B[0][2] + A[1][2]*B[1][2] + A[2][2]*B[2][2];
}

// Returns identity matrix
void Matrix_Identity(Matrix M)
{
	Matrix_Form(M,1,0,0,0,1,0,0,0,1);
}

void Euler_Angles(Matrix Mat,float RX,float RY,float RZ)
{
	float C1 = cos(RX),S1 = sin(RX),
        C2 = cos(RY),S2 = sin(RY),
        C3 = cos(RZ),S3 = sin(RZ);
	
	float S1C2 = S1*C2;
	float S2C3 = S2*C3;
	Mat[0][0]=C2*C3+S1*S2*S3;
	Mat[0][1]=C1*S3;
	Mat[0][2]=S1C2*S3-S2C3;
	Mat[1][0]=S1*S2C3-C2*S3;
	Mat[1][1]=C1*C3;
	Mat[1][2]=S2*S3+S1C2*C3;
	Mat[2][0]=C1*S2;
	Mat[2][1]=-S1;
	Mat[2][2]=C1*C2;
}

// Rotates Matrix Mat over angle axes RX on X,RY on Y and RZ on Z.
void Matrix_Rotation(Matrix Mat,float RX,float RY,float RZ)
{
	Matrix T,M;
	Euler_Angles(T,RX,RY,RZ);
	MatrixXMatrix(T,Mat,M);
	memcpy(Mat,M,sizeof(Matrix));
}

// Computes Surface for Triangle defined by (A,B,C)
float Tri_Surface(Vector *A,Vector *B,Vector *C)
{
	Vector U,V,W;
	Vector_Sub(B,A,&U);
	Vector_Sub(C,A,&V);
	Cross_Product(&U,&V,&W);
	return Vector_Length(&W)*0.5;
}

// the Volume of a TetraHedron is given in the formula
//               |Ax  Ay  Az  1|
// V = (1/6)*|det|Bx  By  Bz  1||
//               |Cx  Cy  Cz  1|
//               |Dx  Dy  Dz  1|
// The sign of the volume stands for the formation order of the vertices
// and is not ignored here (for a reason).
float TetraHedron_Volume(Vector *A,Vector *B,Vector *C,Vector *D)
{
	Matrix M;
	float Det;
	Matrix_Form(M,B->x,C->x,D->x,
		B->y,C->y,D->y,
		B->z,C->z,D->z);
	Det = -Determinant(M);
	Matrix_Form(M,A->x,C->x,D->x,
								A->y,C->y,D->y,
								A->z,C->z,D->z);
	Det += Determinant(M);
	Matrix_Form(M,A->x,B->x,D->x,
		A->y,B->y,D->y,
		A->z,B->z,D->z);
	Det -= Determinant(M);
	Matrix_Form(M,A->x,B->x,C->x,
		A->y,B->y,C->y,
		A->z,B->z,C->z);
	Det += Determinant(M);
	return Det/6.0;
}

// an ad-hoc algo that will not prolly work, should calculate
// the volume under a triangle (=double integral under its plane on
// poly's limits)
// it doesn't work properly, probably because of sign accumulation error.
// I'll reconstruct it to handle signs implicitly and not as it is now,
// using vector order (esp. sign of the tetrahedron volume, and the
// z-order of the A,B and C vectors. the result will be multiplied by
// sgn(n->z) of the face.
float Trapped_Volume(Vector *A,Vector *B,Vector *C)
{
	Vector T,T2,*S;
	Vector U,V,W;
	
	// Prism Volume (surface x height)
	float Prism = 0.5*A->z*(A->y*(B->x-C->x)+B->y*(C->x-A->x)+C->y*(A->x-B->x));
	
	// Quad-Pyramid Volume
	U.z = 0;
	V.z = B->z-A->z;
	W.z = C->z-A->z;
	
	U.x = A->x;
	U.y = A->y;
	
	V.x = T.x = B->x;
	V.y = T.y = B->y;
	T.z = 0;
	W.x = T2.x = C->x;
	W.y = T2.y = C->y;
	T2.z = 0;
	
	float V1 = TetraHedron_Volume(&U,&W,&V,&T2);
	float V2 = TetraHedron_Volume(&U,&T2,&V,&T);
	
	return Prism+V1+V2;
}

////  4D Linear and/or Homogenious Coordinate space Algebra
void Vector4_Form(Vector4 *V,float X,float Y,float Z,float T)
{
	V->x = X;
	V->y = Y;
	V->z = Z;
	V->T = T;
}

void Vector4_Zero(Vector4 *V)
{
	//memset(V,0,sizeof(Vector4));
	V->x = V->y = V->z = V->T = 0;
}

void Vector4_Copy(Vector4 *V,Vector4 *U)
{
	//  memcpy(V,U,sizeof(Vector4));
	V->x = U->x;
	V->y = U->y;
	V->z = U->z;
	V->T = U->T;
}

void Vector4_WNorm(Vector4 *V)
{
	float F = 1.0/V->T;
	V->x *= F;
	V->y *= F;
	V->z *= F;
	V->T = 1.0;
}

void Matrix4_Form(Matrix4 M,float AA,float AB,float AC,float AD,float BA,float BB,float BC,float BD,float CA,float CB,float CC,float CD,float DA,float DB,float DC,float DD)
{
	M[0][0] = AA; M[0][1] = AB; M[0][2] = AC; M[0][3] = AD;
	M[1][0] = BA; M[1][1] = BB; M[1][2] = BC; M[1][3] = BD;
	M[2][0] = CA; M[2][1] = CB; M[2][2] = CC; M[2][3] = CD;
	M[3][0] = DA; M[3][1] = DB; M[3][2] = DC; M[3][3] = DD;
}

void Matrix4_Copy(Matrix4 A,Matrix4 B)
{
	memcpy(A,B,sizeof(Matrix4));
}

void Matrix4_Identity(Matrix4 M)
{
	Matrix4_Form(M,1,0,0,0,
		0,1,0,0,
		0,0,1,0,
		0,0,0,1);
}

void Matrix4_TransposeOut(Matrix4 A,Matrix4 B)
{
	Matrix4_Form(B,A[0][0],A[1][0],A[2][0],A[3][0],
		A[0][1],A[1][1],A[2][1],A[3][1],
		A[0][2],A[1][2],A[2][2],A[3][2],
		A[0][3],A[1][3],A[2][3],A[3][3]);
}


void Matrix4_Transpose(Matrix4 A)
{
	Matrix4_TransposeOut(A,A);
}


void MatrixXVector4(Matrix4 M,Vector4 *U,Vector4 *V)
{
	V->x = M[0][0] * U->x + M[0][1] * U->y + M[0][2] * U->z + M[0][3] * U->T;
	V->y = M[1][0] * U->x + M[1][1] * U->y + M[1][2] * U->z + M[1][3] * U->T;
	V->z = M[2][0] * U->x + M[2][1] * U->y + M[2][2] * U->z + M[2][3] * U->T;
	V->T = M[3][0] * U->x + M[3][1] * U->y + M[3][2] * U->z + M[3][3] * U->T;
}

void MatrixTXVector4(Matrix4 M,Vector4 *U,Vector4 *V)
{
	V->x = M[0][0] * U->x + M[1][0] * U->y + M[2][0] * U->z + M[3][0] * U->T;
	V->y = M[0][1] * U->x + M[1][1] * U->y + M[2][1] * U->z + M[3][1] * U->T;
	V->z = M[0][2] * U->x + M[1][2] * U->y + M[2][2] * U->z + M[3][2] * U->T;
	V->T = M[0][3] * U->x + M[1][3] * U->y + M[2][3] * U->z + M[3][3] * U->T;
}

void MatrixXMatrix4(Matrix4 A,Matrix4 B,Matrix4 C)
{
	Vector4 *AV = (Vector4 *)A;
	Vector4 *CV = (Vector4 *)C;
	MatrixTXVector4(B,AV  ,CV  );
	MatrixTXVector4(B,AV+1,CV+1);
	MatrixTXVector4(B,AV+2,CV+2);
	MatrixTXVector4(B,AV+3,CV+3);
}

void MatrixTXMatrix4(Matrix4 A,Matrix4 B,Matrix4 C)
{
	Matrix4 AT;
	Matrix4_TransposeOut(A,AT);
	MatrixXMatrix4(AT,B,C);
}

void Matrix_Expand3to4(Matrix A,Matrix4 A4)
{
	Matrix4_Form(A4,A[0][0],A[0][1],A[0][2],0,
		A[1][0],A[1][1],A[1][2],0,
		A[2][0],A[2][1],A[2][2],0,
		0   ,   0   ,   0   ,1);
}

// Generates a Homogenius space Projection matrix,given the FOV,
// and Near/Far Z-Planes. if FZP = inf,R reduces to S.

//       [C 0 0 0  ][x]   [Cx]                     [Cx/Sz] = [Cot*x/z]
//       [0 C 0 0  ][y]   [Cy]                     [Cy/Sz] = [Cot*y/z]
//  Pv = [0 0 R -NR][z] = [R*z-NR] , ==> Hnorm ==> [(Rz-NR)/Sz] = [1/(1-N/F)-N/(z*(1-N/F))]
//       [0 0 S 0  ][1]   [Sz]                     [1] meaningless

void Projection_Matrix(Matrix4 P,float FOV,float NZP,float FZP)
{
	float FOV2 = FOV*0.5;
	float S = sin(FOV2);
	float C = cos(FOV2);
	float R = S/(1-NZP/FZP);
	Matrix4_Form(P,C,0,0,0,
		0,C,0,0,
		0,0,R,-NZP*R,
		0,0,S,0);
}

// Forms a Complex number.
void Complex_Form(Complex *Z,float X,float Y)
{
	Z->x=X;
	Z->y=Y;
}

// Puts the adjunct to Complex Z into W.
void Complex_Adj(Complex *Z,Complex *W)
{
	W->x=Z->x;
	W->y=-Z->y;
}

// Converts Complex to Polar point
void Complex2AngLen(Complex *Z,float *Ang,float *Len)
{
	*Len=sqrt(Z->x*Z->x + Z->y*Z->y);
	*Ang=acos(Z->x/(*Len));
	if (Z->y<0) *Ang=2*PI-(*Ang);
}

// Converts Polar point to Complex number.
void AngLen2Complex(Complex *Z,float Ang,float Len)
{
	Z->x = Len * cos(Ang);
	Z->y = Len * sin(Ang);
}

// Add two Complex Numbers Z and W into R.
void Complex_Add(Complex *Z,Complex *W,Complex *R)
{
	R->x = Z->x+W->x;
	R->y = Z->y+W->y;
}

// Subtracts Complex W from Complex Z into R.
void Complex_Sub(Complex *Z,Complex *W,Complex *R)
{
	R->x = Z->x-W->x;
	R->y = Z->y-W->y;
}

// Multiplies Complex Numbers Z and W into R.
void Complex_Mul(Complex *Z,Complex *W,Complex *R)
{
	R->x = Z->x*W->x-Z->y*W->y;
	R->y = Z->x*W->y+Z->y*W->x;
}

// Divides Complex Z by Complex W into R.
void Complex_Div(Complex *Z,Complex *W,Complex *R)
{
	float F = 1/(W->x*W->x+W->y*W->y);
	R->x=(Z->x*W->x+Z->y*W->y)*F;
	R->y=(Z->y*W->x-Z->x*W->y)*F;
}

// Forms a Quaternion
void Quaternion_Form(Quaternion *Q,float X,float Y,float Z,float W)
{
	Q->x=X;
	Q->y=Y;
	Q->z=Z;
	Q->W=W;
}

void Quaternion_Copy(Quaternion *Q,Quaternion *S)
{
	memcpy(Q,S,sizeof(Quaternion));
}

// Scale quaternion by value into itself
void Quaternion_SelfScale( Quaternion *Q,float S)
{
	Q->x *= S;
	Q->y *= S;
	Q->z *= S;
	Q->W *= S;
}

// Scale quaternion by value.
void Quaternion_Scale( Quaternion *Q,float S,Quaternion *P)
{
	P->W = Q->W*S;
	P->x = Q->x*S;
	P->y = Q->y*S;
	P->z = Q->z*S;
}

// Returns The Length of Quaternion Q.
float Quaternion_Length(Quaternion *Q)
{
	return Q->x * Q->x + Q->y * Q->y + Q->z * Q->z + Q->W * Q->W;
}

// Normalizes the Quaternion Q.
void Normalize_Quaternion(Quaternion *Q)
{
	float Length, C;
	
	Length = Quaternion_Length(Q);
	
	if (Length > EPSILON)
	{
		C = 1.0/Length;
		Q->x *= C;
		Q->y *= C;
		Q->z *= C;
		Q->W *= C;
	}
	else Quaternion_Form(Q,0,0,0,1);
}

// Normalizes Q into Dest.
void Quaternion_Unit(Quaternion *Q,Quaternion *Dest)
{
	float S;
	S = 1.0/Quaternion_Length(Q);
	Quaternion_Scale( Q,S,Dest );
}


// Negates Quaternion Q, like, DUH.
// still represents the same effective rotation.
void Quaternion_Negate(Quaternion *Q)
{
	float D;
	D = 1.0/Quaternion_Length(Q);
	Q->W *= -D;
	Q->x *= -D;
	Q->y *= -D;
	Q->z *= -D;
}

// Creates a Logical Quaternion out of an axis. The length of the
// axis will determine the Quaternion angle, and may be up to PI/2.
// the longer the axis is, the lesser will be the angle.
// 0 axis yields the identity quaternion.
void Quaternion_Exponent(Quaternion *Q, Quaternion *Dest)
{
	float D,D1;
	D = sqrt( Q->x*Q->x + Q->y*Q->y + Q->z*Q->z );
	if (D > 0) D1 = sin(D)/D; else D1 = 1;
	Dest->W = cos(D);
	Dest->x = Q->x*D1;
	Dest->y = Q->y*D1;
	Dest->z = Q->z*D1;
}

// Negates the Quaternion_Exponent operation, and therefore
// Reconverts the Quaternion into an Axis which can be modified linearly.
void Quaternion_Logarithm(Quaternion *Q, Quaternion *Dest)
{
	float D;
	D = sqrt( Q->x*Q->x + Q->y*Q->y + Q->z*Q->z );
	if (Q->W != 0.0) D = atan(D/Q->W); else D = PI_D2;
	Dest->W = 0.0;
	Dest->x = Q->x*D;
	Dest->y = Q->y*D;
	Dest->z = Q->z*D;
}

//Computes the Multiplicative Inverse of Q into Dest.
void Quaternion_Inverse( Quaternion *Q, Quaternion *Dest )
{
	float D;
	D = Quaternion_Length(Q);
	if (D != 0) D = 1.0/D; else D = 1;
	Dest->W =  Q->W * D;
	Dest->x = -Q->x * D;
	Dest->y = -Q->y * D;
	Dest->z = -Q->z * D;
}

// Returns Dot product of Normalized quternions <Q1,Q2>
float Quaternion_Dot_Product( Quaternion *Q1, Quaternion *Q2 )
{
	return Q1->x*Q2->x + Q1->y*Q2->y + Q1->z*Q2->z + Q1->W*Q2->W;
}

// Converts Angle axis Representation to Quaternionic Representation (Q->P)
void AngleAxis2Quaternion(Quaternion *Q,Quaternion *P)
{
	float div2, sindiv2;
	
	div2 = Q->W*0.5;
	
	sindiv2 = sin(div2);
	
	P->x = Q->x * sindiv2;
	P->y = Q->y * sindiv2;
	P->z = Q->z * sindiv2;
	P->W = cos(div2);
}

// Converts Quaternion Q into Angle Axis form
void Quaternion2AngleAxis(Quaternion *Q, Quaternion *A)
{
	int I;
	float HalfAng,S;
	Quaternion Qn;
	
	Quaternion_Unit( Q,&Qn );
	HalfAng = acos( Qn.W );
	A->W = 2.0*HalfAng;
	S = 1.0/sin(HalfAng);
	A->x = Qn.x*S;
	A->y = Qn.y*S;
	A->z = Qn.z*S;
}

void Quaternion_Sub(Quaternion *Q,Quaternion *P,Quaternion *R)
{
	R->W = Q->W - P->W;
	R->x = Q->x - P->x;
	R->y = Q->y - P->y;
	R->z = Q->z - P->z;
}

// Computes a Linear Combination between P and Q,as if they were 4D-Vectors.
void Quaternion_Linear(Quaternion *Q,float A,Quaternion *P,float B,Quaternion *C)
{
	C->W = Q->W*A + P->W*B;
	C->x = Q->x*A + P->x*B;
	C->y = Q->y*A + P->y*B;
	C->z = Q->z*A + P->z*B;
}

// Multiplies Quaternionic Numbers A and B. Stores result in C.
// [S1,V1]*[S2,V2] = [S1*S2-<V1,V2>,S1*V2+S2*V1+V1xV2],
// B is over A in rotation space
void Quaternion_Mul(Quaternion *A,Quaternion *B,Quaternion *C) // C=A*B
{
	Quaternion D;
	
	D.W = A->W*B->W - A->x*B->x - A->y*B->y - A->z*B->z;
	D.x = A->W*B->x + A->x*B->W + A->y*B->z - A->z*B->y;
	D.y = A->W*B->y + A->y*B->W + A->z*B->x - A->x*B->z;
	D.z = A->W*B->z + A->z*B->W + A->x*B->y - A->y*B->x;
	
	// Prevent Overlapping,in case C=A or B...
	memcpy(C,&D,sizeof(Quaternion));
}

// Calculate logarithm of the relative rotation from P to Q into Dest
void Quaternion_Ln_Dif(Quaternion *P, Quaternion *Q, Quaternion *Dest)
{
	Quaternion Inv,Dif;
	float D,D1;
	float S;
	
	Quaternion_Inverse(P,&Inv );         // inv = p^-1;
	Quaternion_Mul( Q,&Inv,&Dif ); // dif = (p^-1)*q
	
	//  Dest = ln((p^-1)*q).
	D = sqrt( Dif.x*Dif.x + Dif.y*Dif.y + Dif.z*Dif.z );
	S = Quaternion_Dot_Product(P,Q);
	if (S != 0) D1 = atan(D/S); else D1 = PI_D2;
	if (D != 0) D1 /= D;
	
	Dest->W = 0;
	
	Dest->x = Dif.x*D1;
	Dest->y = Dif.y*D1;
	Dest->z = Dif.z*D1;
}


// Converts Represented Rotation of Quaternion Q to Matrix M.
void Convert_Quat2Mat(Quaternion *Q,Matrix M)
{
	float S,XS,YS,ZS,WX,WY,WZ,XX,XY,XZ,YY,YZ,ZZ,Den;
	
	Den = Quaternion_Length(Q);
	if (Den==0.0) S = 1.0; else S = 2.0/Den;
	
	XS = Q->x * S;   YS = Q->y * S;  ZS = Q->z * S;
	WX = Q->W * XS;  WY = Q->W * YS; WZ = Q->W * ZS;
	XX = Q->x * XS;  XY = Q->x * YS; XZ = Q->x * ZS;
	YY = Q->y * YS;  YZ = Q->y * ZS; ZZ = Q->z * ZS;
	
	Matrix_Form(M,1.0-YY-ZZ,XY-WZ,XZ+WY,
		XY+WZ,1.0-XX-ZZ,YZ-WX,
		XZ-WY,YZ+WX,1.0-XX-YY);
}


// Converts Matrix M to Representing Rotation Quaternion Q.
void Convert_Mat2Quat( Matrix M, Quaternion *Q )
{
	float S,V;
	int I,J,K;
	
	V = M[0][0] + M[1][1] + M[2][2];
	if (V > 0.0) {
		S = sqrt(V + 1.0);
		Q->W = 0.5 * S;
		S = 0.5 / S;
		Q->x = (M[2][1] - M[1][2]) * S;
		Q->y = (M[0][2] - M[2][0]) * S;
		Q->z = (M[1][0] - M[0][1]) * S;
	}   else {
		if (M[1][1] > M[0][0])  {
			if (M[2][2] > M[1][1])  {
				S = sqrt( (M[2][2] - (M[0][0] + M[1][1])) + 1.0 );
				Q->z = S * 0.5;
				if (S != 0.0)   S = 0.5/S;
				Q->W = (M[1][0] - M[0][1]) * S;
				Q->x = (M[0][2] + M[2][0]) * S;
				Q->y = (M[1][2] + M[2][1]) * S;
			}   else    {
				S = sqrt( (M[1][1] - (M[2][2] + M[0][0])) + 1.0 );
				Q->y = S * 0.5;
				if (S != 0.0)   S = 0.5/S;
				Q->W = (M[0][2] - M[2][0]) * S;
				Q->z = (M[2][1] + M[1][2]) * S;
				Q->x = (M[0][1] + M[1][0]) * S;
			}
		}   else
			if (M[2][2] > M[0][0])  {
				S = sqrt( (M[2][2] - (M[0][0] + M[1][1])) + 1.0 );
				Q->z = S * 0.5;
				if (S != 0.0)   S = 0.5/S;
				Q->W = (M[1][0] - M[0][1]) * S;
				Q->x = (M[0][2] + M[2][0]) * S;
				Q->y = (M[1][2] + M[2][1]) * S;
			}   else    {
				S = sqrt( (M[0][0] - (M[1][1] + M[2][2])) + 1.0 );
				Q->x = S * 0.5;
				if (S != 0.0)   S = 0.5/S;
				Q->W = (M[2][1] - M[1][2]) * S;
				Q->y = (M[1][0] + M[0][1]) * S;
				Q->z = (M[2][0] + M[0][2]) * S;
			}
	}
}

void Quaternion_SLERP( Quaternion *A,Quaternion *B, Quaternion *Dest, float Time,float Spin )
{
	double K1,K2;                   // interpolation coefficions.
	double Angle;                   // Angle between A and B
	double AngleSpin;           // Angle between A and B plus Spin.
	double sin_a, cos_a;    // sine, cosine of Angle
	int FlipK2;                     // use negation of K2.
	
	cos_a = Quaternion_Dot_Product( A,B );
	if (cos_a < 0.0) cos_a = -cos_a, FlipK2 = -1;  else FlipK2 = 1;
	
	if ((1.0 - cos_a) < EPSILON) {
		K1 = 1.0 - Time;
		K2 = Time;
	} else {
		Angle = acos(cos_a);
		sin_a = sin(Angle);
		AngleSpin = Angle + Spin*PI;
		K1 = sin( Angle - Time*AngleSpin ) / sin_a;
		K2 = sin( Time*AngleSpin ) / sin_a;
	}
	K2 *= FlipK2;
	
	Dest->x = K1*A->x + K2*B->x;
	Dest->y = K1*A->y + K2*B->y;
	Dest->z = K1*A->z + K2*B->z;
	Dest->W = K1*A->W + K2*B->W;
}

void Quaternion_SLERP_V2( Quaternion *A,Quaternion *B, Quaternion *Dest, float Time,float Spin )
{
	double K1,K2;                   // interpolation coefficions.
	double Angle;                   // Angle between A and B
	double AngleSpin;           // Angle between A and B plus Spin.
	double sin_a, cos_a;    // sine, cosine of Angle
	
	cos_a = Quaternion_Dot_Product( A,B );
	
	if (1.0 - fabs(cos_a) < EPSILON) {
		K1 = 1.0 - Time;
		K2 = Time;
	} else {
		Angle = acos(cos_a);
		sin_a = sin(Angle);
		AngleSpin = Angle + Spin*PI;
		K1 = sin( Angle - Time*AngleSpin ) / sin_a;
		K2 = sin( Time*AngleSpin ) / sin_a;
	}
	
	Dest->x = K1*A->x + K2*B->x;
	Dest->y = K1*A->y + K2*B->y;
	Dest->z = K1*A->z + K2*B->z;
	Dest->W = K1*A->W + K2*B->W;
}

void Spline_SetKey_1D(Spline *S,float X, float Frame,float SpF[5])
{
	SplineKey *SK=S->Keys+S->CurKey;
	SK->Frame = Frame;
	SK->Pos.x = X;
	SK->Tens = SpF[0];
	SK->Cont = SpF[1];
	SK->Bias = SpF[2];
	SK->EaseTo = SpF[3];
	SK->EaseFrom = SpF[4];
	SK->DS.x = 0;
	SK->DD.x = 0;
	S->CurKey++;
}

void Spline_SetKey_3D(Spline *S,Vector *V, float Frame,float SpF[5])
{
	SplineKey *SK=S->Keys+S->CurKey;
	SK->Frame = Frame;
	SK->Pos.x = V->x;
	SK->Pos.y = V->y;
	SK->Pos.z = V->z;
	SK->Tens = SpF[0];
	SK->Cont = SpF[1];
	SK->Bias = SpF[2];
	SK->EaseTo = SpF[3];
	SK->EaseFrom = SpF[4];
	SK->DS.x = 0; SK->DS.y = 0; SK->DS.z = 0;
	SK->DD.x = 0; SK->DD.y = 0; SK->DD.z = 0;
	S->CurKey++;
}
void Spline_SetKey_4D(Spline *S,Quaternion *Q,float Frame,float SpF[5])
{
	float s,w;
	Quaternion P;
	Matrix M;
	SplineKey *SK=S->Keys+S->CurKey;
	
	// fetch previous quat.
	if (!S->CurKey) Quaternion_Form(&P,0,0,0,1); else Quaternion_Copy(&P,&((SK-1)->Pos));
	SK->Frame = Frame;
	SK->AA.x = Q->x;
	SK->AA.y = Q->y;
	SK->AA.z = Q->z;
	SK->AA.W = Q->W;
	AngleAxis2Quaternion(&SK->AA,Q);
	Normalize_Quaternion(Q);
	Quaternion_Mul(Q,&P,&SK->Pos);
	SK->Tens = SpF[0];
	SK->Cont = SpF[1];
	SK->Bias = SpF[2];
	SK->EaseTo = SpF[3];
	SK->EaseFrom = SpF[4];
	// Preset Derivatives
	SK->DS.x = 0; SK->DS.y = 0; SK->DS.z = 0; SK->DS.W = 0;
	SK->DD.x = 0; SK->DD.y = 0; SK->DD.z = 0; SK->DD.W = 0;
	S->CurKey++;
}

// This will soon contain a Superior version of the Bartels-Kochanek Spline
// Unit.

// Linear Interpolation between Quaternions.
void LERP(Quaternion *Q,Quaternion *P,float T,Quaternion *R)
{
	float S=1-T;
	R->W = Q->W*S + P->W*T;
	R->x = Q->x*S + P->x*T;
	R->y = Q->y*S + P->y*T;
	R->z = Q->z*S + P->z*T;
}

// Spherical Linear Interpolation between Quaternions.
void SLERP(Quaternion *Q,Quaternion *P,float T,Quaternion *R)
{
	float A = acos(Quaternion_Dot_Product(Q,P));
	if (A>1.0-EPSILON) {LERP(Q,P,T,R); return;}
	float rsA = 1.0/sin(A);
	float TA = T*A;
	float K = sin(A-TA)*rsA;
	float L = sin(TA)*rsA;
	R->W = Q->W*K + P->W*L;
	R->x = Q->x*K + P->x*L;
	R->y = Q->y*K + P->y*L;
	R->z = Q->z*K + P->z*L;
}

// hmm...i wonder.
void SLERP2(Quaternion *Q,Quaternion *P,float T,Quaternion *R)
{
	Quaternion LQ,LP,LL;
	Quaternion_Logarithm(Q,&LQ);
	Quaternion_Logarithm(P,&LP);
	LERP(&LQ,&LP,T,&LL);
	Quaternion_Exponent(&LL,R);
}


void SQUAD(Quaternion *Q,Quaternion *A,Quaternion *B,Quaternion *P,float T,Quaternion *R)
{
	Quaternion QA,QT;
	SLERP(Q,P,T,&QA);
	SLERP(A,B,T,&QT);
	SLERP(&QA,&QT,2*T*(1-T),R);
}

// Bartels-Kochanek Spline Ease function.
float BKS_Ease(float T,float A,float B)
{
	float k;
	float S = A+B;
	if (S == 0.0) return T;
	if (S > 1.0)
	{
		k = 1.0/S;
		A *= k;
		B *= k;
		S = 1.0;
	} else S=1.0/(2.0-S);
	if (T < A) return T*T*S/A;
	if (T < 1.0-B) return S*(2*T - A);
	T = 1.0-T;
	return 1.0-S*T*T/B;
}

/////////////////////////////////////////
/////// QUATERNIONIC SPLINE UNIT ////////
/////////////////////////////////////////

// This version of SLERP uses a Quaternion and a Relative Angle/Axis for
// further rotation.
void SLERP_Axis(Quaternion *Q,Quaternion *AA,float T,Quaternion *R)
{
	Quaternion P,AA2;
	Quaternion_Copy(&AA2,AA); AA2.W*=T;
	AngleAxis2Quaternion(&AA2,&P);
	//  Quaternion_Normalize(&P);
	Quaternion_Mul(&P,Q,R);
}

void Spline_CompDeriv_Bezier(SplineKey *P,SplineKey *C,SplineKey *N)
{
	Quaternion G1,G2,G3;
	// Compute Tangent Offsets
	SLERP(&C->Pos,&P->Pos,-(1.0+C->Bias)/3.0,&G1);
	SLERP(&C->Pos,&N->Pos, (1.0-C->Bias)/3.0,&G2);
	//  SLERP_Axis(&P->Pos,&C->AA,1.0-(1.0+C->Bias)/3.0,&G1);
	//  SLERP_Axis(&C->Pos,&N->AA,    (1.0-C->Bias)/3.0,&G2);
	
	// Compute Incoming Derivative
	SLERP(&G1,&G2,0.5+0.5*C->Cont,&G3);
	SLERP(&C->Pos,&G3,C->Tens-1.0,&C->DS);
	
	// Compute Outcoming Derivative
	SLERP(&G1,&G2,0.5-0.5*C->Cont,&G3);
	SLERP(&C->Pos,&G3,1.0-C->Tens,&C->DD);
	
}

void Spline_CompDerivFirst_Bezier(SplineKey *C,SplineKey *N)
{
	SLERP(&C->Pos,&N->Pos,(1.0-C->Tens)*(1.0+C->Cont*C->Bias)/3.0,&C->DD);
	//  SLERP_Axis(&C->Pos,&N->AA,(1.0-C->Tens)*(1.0+C->Cont*C->Bias)/3.0,&C->DD);
}

void Spline_CompDerivLast_Bezier(SplineKey *P,SplineKey *C)
{
	SLERP(&C->Pos,&P->Pos,(1.0-C->Tens)*(1.0-C->Cont*C->Bias)/3.0,&C->DS);
	//  SLERP_Axis(&P->Pos,&C->AA,1.0-(1.0-C->Tens)*(1.0+C->Cont*C->Bias)/3.0,&C->DS);
}

void Spline_Init_Bezier(Spline *S)
{
	int32_t I;
	
	S->CurKey = 0;
	if (S->NumKeys < 2) return;
	if (S->NumKeys > 2)
	{
		
		for(I=1;I<S->NumKeys;I++)
			Spline_CompDeriv_Bezier(S->Keys+I-1,S->Keys+I,S->Keys+I+1);
		
		if (S->Flags&TrackLOOP)
		{
			Spline_CompDeriv_Bezier(S->Keys+(S->NumKeys-2),S->Keys,S->Keys+1);
			Spline_CompDeriv_Bezier(S->Keys+(S->NumKeys-2),S->Keys+(S->NumKeys-1),S->Keys+1);
		} else {
			Spline_CompDerivFirst_Bezier(S->Keys,S->Keys+1);
			Spline_CompDerivLast_Bezier(S->Keys+(S->NumKeys-2),S->Keys+(S->NumKeys-1)); //NEAT.
		}
	} else {
		Spline_CompDerivFirst_Bezier(S->Keys,S->Keys+1);
		Spline_CompDerivLast_Bezier(S->Keys,S->Keys+1);
	}
}


void Spline_Subdivide_Bezier(Spline *S,float Frame,Quaternion *Out)
{
	Quaternion Q0,Q1,Q2;
	float t,n;
	SplineKey *SK,*NK;
	
	if (S->Flags&TrackREPEAT)
	{
		SK=S->Keys+S->NumKeys-1;
		Frame = S->Keys->Frame + fmod( Frame,SK->Frame - S->Keys->Frame );
		if (Frame<S->Keys[S->CurKey].Frame) S->CurKey=0;
	}
	
	if (S->NumKeys == 1) // First and ONLY key
	{
		Out->x = S->Keys->Pos.x;
		Out->y = S->Keys->Pos.y;
		Out->z = S->Keys->Pos.z;
		Out->W = S->Keys->Pos.W;
	}
	else
	{
		while (Frame > S->Keys[S->CurKey+1].Frame && S->CurKey<S->NumKeys-1) S->CurKey++;
		
		SK=S->Keys+S->CurKey; NK=SK+1;
		if (Frame > NK->Frame)
		{
			Out->x = NK->Pos.x;
			Out->y = NK->Pos.y;
			Out->z = NK->Pos.z;
			Out->W = NK->Pos.W;
			return;
		}
		if (Frame == SK->Frame) {
			t = 0;
		} else {
			t = (Frame - SK->Frame) / (NK->Frame - SK->Frame);
			t = BKS_Ease(t, SK->EaseFrom, NK->EaseTo);
		}
		if (NK->AA.W>2.0*PI)
		{
			SLERP_Axis(&SK->Pos,&NK->AA,t,Out);
			return;
		}
		else
		{
			SLERP(&SK->Pos,&SK->DD,t,&Q0);
			SLERP(&SK->DD,&NK->DS,t,&Q1);
			SLERP(&NK->DS,&NK->Pos,t,&Q2);
			
			SLERP(&Q0,&Q1,t,&Q0);
			SLERP(&Q1,&Q2,t,&Q1);
			
			SLERP(&Q0,&Q1,t,Out);
		}
	}
}
//////////////////////////////////////

////////////////////////////////////////////////////
/// Bartels-Kochanek Vector Space optimized Unit ///
////////////////////////////////////////////////////
// Bartels-Kochanek Spline Deriviate Calculations.
void BKS_Deriviate( SplineKey *KeyP,SplineKey *Key,SplineKey *KeyN )
{
	float DSA,DSB,DDA,DDB,DSAdjust;
	float PF,F,NF;
	Quaternion U,V;
	float T = 1.0 - Key->Tens;
	float TD;
	
	PF = KeyP->Frame; F =  Key->Frame; NF = KeyN->Frame;
	if (PF > F) {F += PF; NF += PF;}
	if (F > NF) NF += F;
	
	DSAdjust = (F - PF)/(NF - PF);
	DSAdjust += fabs(Key->Cont)*(0.5-DSAdjust); // correction
	TD = T*DSAdjust;
	DSA = TD * (1.0 - Key->Cont) * (1.0 + Key->Bias);
	DSB = TD * (1.0 + Key->Cont) * (1.0 - Key->Bias);
	T -= TD;
	DDA = T * (1.0 + Key->Cont) * (1.0 + Key->Bias);
	DDB = T * (1.0 - Key->Cont) * (1.0 - Key->Bias);
	
	Quaternion_Sub(&Key->Pos,&KeyP->Pos,&U);
	Quaternion_Sub(&KeyN->Pos,&Key->Pos,&V);
	Quaternion_Linear(&U,DSA,&V,DSB,&Key->DS);
	Quaternion_Linear(&U,DDA,&V,DDB,&Key->DD);
}


// Bartels-Kochanek Spline Deriviate Calculations. (First Key: Outgoing)
void BKS_FirstDeriviate(SplineKey *Key,SplineKey *KeyN,SplineKey *KeyNN)
{
	float   f20,f10;
	float   K2;
	Quaternion I1,I2;
	
	f20 = KeyNN->Frame - Key->Frame;
	f10 = KeyN->Frame - Key->Frame;
	K2 = (1-Key->Tens)*(0.25 - f10/(2*f20)) - 0.25;
	
	Quaternion_Sub(&KeyN->Pos,&Key->Pos,&I1);
	Quaternion_Sub(&KeyNN->Pos,&Key->Pos,&I2);
	Quaternion_Linear(&I1,1.5,&I2,K2,&Key->DD);
}

// Bartels-Kochanek Spline Deriviate Calculations. (Last Key: Incoming)
void BKS_LastDeriviate(SplineKey *KeyPP,SplineKey *KeyP,SplineKey *Key)
{
	float f20,f10,K2;
	Quaternion I1,I2;
	
	f20 = Key->Frame - KeyPP->Frame;
	f10 = Key->Frame - KeyP->Frame;
	K2 = (1-Key->Tens)*(0.25 - f10/(2*f20))-0.25;
	
	Quaternion_Sub(&Key->Pos,&KeyP->Pos,&I1);
	Quaternion_Sub(&Key->Pos,&KeyPP->Pos,&I2);
	Quaternion_Linear(&I1,1.5,&I2,K2,&Key->DS);
}

// Bartels-Kochanek Spline Deriviate Calculations. (2 Keys, First key)
void BKS_FirstDeriviate2( SplineKey *Key,SplineKey *KeyN )
{
	float T = 1.0-Key->Tens;
	Quaternion_Linear(&KeyN->Pos,T,&Key->Pos,-T,&Key->DD);
}

// Bartels-Kochanek Spline Deriviate Calculations. (2 Keys, Last key)
void BKS_LastDeriviate2( SplineKey *KeyP,SplineKey *Key )
{
	float T = 1.0-Key->Tens;
	Quaternion_Linear(&Key->Pos,T,&KeyP->Pos,-T,&Key->DS);
}

// Quaternionic Deriviate.
void BKS_QuatDeriviate(SplineKey *KeyP,SplineKey *Key,SplineKey *KeyN)
{
	Quaternion RPC,RCN; // Relative Quaternions.
	Quaternion P,Q,R;
	float DSA,DSB,DDA,DDB,DSAdjust;
	float PF,F,NF;
	float T = 1.0 - Key->Tens;
	float TD;
	float AdjP,AdjN;
	float tm,cm,cp,bm,bp,tmcm,tmcp,c;
	float dt,fp,fn;
	
	
	if (KeyP)
	{
		if (Key->AA.W>PI_M2-EPSILON)
		{
			Quaternion_Copy(&Q,&Key->AA);
			Quaternion_Logarithm(&Q,&RPC);
		} else {
			Quaternion_Copy(&Q,&KeyP->Pos);
			if (Quaternion_Dot_Product(&Q,&Key->Pos)<0) Quaternion_Negate(&Q); // Short ARC
			Quaternion_Ln_Dif(&Q,&Key->Pos,&RPC);
		}
	}
	
	if(KeyN)
	{
		if (KeyN->AA.W>PI_M2-EPSILON) {
			Quaternion_Copy(&Q,&KeyN->AA);
			Quaternion_Logarithm(&Q,&RCN);
		} else {
			Quaternion_Copy(&Q,&KeyN->Pos);
			if (Quaternion_Dot_Product(&Q,&Key->Pos)<0) Quaternion_Negate(&Q);
			Quaternion_Ln_Dif(&Key->Pos,&Q,&RCN);
		}
	}
	
	if (!KeyP) Quaternion_Copy(&RPC,&RCN);
	if (!KeyN) Quaternion_Copy(&RCN,&RPC);
	
	// Linear Derivation.
	AdjP = AdjN = 1.0;
	if( KeyP && KeyN ) {
		AdjP = (Key->Frame-KeyP->Frame)/(KeyN->Frame-KeyP->Frame);
		AdjP += fabs(Key->Cont)*(0.5-AdjP);
		AdjN = 1.0-AdjP;
	}
	bm = 1.0 - Key->Bias;
	bp = 1.0 + Key->Bias;
	tmcm = T * (1.0 - Key->Cont);
	tmcp = T * (1.0 + Key->Cont);
	
	// offsets measure how far is it from RPC
	// incoming tangent is temporally reversed,
	// so we reverse blending rates
	DSA  = 1.0 - tmcm * bp * AdjP;
	DSB  = -tmcp * bm * AdjP;
	// offsets measure how far is it from RCN
	DDA  = tmcp * bp * AdjN;
	DDB  = tmcm * bm * AdjN - 1.0;
	
	// Exponention and Multiplication are used to convert Volume to
	// Spherical Relative Quaternion, and then to Absolute Orientation.
	Quaternion_Linear(&RPC,DSA,&RCN,DSB,&Q);
	Quaternion_SelfScale(&Q,0.5);
	Quaternion_Exponent(&Q,&P);
	Quaternion_Mul(&P,&Key->Pos,&Key->DS);
	
	Quaternion_Linear(&RPC,DDA,&RCN,DDB,&Q);
	Quaternion_SelfScale(&Q,0.5);
	Quaternion_Exponent(&Q,&P);
	Quaternion_Mul(&P,&Key->Pos,&Key->DD);
}

void Spline_Remove_Duplicates(Spline* S) {
		
	for (int i = 0; i != S->NumKeys; ++i) {
		if (S->Keys[i].Frame > 0) {
			return;
		}
	}
	S->NumKeys = 1;
}

// Initializes a 3D/Pos spline. Must always be executed before the spline can
// be correctly Interpolated.
void Spline_Init_3D(Spline *S)
{
	int32_t I;
	
	S->CurKey = 0;
	Spline_Remove_Duplicates(S);
	if (S->NumKeys < 2) return;
	if (S->NumKeys > 2)
	{
		
		for(I=1;I<S->NumKeys-1;I++)
			BKS_Deriviate(S->Keys+I-1,S->Keys+I,S->Keys+I+1);
		
		if (S->Flags&TrackLOOP)
		{
			BKS_Deriviate(S->Keys+(S->NumKeys-2),S->Keys,S->Keys+1);
			BKS_Deriviate(S->Keys+(S->NumKeys-2),S->Keys+(S->NumKeys-1),S->Keys+1);
		} else {
			BKS_FirstDeriviate(S->Keys,S->Keys+1,S->Keys+2);
			BKS_LastDeriviate(S->Keys+(S->NumKeys-3),S->Keys+(S->NumKeys-2),S->Keys+(S->NumKeys-1)); //NEAT.
		}
	} else {
		BKS_FirstDeriviate2(S->Keys,S->Keys+1);
		BKS_LastDeriviate2(S->Keys,S->Keys+1);
	}
}

// Initializes a 4D/Rotation spline. Must always be executed before the
// spline can be correctly Interpolated. (Doesn't support LOOPS)
void Spline_Init_4D(Spline *S)
{
	int32_t I;
	S->CurKey = 0;
	Spline_Remove_Duplicates(S);
	if (S->NumKeys < 2) return;
	if (S->NumKeys > 2)
	{
		for(I=1;I<S->NumKeys-1;I++)
			BKS_QuatDeriviate( S->Keys+I-1,S->Keys+I,S->Keys+I+1 );
		
		BKS_QuatDeriviate( NULL,S->Keys,S->Keys+1 );
		BKS_QuatDeriviate( S->Keys+(S->NumKeys-2),S->Keys+(S->NumKeys-1),NULL );
	}
}

void Spline_Calc_1D(Spline *S,float Frame, float *Out)
{
	int     n;
	float   j,t,t2,t3,dt3,tt2;
	//  float   x,y,z;
	float h[4];
	SplineKey *SK,*NK;
	
	if ((!S)||(!S->NumKeys)) {*Out = 0; return;}
	if (S->Flags&TrackREPEAT)
	{
		Frame = S->Keys->Frame + fmod( Frame,S->Keys[S->NumKeys-1].Frame - S->Keys->Frame );
		if (Frame<S->Keys[S->CurKey].Frame) S->CurKey=0;
	}
	
	if (S->NumKeys == 1) {*Out = S->Keys->Pos.x; return;}
	
	// support rewinding
	if (Frame < S->Keys[S->CurKey].Frame)
		S->CurKey = 0;
	
	if (Frame > S->Keys[S->CurKey+1].Frame && S->CurKey<S->NumKeys-2)
		S->CurKey++; // Proceed to next segment
	
	SK = S->Keys+S->CurKey;
	NK = SK+1;
	if (Frame > NK->Frame) {*Out = NK->Pos.x; return;}
	
	float step = NK->Frame - SK->Frame;
	if (step == 0.0) {
		t = 0.0;
	} else {
		t = (Frame - SK->Frame) / step;
	}
	t = BKS_Ease(t, SK->EaseFrom, NK->EaseTo);
	
	t2 = t*t;    // t2 = t^2;
	t3 = t2*t;   // t3 = t^3;
	dt3 = 2*t3;
	tt2 = 3*t2;
	h[0] = dt3 - tt2 + 1;
	h[1] = tt2 - dt3;
	h[2] = t3  - tt2 + t2 + t;
	h[3] = t3  - t2;
	
	*Out = (h[0]*SK->Pos.x)+(h[1]*NK->Pos.x)+(h[2]*SK->DD.x)+(h[3]*NK->DS.x);
}

void Spline_Calc_3D(Spline *S,float Frame, Vector *Out)
{
	int     n;
	float   j,t,t2,t3,tt2,dt3;
	float h[4];
	SplineKey *SK,*NK;
	
	if ((!S)&&(!S->NumKeys)) {Out->x = Out->y = Out->z = 0; return;}
	if (S->Flags&TrackREPEAT)
	{
		Frame = S->Keys->Frame + fmod( Frame,S->Keys[S->NumKeys-1].Frame - S->Keys->Frame );
		if (Frame<S->Keys[S->CurKey].Frame) S->CurKey=0;
	}
	
	if (S->NumKeys == 1) {memcpy(&Out->x,&S->Keys->Pos.x,sizeof(Vector)); return;}
	
	// support rewinding
	if (Frame < S->Keys[S->CurKey].Frame)
		S->CurKey = 0;
	
	while (Frame > S->Keys[S->CurKey+1].Frame && S->CurKey<S->NumKeys-2)
		S->CurKey++; // Proceed to next segment(s)
	
	SK=S->Keys+S->CurKey;
	NK=SK+1;
	if (Frame > NK->Frame) {memcpy(&Out->x,&NK->Pos.x,sizeof(Vector)); return;}
	
	t = (Frame - SK->Frame)/ (NK->Frame - SK->Frame);
	t = BKS_Ease(t, SK->EaseFrom, NK->EaseTo);
	
	t2 = t*t;  // t2 = t^2;..Like DUH
	t3 = t2*t; // t3 = t^3;...Umm...Beavis..U already said that...
	dt3 = 2*t3;
	tt2 = 3*t2;
	h[0] = dt3 - tt2 + 1;
	h[1] = tt2 - dt3;
	h[2] = t3  - tt2 + t2 + t;
	h[3] = t3  - t2;
	
	Out->x = (h[0]*SK->Pos.x)+(h[1]*NK->Pos.x)+(h[2]*SK->DD.x)+(h[3]*NK->DS.x);
	Out->y = (h[0]*SK->Pos.y)+(h[1]*NK->Pos.y)+(h[2]*SK->DD.y)+(h[3]*NK->DS.y);
	Out->z = (h[0]*SK->Pos.z)+(h[1]*NK->Pos.z)+(h[2]*SK->DD.z)+(h[3]*NK->DS.z);
}


void Spline_Calc_4D_Alt(Spline *S,float Frame, Quaternion *Out)
{
	int     n;
	float   j,t,t2,t3,tt2,dt3;
	float h[4];
	SplineKey *SK,*NK;
	
	if ((!S)&&(!S->NumKeys)) {Out->x = Out->y = Out->z = 0; Out->W = 1; return;}
	if (S->Flags&TrackREPEAT)
	{
		Frame = S->Keys->Frame + fmod( Frame,S->Keys[S->NumKeys-1].Frame - S->Keys->Frame );
		if (Frame<S->Keys[S->CurKey].Frame) S->CurKey=0;
	}
	
	if (S->NumKeys == 1) {Quaternion_Copy(Out,&S->Keys->Pos); return;}
	
	while (Frame > S->Keys[S->CurKey+1].Frame && S->CurKey<S->NumKeys-2)
		S->CurKey++; // Proceed to next segment(s)
	
	SK = S->Keys+S->CurKey;
	NK = SK+1;
	if (Frame > NK->Frame) {Quaternion_Copy(Out,&NK->Pos); return;}
	
	t = (Frame - SK->Frame) / (NK->Frame - SK->Frame);
	t = BKS_Ease(t, SK->EaseFrom, NK->EaseTo);
	
	t2 = t*t;
	t3 = t2*t;
	dt3 = 2*t3;
	tt2 = 3*t2;
	h[0] = dt3 - tt2 + 1;
	h[1] = tt2 - dt3;
	h[2] = t3  - tt2 + t2 + t;
	h[3] = t3  - t2;
	
	Out->x = (h[0]*SK->Pos.x)+(h[1]*NK->Pos.x)+(h[2]*SK->DD.x)+(h[3]*NK->DS.x);
	Out->y = (h[0]*SK->Pos.y)+(h[1]*NK->Pos.y)+(h[2]*SK->DD.y)+(h[3]*NK->DS.y);
	Out->z = (h[0]*SK->Pos.z)+(h[1]*NK->Pos.z)+(h[2]*SK->DD.z)+(h[3]*NK->DS.z);
	Out->W = (h[0]*SK->Pos.W)+(h[1]*NK->Pos.W)+(h[2]*SK->DD.W)+(h[3]*NK->DS.W);
}

// Under Research. An attempt to interpolate arcs over int32_t distances.
void Spline_Calc_4D(Spline *S,float Frame, Quaternion *Out)
{
	float t;
	Quaternion Q,P,Arc,Deriv,Axis;
	int fspins;
	SplineKey *SK,*NK;
	
	if ((!S)&&(!S->NumKeys)) {Out->x = Out->y = Out->z = 0; Out->W = 1; return;}
	if (S->NumKeys==1) {Quaternion_Copy(Out,&S->Keys->Pos); return;}
	
	// support rewinding
	if (Frame < S->Keys[S->CurKey].Frame)
		S->CurKey = 0;
	
	
	while (Frame > S->Keys[S->CurKey+1].Frame && S->CurKey<S->NumKeys-2) S->CurKey++; // Proceed to next segment(s)
	
	SK=S->Keys+S->CurKey;
	NK=SK+1;
	if (Frame>NK->Frame) {Quaternion_Copy(Out,&NK->Pos); return;}
	t = (Frame-SK->Frame)/(NK->Frame-SK->Frame);
	t = BKS_Ease(t,SK->EaseFrom,NK->EaseTo);
	
	// loser interpolation
	//  SLERP(&SK->Pos,&NK->Pos,t,Out);
	//  SLERP_Axis(&SK->Pos,&NK->AA,t,Out);
	
	// basic interpolation
	SQUAD(&SK->Pos,&SK->DD,&NK->DS,&NK->Pos,t,Out);
	
	// improved interpolation
	// first of all, the main arc:
	//  SLERP_Axis(&SK->Pos,&NK->AA,t,&Arc);
	// ok, now for shittiest part, tangent arc:
	/*  Quaternion_Inverse(&SK->DD,&Q);
	Quaternion_Mul(&NK->DS,&Q,&Axis);
	if (Quaternion_Dot_Product(&Axis,&Arc)<0) Quaternion_Negate(&Axis);
	fspins = floor(NK->AA.W/PI_M2);
	Axis.W = acos(Quaternion_Dot_Product(&SK->DD,&NK->DS))*2.0+fspins;
	SLERP_Axis(&SK->DD,&Axis,t,&Deriv);*/
	//  SLERP(&SK->DD,&NK->DS,t,&Deriv);
	// blend
	//  SLERP(&Arc,&Deriv,t*(1-t)*2.0,Out);
}


// scales all spline key positions by a constant factor.
void Spline_Scale(Spline *S, float s)
{
	for(mword i=0; i<S->NumKeys; i++)
	{
		Quaternion_SelfScale(&S->Keys[i].Pos, s);
	}
}