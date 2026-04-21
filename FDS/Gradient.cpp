#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"

#include <vector>
#include <stdexcept>
#include "Gradient.h"

template <typename T>
T lerpRatio(T x0, T x1, T x) {
	return (x - x0) / (x1 - x0);
}

Color lerp(Color a, Color b, float t) {
	return Color{
		a.B * (1 - t) + b.B * t,
		a.G * (1 - t) + b.G * t,
		a.R * (1 - t) + b.R * t,
		a.A * (1 - t) + b.A * t
	};
}

Color evalGradient(const std::vector<GradientEndpoint>& endpoints, float u, float v, float vSlack, bool rainDrop) {
	float vCenter = (rainDrop) ? -0.3 : 0;
	if (rainDrop) {
		u = 1 - fabs(u - 0.5) * 2;
		v = (v - 0.5) * 2;
		float r;
		if (v < vCenter) {
			r = lerpRatio(vCenter, -1.0f, v);
		} else {
			r = lerpRatio(vCenter, 1.0f, v);
		}
		float reduction = 1.0 - sqrt(1 - r * r);
		u = std::max(u - reduction, 0.0f);
	} else {
		u = 1 - fabs(u - 0.5) * 2;
		v = 1 - fabs(v - 0.5) * 2;
		if (v < vSlack) {
			float r = 1.0f - v / vSlack;
			float reduction = 1.0 - sqrt(1 - r * r);
			u = std::max(u - reduction, 0.0f);
		}
	}
	if (u <= endpoints.front().u) {
		return endpoints.front().c;
	}
	for (size_t i = 1; i != endpoints.size(); ++i) {
		if (endpoints[i].u < u) continue;

		float t = lerpRatio(endpoints[i - 1].u, endpoints[i].u, u);
		Color c = lerp(endpoints[i - 1].c, endpoints[i].c, t);		
		return c;
	}
	return endpoints.back().c;
}

Material* Generate_Gradient(const std::vector<GradientEndpoint>& endpoints, int txSize, float vSlack, bool rainDrop) {
	Image Img;
	Material* M = getAlignedType<Material>(16); //(Material*)getAlignedBlock(sizeof(Material), 16);
	//memset(M, 0, sizeof(Material));
	Img.x = txSize;
	Img.y = txSize;
	// whatever that means
	M->Flags = Mat_Virtual;
	M->Txtr = new Texture;
	M->Txtr->Flags = Txtr_Nomip | Txtr_Tiled;
	memset(M->Txtr, 0, sizeof(Texture));
	M->Txtr->BPP = 32;
	Img.Data = new DWord[txSize * txSize];

	if (endpoints.size() < 2) {
		throw std::runtime_error("this sucks");
	}

	for (size_t y = 0; y != txSize; ++y) {
		float v = (float)y / txSize;
		for (size_t x = 0; x != txSize; ++x) {
			float u = (float)x / txSize;
			Color c = evalGradient(endpoints, u, v, vSlack, rainDrop);
			Img.Data[x + y * txSize] = ToQColor(c);
		}
	}
	
	Convert_Image2Texture(&Img, M->Txtr);
	delete[] Img.Data;

	//Sachletz((dword*)(M->Txtr->Data), txSize, txSize);
	M->Txtr->Mipmap[0] = M->Txtr->Data;
	M->Txtr->numMipmaps = 1;

	return M;

}
