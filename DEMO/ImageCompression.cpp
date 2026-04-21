#include "ImageCompression.h"

const float log2conv = 1.0 / log(2.0);

#if !defined(__APPLE__)
inline float log2(float x) noexcept
{
	return log(x)*log2conv;
}
#endif

mword S3TC_coder::calcError3(dword *pixels, mword numPixels, dword u, dword v)
{
	mword r[4], g[4], b[4];

	b[0] = (u&0xff);
	b[1] = (v&0xff);
	g[0] = ((u>>8)&0xff);
	g[1] = ((v>>8)&0xff);
	r[0] = ((u>>16)&0xff);
	r[1] = ((v>>16)&0xff);
	b[2] = (b[0] + b[1])>>1;
	g[2] = (g[0] + g[1])>>1;
	r[2] = (r[0] + r[1])>>1;

	b[3] = g[3] = r[3] = 0;

	mword accError = 0;	
	for(mword i=0; i<numPixels; i++)
	{
		mword pr = (pixels[i]>>16)&0xff;
		mword pg = (pixels[i]>>8)&0xff;
		mword pb = (pixels[i])&0xff;

		mword minError = mword(-1);
		mword index;
		for(mword j=0; j<4; j++)
		{
			sword dr = sword(pr - r[j]);
			sword dg = sword(pg - g[j]);
			sword db = sword(pb - b[j]);
			mword error = dr*dr + dg*dg + db*db;
			if (minError > error )
			{
				minError = error;
				index = j;
			}
		}
		accError += minError;
	}
	return accError;
}

mword S3TC_coder::calcError4(dword *pixels, mword numPixels, mword u, mword v)
{
	mword r[4], g[4], b[4];

	b[0] = (u&0xff);
	b[1] = (v&0xff);
	g[0] = ((u>>8)&0xff);
	g[1] = ((v>>8)&0xff);
	r[0] = ((u>>16)&0xff);
	r[1] = ((v>>16)&0xff);
	b[2] = (b[0]*2 + b[1])/3;
	b[3] = (b[0] + b[1]*2)/3;
	g[2] = (g[0]*2 + g[1])/3;
	g[3] = (g[0] + g[1]*2)/3;
	r[2] = (r[0]*2 + r[1])/3;
	r[3] = (r[0] + r[1]*2)/3;

	mword accError = 0;
	for(mword i=0; i<numPixels; i++)
	{
		mword pr = (pixels[i]>>16)&0xff;
		mword pg = (pixels[i]>>8)&0xff;
		mword pb = (pixels[i])&0xff;

		mword minError = mword(-1);
		mword index;
		for(mword j=0; j<4; j++)
		{
			int32_t dr = int32_t(pr - r[j]);
			int32_t dg = int32_t(pg - g[j]);
			int32_t db = int32_t(pb - b[j]);
			mword error = dr*dr + dg*dg + db*db;
			if (minError > error)
			{
				minError = error;
				index = j;
			}
		}
		accError += minError;
	}
	return accError;
}

dword S3TC_coder::encode3(dword *pixels, mword numPixels, mword u, mword v)
{
	mword r[4], g[4], b[4];

	b[0] = ((u<<3)&0xf8);
	b[1] = ((v<<3)&0xf8);
	g[0] = ((u>>3)&0xfc);
	g[1] = ((v>>3)&0xfc);
	r[0] = ((u>>8)&0xf8);
	r[1] = ((v>>8)&0xf8);
	b[2] = (b[0] + b[1])>>1;
	g[2] = (g[0] + g[1])>>1;
	r[2] = (r[0] + r[1])>>1;

	b[3] = g[3] = r[3] = 0;

	mword code = 0;	
	for(mword i=0; i<numPixels; i++)
	{
		mword pr = (pixels[i]>>16)&0xff;
		mword pg = (pixels[i]>>8)&0xff;
		mword pb = (pixels[i])&0xff;

		mword minError = mword(-1);
		mword index;
		for(mword j=0; j<4; j++)
		{
			sword dr = sword(pr - r[j]);
			sword dg = sword(pg - g[j]);
			sword db = sword(pb - b[j]);
			mword error = dr*dr + dg*dg + db*db;
			if (minError > error )
			{
				minError = error;
				index = j;
			}
		}
		code |= index << (2*i);
	}
	return code;
}

dword S3TC_coder::encode4(dword *pixels, mword numPixels, mword u, mword v)
{
	mword r[4], g[4], b[4];

	b[0] = ((u<<3)&0xf8);
	b[1] = ((v<<3)&0xf8);
	g[0] = ((u>>3)&0xfc);
	g[1] = ((v>>3)&0xfc);
	r[0] = ((u>>8)&0xf8);
	r[1] = ((v>>8)&0xf8);
	b[2] = (b[0]*2 + b[1])/3;
	b[3] = (b[0] + b[1]*2)/3;
	g[2] = (g[0]*2 + g[1])/3;
	g[3] = (g[0] + g[1]*2)/3;
	r[2] = (r[0]*2 + r[1])/3;
	r[3] = (r[0] + r[1]*2)/3;

	mword code = 0;
	for(mword i=0; i<numPixels; i++)
	{
		mword pr = (pixels[i]>>16)&0xff;
		mword pg = (pixels[i]>>8)&0xff;
		mword pb = (pixels[i])&0xff;

		mword minError = mword(-1);
		mword index;
		for(mword j=0; j<4; j++)
		{
			sword dr = sword(pr - r[j]);
			sword dg = sword(pg - g[j]);
			sword db = sword(pb - b[j]);
			mword error = dr*dr + dg*dg + db*db;
			if (minError > error)
			{
				minError = error;
				index = j;
			}
		}
		code |= index << (2*i);
	}
	return code;
}

mword S3TC_coder::rgbDistance(mword u, mword v)
{
	// doesn't quite work
//	sdword r = ((u>>16) - (v>>16)) & 0xff;
//	sdword g = ((u>>8) - (v>>8)) & 0xff;
//	sdword b = (u - v) & 0xff;

	int32_t r = ((u>>16)&0xff) - ((v>>16)&0xff);
	int32_t g = ((u>>8)&0xff) - ((v>>8)&0xff);
	int32_t b = (u&0xff) - (v&0xff);
	return r*r + g*g + b*b;
}

void S3TC_coder::encode(Image *Im)
{
	Image &I = *Im;
	mword xBlocks = (I.x+3)>>2;
	mword yBlocks = (I.y+3)>>2;

	delete [] _stream;
	// 8 bytes for header, 64bit/block.
	_stream = new byte [8 + xBlocks * yBlocks * 8];
	byte *output = _stream;

	((dword *)output)[0] = I.x;
	((dword *)output)[1] = I.y;
	output += 2*sizeof(dword);

	for(mword y=0; y<yBlocks; y++)
	{
		for(mword x=0; x<xBlocks; x++)
		{
			// check block boundaries
			mword xLimit = I.x - (x<<2);
			if (xLimit > 4) 
				xLimit = 4;
			mword yLimit = I.y - (y<<2);
			if (yLimit > 4) 
				yLimit = 4;

			// collect data (pixels) from block
			dword *blockPtr = I.Data + ((x + y * I.x) << 2);
			dword block[16];
			dword blockSize = 0;
			mword i, j;
			for(j=0; j<yLimit; j++)
			{
				for(i=0; i<xLimit; i++)
				{
					block[blockSize++] = blockPtr[i];
				}
				blockPtr += I.x;
			}

			// look for two points most far apart in block
			mword maxDistance = 0;
			mword u=0, v=0;
			for(j=0; j<blockSize; j++)
			{
				for(i=j+1; i<blockSize; i++)
				{
					mword distance = rgbDistance(block[i], block[j]);
					if (maxDistance < distance)
					{
						maxDistance = distance;
						u = i;
						v = j;
					}
				}
			}
			u = block[u];
			v = block[v];

			// now attempt to optimize S3TC representation of block by adjusting u, v
			// we skip this for now: just compute which method (3-color or 4-color) compresses
			// with less error using (u, v) and use it. Actually we'll want to compute
			// u, v entirely differently for 3-color mode because we have a black color 
			// in the palette regardless of what u and v are.
			mword error0 = calcError4(block, blockSize, u, v);
			mword error1 = calcError3(block, blockSize, u, v);
			mword which = (error1 < error0);

			// convert selected colors to R5G6B5
			u = toR5G6B5(u);
			v = toR5G6B5(v);
			// encode which method we are using by the order of u, v 
			// (1bit equiv. to a permutation in S2 :)
			if ((u <= v) ^ which)
			{
				mword swap = u;
				u = v;
				v = swap;
			}

			dword code;
			if (which)
			{
				code = encode3(block, blockSize, u, v);
			} else {
				code = encode4(block, blockSize, u, v);
			}

			// write encoded block
			// write u, v to stream in format R5G6B5.
			((word *)output)[0] = u;
			((word *)output)[1] = v;
			// write pixels
			((dword *)output)[1] = code;
			output += 2*sizeof(word) + sizeof(dword);
		}
	}
}

void S3TC_coder::decode(Image *Im)
{
	Image &I = *Im;

	delete [] I.Data;

	byte *input = _stream;
	I.x = ((dword *)input)[0];
	I.y = ((dword *)input)[1];
	input += 2*sizeof(dword);

	I.Data = new dword [I.x * I.y];

	mword xBlocks = (I.x+3)>>2;
	mword yBlocks = (I.y+3)>>2;

	for(mword y=0; y<yBlocks; y++)
	{
		for(mword x=0; x<xBlocks; x++)
		{
			// check block boundaries
			mword xLimit = I.x - (x<<2);
			if (xLimit > 4) 
				xLimit = 4;
			mword yLimit = I.y - (y<<2);
			if (yLimit > 4) 
				yLimit = 4;

			dword *blockPtr = I.Data + ((x + y * I.x) << 2);
			dword block[16];

			// read pixel block.
			mword i, j, u, v, code;
			u = ((word *)input)[0];
			v = ((word *)input)[1];
			code = ((dword *)input)[1];
			input += sizeof(word)*2 + sizeof(dword);
			
			// actual data may contain less than 16 entries, but it's faster to use 16
			// in any case.
			mword blockSize = 16;

			dword colors[4];
			mword r[2], g[2], b[2];
			// unpack R5G6B5 format. NOTE: will not mess up order relation
			u = fromR5G6B5(u);
			v = fromR5G6B5(v);
			colors[0] = u;
			colors[1] = v;
			b[0] = (u&0xff);
			b[1] = (v&0xff);
			g[0] = ((u>>8)&0xff);
			g[1] = ((v>>8)&0xff);
			r[0] = ((u>>16)&0xff);
			r[1] = ((v>>16)&0xff);

			if (u > v)
			{
				// 4-color mode
				colors[2] = 
					(((b[0]*2 + b[1])/3)      ) +
					(((g[0]*2 + g[1])/3) << 8 ) +
					(((r[0]*2 + r[1])/3) << 16);
				colors[3] =
					(((b[0] + b[1]*2)/3)      ) +
					(((g[0] + g[1]*2)/3) << 8 ) +
					(((r[0] + r[1]*2)/3) << 16);
			} else {
				// 3-color mode
				colors[2] =
					(((b[0] + b[1]) >> 1)      ) +
					(((g[0] + g[1]) >> 1) << 8 ) +
					(((r[0] + r[1]) >> 1) << 16);
				colors[3] = 0;
			}

			// unpack code
			for(i=0; i<blockSize; i++)
			{
				block[i] = colors[(code >> (2*i))&3];
			}

			mword offset = 0;
			for(j=0; j<yLimit; j++)
			{
				for(i=0; i<xLimit; i++)
				{
					blockPtr[i] = block[offset++];
				}
				blockPtr += I.x;
			}
		}
	}
}

/*void S3TC_coder::compress()
{
	// reorder stream (permuted bitslicing :)
	// actually it might be better to also scramble blocks (as in, batch 8x8 tiles),
	// to improve correlation between data.
	mword x = ((dword*)_stream)[0];
	mword y = ((dword*)_stream)[1];	
	dword *in = &((dword*)_stream)[2];

	mword order[64] = {
		// MSBs are compressable (more 0s than 1s, i think) so move them to front...
		33, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55, 57, 59, 61, 63,

		4, 10, 15,   16+4, 16+10, 16+15,
		3,  9, 14,	 16+3,  16+9, 16+14,
		2,  8, 13,   16+2,  16+8, 16+13,
		1,  7, 12,   16+1,  16+7, 16+12,
		0,  6, 11,   16+0,  16+6, 16+11,
		    5,              16+5,

		// LSBs are complete noise
		32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62
	};

	mword invorder[64];
	mword i, j;
	for(i=0; i<64; i++)
		invorder[order[i]] = i;

	mword numBlocks = ((x+3)>>2)*((y+3)>>2);
	dword *temp = new dword [2 + numBlocks*2];
	memset(temp, 0, sizeof(dword)*(2+numBlocks*2));
	temp[0] = x;
	temp[1] = y;

	mword streamLength = numBlocks*2;
	mword offset = 64; // offset is 64 bits, immidiately following header

	// bitslicing code
//	for(j=0; j<64; j++)
//	{
//		mword channel = order[j];
//		
//		if (channel < 32)
//		{
//			for(i=0; i<streamLength; i+=2, offset++)
//			{
//				mword d = (in[i]>>channel)&1;
//				temp[offset>>5] += d<<(offset&0x1f);
//			}
//		} else {
//			channel -= 32;
//			for(i=1; i<streamLength; i+=2, offset++)
//			{
//				mword d = (in[i]>>channel)&1;
//				temp[offset>>5] += d<<(offset&0x1f);
//			}
//		}
//	}
	// let's do something simple, like putting all palettes and bitcodes seperately!
	dword *out = temp+2;
	for(i=0; i<numBlocks; i++)
	{
		out[i] = in[2*i];
	}
	out += numBlocks;
	for(i=0; i<numBlocks; i++)
	{
		out[i] = in[2*i+1];
	}

	mword filesize = (2 + streamLength) * sizeof(dword);
	FILE *F = fopen("texture.st3c", "wb");
	fwrite(_stream, filesize, 1, F);
	fclose(F);

	F = fopen("texture.st3c.reorder", "wb");
	fwrite(temp, filesize, 1, F);
	fclose(F);

	delete [] _stream;
	_stream = (byte*)(temp);
}

void S3TC_coder::decompress()
{
	// reorder stream (permuted bitslicing :)
	// actually it might be better to also scramble blocks (as in, batch 8x8 tiles),
	// to improve correlation between data.
	mword x = ((dword*)_stream)[0];
	mword y = ((dword*)_stream)[1];	
	dword *in = &((dword*)_stream)[2];

	mword order[64] = {
		// MSBs are compressable (more 0s than 1s, i think) so move them to front...
		33, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55, 57, 59, 61, 63,

		// RGB most to least significant
		4, 10, 15,   16+4, 16+10, 16+15,
		3,  9, 14,	 16+3,  16+9, 16+14,
		2,  8, 13,   16+2,  16+8, 16+13,
		1,  7, 12,   16+1,  16+7, 16+12,
		0,  6, 11,   16+0,  16+6, 16+11,
		    5,              16+5,

		// LSBs are complete noise
		32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62
	};

	mword invorder[64];
	mword i, j;
	for(i=0; i<64; i++)
		invorder[order[i]] = i;

	mword numBlocks = ((x+3)>>2)*((y+3)>>2);
	dword *temp = new dword [2 + numBlocks*2];
	memset(temp, 0, sizeof(dword)*(2+numBlocks*2));
	temp[0] = x;
	temp[1] = y;

	mword offsets[64]; // offsets are in bits, from end of header
	for(i=0, j=0; i<64; i++, j+=numBlocks)
	{
		offsets[order[i]] = j;
	}

	mword streamLength = numBlocks*2;
	mword offset = 64; // offset is 64 bits, immidiately following header

//	for(i=0; i<numBlocks; i++)
//	{
//		for(j=0; j<64; j++, offset++)
//		{
//			// read one bit from from offsets[j]
//			mword x = offsets[j]++;
//			mword b = (in[x>>5] >> (x&0x1f))&1;
//			temp[offset>>5] += b<<(offset&0x1f);
//		}
//	}

	dword *out = temp+2;
	for(i=0; i<numBlocks; i++)
	{
		out[2*i]   = in[i];
		out[2*i+1] = in[numBlocks+i];
	}
	
	delete [] _stream;
	_stream = (byte*)(temp);
}*/

// this routine is used for research. the commented out version the current 'release' build
// of the S3TC lossless compressor.
void S3TC_coder::compress()
{
/*	mword x = ((dword*)_stream)[0];
	mword y = ((dword*)_stream)[1];	
	word *in = ((word*)_stream) + 4;

	mword numBlocks = ((x+3)>>2)*((y+3)>>2);
	mword S3TCFilesize = (2 + numBlocks*2) * sizeof(dword);
	mword reorderedS3TCFilesize = 8 + numBlocks*10;
	byte *temp = new byte [8 + numBlocks*10];
	memset(temp, 0, 8 + numBlocks*10);

	((dword *)temp)[0] = x;
	((dword *)temp)[1] = y;
	byte *out = temp+8;

	mword i;
	for(i=0; i<numBlocks; i++)
	{
		// just copy everythnig
//		memcpy(out, in, 4);		
//		out += 4;
		// unpack 565 and copy indices
		out[0] = in[0]&0x1f;
		out[1] = (in[0]>>6)&0x1f; // killing 1 bit on purpose here
		out[2] = (in[0]>>1)&0x1f;
		out[3] = in[1]&0x1f;
		out[4] = (in[1]>>6)&0x1f; // killing 1 bit on purpose here
		out[5] = (in[1]>>1)&0x1f;
		out += 6;
		((word*)out)[0] = in[2];
		((word*)out)[1] = in[3];
		out += 4;
		in += 4;

	}

	FILE *F = fopen("texture.st3c", "wb");
	fwrite(_stream, S3TCFilesize, 1, F);
	fclose(F);

	F = fopen("texture.st3c.reorder", "wb");
	fwrite(temp, reorderedS3TCFilesize, 1, F);
	fclose(F);

	delete [] _stream;
	_stream = (byte*)(temp);*/

	// just compute the entropy of all sorts of multi-bit channels such as
	// [0..4], [5..10], [11..15] etc
	mword x = ((dword*)_stream)[0];
	mword y = ((dword*)_stream)[1];	
	dword *in = ((dword*)_stream) + 2;
	
	mword numBlocks = ((x+3)>>2)*((y+3)>>2);

	// multibit channel
	const mword base = 44;
	const mword limit = 2;
	const mword bins = 1<<limit;
	mword count[bins];
	memset(count, 0, bins*sizeof(mword));


	// NOTE: MTF lowered the band entropy for palette entries. (it works!)

	// TODO: use a smarter MTF! if we move a symbol to front, we might as well wanna bring
	// all his groupies. more precisely, move its nearby environment to front. this
	// is because we're compressing numbers, not just alphabet symbols.
	// we can actually build some kind of a heirarchy in which we first move all symbols
	// with same MSB to front, then all the symbols with same 2 most bits to front, etc'

	// NOTE: compressing deltas works much better than MTF.

	// array implementation is inefficient, try to find a better datastructure
	int32_t symbolEncoder[bins];
	
	mword i;
	for(i=0; i<bins; i++)
		symbolEncoder[i] = i;

	int32_t prev = 0, symbol, value;
	for(i=0; i<numBlocks; i++)
	{
		// assuming no wraparound to next dword
		mword mask = (1<<limit)-1;
		if (base < 32)
		{
			mword shift = base;
			symbol = (in[i*2+0] >> shift) & mask;
		} else {
			mword shift = base - 32;
			symbol = (in[i*2+1] >> shift) & mask;
		}
		// read current encoding
//		value = symbolEncoder[symbol];
		// compress deltas
		value = symbol - prev;
		prev = symbol;
		if (value < 0)
			value += bins;
		// update count
		++count[value];
		// move symbol to front
//		for(mword j=0; j<bins; j++)
//			if (symbolEncoder[j] < value)
//				++symbolEncoder[j];
//		symbolEncoder[symbol] = 0;
	}
	float entropy = 0.0;
	for(i=0; i<(1<<limit); ++i)
	{
		float prob = float(count[i]) / float(numBlocks);
		if (prob>0.0)
			entropy += prob * log2(prob);
	}
	float smoothEntropy = log2(1.0/bins);

	// entropy = amount of information in symbol string
	int breakhere=1;
}

void S3TC_coder::decompress()
{
}
#define POINTER_64
//#include <windows.h>
void ImageCompressionTestCode()
{
	Image I, Output;
	Output.Data = NULL;

	Load_Image_JPEG(&I, "TEXTURES\\PrimaryTest.jpg");
//	Load_Image_JPEG(&I, "TEXTURES\\B5.jpg");

	int yadda = 1;

	S3TC_coder s3tc;

	mword numPasses = 1;
//	mword timerStart = Timer;
	for(mword z=0; z<numPasses; z++)
		s3tc.encode(&I);

	s3tc.compress();
	s3tc.decompress();


//	mword timeTaken = Timer - timerStart;
	mword timerStart = Timer;
	for(mword z=0; z<numPasses; z++)
		s3tc.decode(&Output);
	mword timeTaken = Timer - timerStart;

	// #pixels*timer resolution*#passes / time taken
	float pixelsPerSecond = float(I.x*I.y)*100.0f*numPasses / float(timeTaken);
	char str[128];
	sprintf(str, "%f pixels per second\n", pixelsPerSecond);
//	OutputDebugString(str);


	Timer = 0;
	while (Timer < 10000)
	{
		byte *page = (byte*)VPage;
		memset(page, 0, PageSize);
		for(mword j=0; j<640; j++)
		{
			mword y = j;//64+(j>>1);
			dword *txtr = &(I.Data[y*I.x]);
			dword *txtr2 = &(Output.Data[y*I.x]);
			for(mword i=0; i<480; i++)
			{
				mword x = i;//64+(i>>1);
				if (0)//(x == 112 && y == 104)
				{
					((dword *)page)[i] = 0xffffffff;
					((dword *)page)[i+300] = 0xffffffff;
				} else {
//					((dword *)page)[i] = txtr[x];// & 0xf8fcf8;
//					((dword *)page)[i+300] = txtr2[x];
					((dword *)page)[i] = txtr2[x];
				}
			}
			page += VESA_BPSL;
		}
		MainSurf->Flip(MainSurf);

		if (Keyboard[ScESC])
			break;
	}
}
