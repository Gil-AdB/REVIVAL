#include "Base/FDS_DECS.H"
#include "Base/FDS_VARS.H"

int32_t Basic_FOffs;

Font * LoadAFT(const char *FileName)
{
	int32_t I, J, K;
	FILE *F;
	char Binary[8], Nums[8];
	char Num, Mask, BPP;
	char XS, YS;
	char Bfr[156], Jump;
	Font *FFI = new Font;

	F = fopen(FileName, "rb");
	if (!F) {
		printf("AFT Font File %s open failure.\n", FileName);
		exit(-1);
	}
	//  printf("� Loading an AFT font file...\n");

		// Header...
	fread(&XS, 1, 1, F);
	fread(&YS, 1, 1, F);
	fread(&BPP, 1, 1, F);
	Jump = 8 / BPP;

	FFI->X = XS;
	FFI->Y = YS;
	FFI->XY = FFI->X*FFI->Y;
	FFI->BPP = BPP;
	FFI->Ch = (byte *)malloc(128 * XS*YS);
	FFI->Len = (byte *)malloc(128);

	for (K = 0; K < 128 * YS*XS; K += Jump)
	{
		fread(&Num, 1, 1, F);
		Mask = 1;
		for (J = 0; J < 8; J++)
		{
			Binary[J] = (Num&Mask) / Mask;
			Mask *= 2;
		}
		for (J = 0; J < Jump; J++)
		{
			Mask = 1;
			Nums[J] = 0;
			for (I = 0; I < BPP; I++)
			{
				Nums[J] += Binary[J*BPP + I] * Mask;
				Mask *= 2;
			}
		}
		for (J = 0; J < Jump; J++)
			FFI->Ch[K + J] = Nums[J];

	}
	for (I = 0; I < 128; I++)
	{
		fread(&Bfr, 1, 1, F);
		FFI->Len[I] = Bfr[0];
	}
	FFI->Len[32] = 0;
	fclose(F);
	return FFI;
}

int32_t OutTextXY(byte *Where,int32_t X,int32_t Y, const char *S, uint8_t C, int xres /* = XRes*/, int yres /*= YRes*/)
{
	int DX;
	int II, IJ, IK;
	byte CP = 0, Ch;
	int32_t PgOffset, POfs, FOfs, FEnd, Hisx;
	byte TC[2];
	char CPP = (BPP + 1) >> 3;
	byte *Ptr, *Pen;
	Font *F = Active_Font;
	if (Y >= yres - F->Y) {
		return Y;
	}


	if (BPP == 8) { TC[0] = 0; TC[1] = C; }
	else {
		TC[0] = 0;
		TC[1] = 192;
	}

	long width = 0;
	DX = X;
	PgOffset = Y * xres;//YOffs[Y];

	Hisx = F->X;

	while ((Ch = *S++))
	{
		FEnd = F->Len[Ch];
		if (Ch == 13 || Ch == 10 || DX + FEnd + 2 >= xres) { 
			Y += F->Y + 2;
			PgOffset = Y * xres; 
			DX = X; 
			if (Y >= yres - F->Y) return Y;
			continue;
		}

		POfs = (PgOffset + DX)*CPP;
		FOfs = Ch * F->XY;
		for (IJ = 0; IJ < F->Y; IJ++)
		{
			Ptr = F->Ch + FOfs;
			II = FEnd;
			Pen = Where + POfs;
			while (II--)
			{
				//        Where[POfs+II] = TC[F->Ch[FOfs+II]];
				if (TC[*Ptr++])
					memset(Pen, C, CPP);
				Pen += CPP;
			}
			FOfs += Hisx;
			POfs += xres * CPP;
		}
		DX += FEnd + 2;
		if (width < DX - X) {
			width = DX - X;
		}
	}
	return Y;
}

void OutTextXYExtents(int32_t X, int32_t Y, const char* S, int xres, int yres, int32_t& out_height, int32_t& out_width)
{
	int DX;
	int II, IJ, IK;
	byte CP = 0, Ch;
	long int PgOffset, POfs, FOfs, FEnd, Hisx;
	byte TC[2];
	char CPP = (BPP + 1) >> 3;
	byte* Ptr, * Pen;
	Font* F = Active_Font;
	if (Y >= yres - F->Y) {
		out_width = 0;
		out_height = 0;
		return;
	}

	long width = 0;
	DX = X;
	PgOffset = Y * xres;//YOffs[Y];

	Hisx = F->X;

	while ((Ch = *S++))
	{
		FEnd = F->Len[Ch];
		if (Ch == 13 || Ch == 10 || DX + FEnd + 2 >= xres) { 
			Y += F->Y + 2; 
			PgOffset = Y * xres; 
			DX = Basic_FOffs; 
			if (Y >= yres - F->Y) { 
				out_width = width;
				out_height = Y;  
				return; 
			}; 
		}

		POfs = (PgOffset + DX) * CPP;
		FOfs = Ch * F->XY;
		for (IJ = 0; IJ < F->Y; IJ++)
		{
			FOfs += Hisx;
			POfs += xres * CPP;
		}
		DX += FEnd + 2;
		if (width < DX - X) {
			width = DX - X;
		}
	}
	out_width = width;
	out_height = Y + F->Y;
}


// Writes string S on [X,Y] using the color C, transparently.
long Write_String_T32(long X, long Y, byte *S, DWord C)
{
	int DX;

	byte CP = 0, Ch;
	DWord TC[2] = { 0,C }, DW;
	byte *Ptr;
	DWord *Pen;
	long CLen, FOfs;
	long I, J;

	Font *F = Active_Font;
	DWord *VP = ((DWord *)VPage) + YOffs[Y], *WP;

	if (Y >= YRes - F->Y) return Y;
	DX = X;

	while ((Ch = *S++))
	{
		CLen = F->Len[Ch];
		if (Ch == 13 || Ch == 10 || DX + CLen + 2 >= XRes) { Y += F->Y + 2; VP += YOffs[F->Y + 2]; DX = Basic_FOffs; if (Y >= YRes - F->Y) return Y; }

		WP = VP + DX;
		FOfs = Ch * F->XY;
		for (J = 0; J < F->Y; J++)
		{
			Ptr = F->Ch + FOfs;
			I = CLen;
			Pen = WP;
			while (I--)
			{
				if ((DW = TC[*Ptr++])) *Pen = DW;
				Pen++;
			}
			WP += YOffs[1];
			FOfs += F->X;
		}
		DX += CLen + 2;
	}
	return Y;
}
