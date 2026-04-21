#include "PhotonTracer.h"

static ptRealType uRandom(ptRealType a, ptRealType b)
{
	ptRealType x = ptRealType(RAND_15()/ptRealType(RAND_15_MAX));
	return a + (b-a)*x;
}

void ptVector2::fromPolar(ptRealType angle, ptRealType radius)
{
	x = radius * cos(angle);
	y = radius * sin(angle);
}

void ptVector2::normalize()
{
	ptRealType factor = 1.0/sqrt(x*x + y*y);
	x *= factor;
	y *= factor;
}

ptObject2D::ptObject2D()
{
	_verts= NULL;
	_numVerts = 0;
	_material = PT_MAT_UNDEFINED;
	_flags = 0;
}

ptObject2D::ptObject2D(mword numVerts)
{
	_verts= NULL;
	_numVerts = 0;
	_material = PT_MAT_UNDEFINED;
	_flags = 0;
	create(numVerts);
}

ptErrorCode ptObject2D::create(mword numVerts)
{
	_verts = new ptObject2DVertex [numVerts];
	_numVerts = numVerts;

	// unimportant, can be remarked
	memset(_verts, 0, sizeof(ptObject2DVertex) * numVerts);

	return PT_OK;
}

void ptObject2D::computeNormals()
{	
	mword i=0, j = _numVerts-1;
	for(; i<_numVerts; j=i, ++i)
	{
		ptVector2 d = _verts[i]._position - _verts[j]._position;
		_verts[i]._normal = ptVector2( d.y,-d.x);
		_verts[i]._normal.normalize();
		_verts[i]._lineOffset = _verts[i]._position * _verts[i]._normal;
	}

}

ptObject2D::~ptObject2D()
{
	delete [] _verts;
}

static ptRealType calcRefIndex(ptMaterial M, ptRealType w)
{
	//red 1.513 (~680nm)
	//green 1.519 (~530nm)
	//blue 1.528 (~450nm)
//	if (w > 530E-09)
//	{
//		// interpolate using red and green
//		ptRealType t = (w - 530E-09) / (700E-09 - 530E-09);
//		return 1.519 - 0.006*t;
//		return 1.519 - 0.09*t;
//	} else {
//		// interpolate using green and blue
//		ptRealType t = (w - 530E-09) / (450E-09 - 530E-09);
//		return 1.519 + 0.009*t;
//		return 1.519 + 0.09*t;
//	}
	return 1.519 - 0.01 * (w - 530E-09) / (700E-09 - 530E-09);
}

static ptRealType *ptFramebuffer;


float   spectrum_xy[][2] = {
    {0.1741, 0.0050}, {0.1740, 0.0050}, {0.1738, 0.0049}, {0.1736, 0.0049},
    {0.1733, 0.0048}, {0.1730, 0.0048}, {0.1726, 0.0048}, {0.1721, 0.0048},
    {0.1714, 0.0051}, {0.1703, 0.0058}, {0.1689, 0.0069}, {0.1669, 0.0086},
    {0.1644, 0.0109}, {0.1611, 0.0138}, {0.1566, 0.0177}, {0.1510, 0.0227},
    {0.1440, 0.0297}, {0.1355, 0.0399}, {0.1241, 0.0578}, {0.1096, 0.0868},
    {0.0913, 0.1327}, {0.0687, 0.2007}, {0.0454, 0.2950}, {0.0235, 0.4127},
    {0.0082, 0.5384}, {0.0039, 0.6548}, {0.0139, 0.7502}, {0.0389, 0.8120},
    {0.0743, 0.8338}, {0.1142, 0.8262}, {0.1547, 0.8059}, {0.1929, 0.7816},
    {0.2296, 0.7543}, {0.2658, 0.7243}, {0.3016, 0.6923}, {0.3373, 0.6589},
    {0.3731, 0.6245}, {0.4087, 0.5896}, {0.4441, 0.5547}, {0.4788, 0.5202},
    {0.5125, 0.4866}, {0.5448, 0.4544}, {0.5752, 0.4242}, {0.6029, 0.3965},
    {0.6270, 0.3725}, {0.6482, 0.3514}, {0.6658, 0.3340}, {0.6801, 0.3197},
    {0.6915, 0.3083}, {0.7006, 0.2993}, {0.7079, 0.2920}, {0.7140, 0.2859},
    {0.7190, 0.2809}, {0.7230, 0.2770}, {0.7260, 0.2740}, {0.7283, 0.2717},
    {0.7300, 0.2700}, {0.7311, 0.2689}, {0.7320, 0.2680}, {0.7327, 0.2673},
    {0.7334, 0.2666}, {0.7340, 0.2660}, {0.7344, 0.2656}, {0.7346, 0.2654},
    {0.7347, 0.2653}, {0.7347, 0.2653}, {0.7347, 0.2653}, {0.7347, 0.2653},
    {0.7347, 0.2653}, {0.7347, 0.2653}, {0.7347, 0.2653}, {0.7347, 0.2653},
    {0.7347, 0.2653}, {0.7347, 0.2653}, {0.7347, 0.2653}, {0.7347, 0.2653},
    {0.7347, 0.2653}, {0.7347, 0.2653}, {0.7347, 0.2653}, {0.7347, 0.2653},
    {0.7347, 0.2653}};
static ptVector3 WLtoRGB(ptRealType waveLength)
{
	// invisible light
	if (waveLength < 380E-09 || waveLength >= 780E-09)
		return ptVector3(0, 0, 0);

/*	int32_t index = (waveLength - 380E-09) / 5E-09;
	ptRealType t = //fmod(waveLength - 380E-09, 5E-09) / 5E-09;
		(waveLength - (380E-09 + index * 5E-09)) / 5E-09;
	ptVector3 cie;
	
	cie.x = spectrum_xy[index][0] + t * (spectrum_xy[index+1][0] - spectrum_xy[index][0]);
	cie.y = spectrum_xy[index][1] + t * (spectrum_xy[index+1][1] - spectrum_xy[index][1]);

	// perform desaturation
	// first, define or compute some constants.
	ptVector2 r(0.628, 0.346);
	ptVector2 g(0.268, 0.588);
    ptVector2 b(0.150, 0.070);
    ptVector2 w(0.313, 0.329);
	ptVector2 rg = g - r;
	ptVector2 gb = b - g;
	ptVector2 br = r - b;
	ptVector2 wr = w - r;
	ptVector2 wg = w - g;
	ptVector2 wb = w - b;
	ptVector2 rg_n(rg.y, -rg.x);
	ptVector2 gb_n(gb.y, -gb.x);
	ptVector2 br_n(br.y, -br.x);
	ptRealType rg_o =-(r * rg_n);
	ptRealType gb_o =-(g * gb_n);
	ptRealType br_o =-(b * br_n);
	ptRealType rg_w = rg_o + w * rg_n;
	ptRealType gb_w = gb_o + w * gb_n;
	ptRealType br_w = br_o + w * br_n;

	// now, test which edge is nearest to present cie values
	ptRealType o;
	ptVector2 wc(cie.x - w.x, cie.y - w.y);
	ptVector2 wcn(wc.y, -wc.x);

	// check rg segment is stabbed by segment from w to cie color
	o = cie.x * rg_n.x + cie.y * rg_n.y + rg_o;
	if (o>0 && ((wcn*wr)*(wcn*wg)< 0))
	{
		t = rg_w / (rg_w - o);
		t += 0.1 * (1.0-t); // 10% whiter
		cie.x = w.x + t * wc.x;
		cie.y = w.y + t * wc.y;
	} else {
		o = cie.x * gb_n.x + cie.y * gb_n.y + gb_o;
		if (o>0 && ((wcn*wg)*(wcn*wb)< 0))
		{
			t = gb_w / (gb_w - o);
			t += 0.1 * (1.0-t); // 10% whiter
			cie.x = w.x + t * wc.x;
			cie.y = w.y + t * wc.y;
		} else {
			o = cie.x * br_n.x + cie.y * br_n.y + br_o;
			t = br_w / (br_w - o);
			t += 0.1 * (1.0-t); // 10% whiter
			cie.x = w.x + t * wc.x;
			cie.y = w.y + t * wc.y;
		}
	}

	cie.z = 1.0 - cie.x - cie.y;
//	[R] = [  2.739 -1.145 -0.424 ] [X]
//	[G] = [ -1.119  2.029  0.033 ] [Y]
//	[B] = [  0.138 -0.333  1.105 ] [Z]
	ptVector3 rgb(
		 2.739*cie.x-1.145*cie.y-0.424*cie.z, // R
		-1.119*cie.x+2.029*cie.y+0.033*cie.z, // G
		 0.138*cie.x-0.333*cie.y+1.105*cie.z);// B
	// clipping
	if (rgb.x < 0) rgb.x = 0;
	if (rgb.y < 0) rgb.y = 0;
	if (rgb.z < 0) rgb.z = 0;
	return rgb;*/

	float wl = waveLength*1E+09;
	if (wl <= 440.0)
		return ptVector3((440.0-wl)/(440.0-380.0), 0.0, 1.0);
	if (wl <= 490.0)
		return ptVector3(0.0, (wl-440.0)/(490.0-440.0), 1.0);
	if (wl <= 510.0)
		return ptVector3(0.0, 1.0, (510.0-wl)/(510.0-480.0));
	if (wl <= 580.0)
		return ptVector3((wl-510.0)/(580.0-510.0), 1.0, 0.0);
	if (wl <= 645.0)
		return ptVector3(1.0, (645.0-wl)/(645.0-580.0), 0.0);
	if (wl <= 780.0)
		return ptVector3(1.0, 0.0, 0.0);
	
	return ptVector3(1.0, 0.0, 0.0);
}

static void PhotonTracer(const ptObject2D &O, ptPhoton2D &p)
{
	// parhaps let the function compute that later on. right
	// now it is assumed photons begin outside the object.
	int inside = 0;
	p._curRefIndex = 1.0; // hit nothin' but air (i think air = space = 1.0 refindex?)

	ptRealType objRefIndex = calcRefIndex(O._material, p._waveLength);

	// compute RGB representation of photon...(use LHS -> RGB conversion)
	// parhaps just a functional dot-product?
	// right using some bizayoni combination
	ptVector3 photonColor = 1E-03 * WLtoRGB(p._waveLength);

//	int32_t test_x = (p._waveLength - 350E-09) *1E+09;
//	for(mword test_y=0; test_y<10; test_y++)
//	{
//		mword offset = 3*(test_y*XRes + test_x);
//		ptFramebuffer[offset] = 1E+03*photonColor.z;
//		ptFramebuffer[offset+1] = 1E+03*photonColor.y;
//		ptFramebuffer[offset+2] = 1E+03*photonColor.x;
//	}

	mword i, iterations = 100;
	ptRealType subStep = 0.0;

	while (iterations--)
	{
		// calculate ray normal
		ptVector2 rayNormal( p._direction.y, -p._direction.x);

		// intersect p with boundaries of the object
		int32_t col = 0;
		ptVector2 normal;
		ptRealType t, isect = 1E+17;
		ptRealType prevDot = (O[O._numVerts-1]._position - p._position) * rayNormal, dot;
		for(i=0; i<O._numVerts; ++i, prevDot = dot)
		{
			dot = (O[i]._position - p._position) * rayNormal;

			const ptObject2DVertex &v = O[i];
			ptRealType d = p._direction * v._normal;

			if (fabs(d) < 1E-09)
				continue;
			
			t = (v._lineOffset - p._position * v._normal) / d;
			if (t < 1E-04)
				continue;

			if (dot*prevDot > 0.0) // equal signs - ray misses segment
				continue;

			if (isect > t)
			{
				isect = t;
				normal = O[i]._normal;
				col = 1;
			}
		}

		

		if (col == 0) // no collision 
			t = 40.0; // alot, so it scans ahead enough to see the photon leave

		// advance through ray up to t while drawing to screen (needs to be done precisely using a 2D line function
		// or aliasing artifacts will occur).
		ptRealType xRange = 5.0;
		ptRealType yRange = xRange * 3.0 / 4.0;
		ptRealType xstep = xRange / XRes;
		ptRealType ystep = yRange / YRes;
		ptRealType step = (xstep < ystep) ? xstep : ystep;
		ptVector2 pos = p._position;
		ptVector2 delta = step * p._direction;
		pos += subStep * delta;
		for(ptRealType s = subStep; s < isect; s += step, pos += delta)
		{
			ptRealType x = XRes * (pos.x / xRange + 0.5);
			ptRealType y = YRes * (pos.y / yRange + 0.5);

			if (x<0 || y<0 || x >= XRes || y >= YRes)
			{
				// check if ray still flies towards origin
				if (pos * delta < 0.0)
				{
					continue;
				} else {
					// ray out of viewable region, it is assumed no objects are positioned outside of it so it will never
					// bounce back into the region.
					return;
				}

			}
			int32_t ix = int32_t(x);
			int32_t iy = int32_t(y);
			int32_t offset = (ix + iy * XRes) * 3;
			ptFramebuffer[offset+0] += photonColor.z; //B
			ptFramebuffer[offset+1] += photonColor.y; //G
			ptFramebuffer[offset+2] += photonColor.x; //R
		}
		subStep = fmod(isect-subStep, step);

		// intersection has occured, otherwise clipper should've bailed out by now

		// update photon position
		p._position += isect * p._direction;

		// calc alpha - entry angle with normal
		dot = p._direction * normal;

		if (dot < 0.0)
		{
			// swap normal
			normal *=-1.0;
			dot *=-1.0;
		}

		ptRealType alpha = acos(dot), beta;
		ptRealType n1, n2;

		// calc beta - exit angle with normal		
		n1 = p._curRefIndex;
		if (inside)
		{
			n2 = 1.0f; // air's refractive index
		} else {
			n2 = objRefIndex; // object's refractive index
		}
		ptRealType sinBeta = n1*sin(alpha)/n2;

		// compute chance for refraction
		ptRealType refChance;
		if (sinBeta >= 1.0)
			refChance = 1.0; // beyond critical angle, must refract
		else {
			beta = asin(sinBeta);
			// todo: exact chance to refract, should be 
			// (sin(alpha-beta)/sin(alpha+beta))^2
			ptRealType base = sin(alpha-beta) / sin(alpha+beta);
			refChance = base*base;
//			refChance = 0.0;
		}

		if (uRandom(0.0, 1.0) < refChance)
		{
			// refraction occurs
			p._direction -= 2.0 * dot * normal;
		} else {
			// light changes medium, update reflective index
			p._curRefIndex = n2;
			// hack: update 'inside'
			inside ^= 1;

			// rotate by alpha minus beta towards normal
			ptRealType angleDiff = alpha - beta;
			if (p._direction.x * normal.y + p._direction.x * (-normal.x) > 0.0)
				angleDiff *= -1.0;

			// rotate direction by angleDiff CCW
			ptRealType c = cos(angleDiff), s = sin(angleDiff);
			ptVector2 v = p._direction;
			p._direction.x = v.x * c + v.y * s;
			p._direction.y =-v.x * s + v.y * c;			
		}
	}

}

static void PhotonTracerPrismTest()
{
	mword numPhotons = 20000;

	ptObject2D O(3);
	O._flags = PT_OBJ_TRANSCLUENT;
	O._material = PT_MAT_GLASS;


	O[0]._position.fromPolar(0.0 * PI / 180.0, 1.0);
	O[1]._position.fromPolar(120.0 * PI / 180.0, 1.0);
	O[2]._position.fromPolar(240.0 * PI / 180.0, 1.0);

	O.computeNormals();

	// beamAngle/beamTarget either selected at random or predetermined so rays go a int32_t way inside the prism.
	ptRealType beamAngle = RAND_15() * TWOPI / 32768.0;
	ptVector2 beamDirection;
	beamDirection.fromPolar(beamAngle, 1.0);
	ptVector2 beamTarget(uRandom(-0.2, 0.2), uRandom(-0.2, 0.2));
	ptVector2 beamSource = beamTarget - 20.0 * beamDirection;
	ptRealType beamThickness = 0.005;

	// create a 'scene' class? maybe later
	// PhotonTracer2D PT;

	// clear framebuffer
	memset(ptFramebuffer, 0, sizeof(ptRealType) * XRes * YRes * 3);

	ptRealType xRange = 5.0;
	ptRealType yRange = xRange * 3.0 / 4.0;
	mword i;
	for(i=0; i<O._numVerts; i++)
	{
		ptRealType x = XRes * (O[i]._position.x / xRange + 0.5);
		ptRealType y = YRes * (O[i]._position.y / yRange + 0.5);
		int32_t ix = int32_t(x);
		int32_t iy = int32_t(y);
		int32_t offset = (ix + iy * XRes) * 3;
		ptFramebuffer[offset] = 1.0;
		ptFramebuffer[offset+1] = 1.0;
		ptFramebuffer[offset+2] = 1.0;
	}

	for(mword i=0; i<numPhotons; ++i)
	{
		// generate photon inside white light band (aim, then shoot). 
		ptRealType offset = uRandom(-beamThickness, beamThickness);
		ptVector2 source(beamSource.x + offset * beamDirection.y, beamSource.y - offset * beamDirection.x);

		// helps antialiasing
		source += 0.0078125000 * uRandom(0.0, 1.0) * beamDirection;

		ptRealType wlength = 1E-09 * uRandom(350.0, 680.0);
		ptPhoton2D p(source, beamDirection, wlength);
		PhotonTracer(O, p);
	}
}

void PhotonTracerRender()
{
	ptRealType *source = ptFramebuffer;
	dword *page = (dword *)(MainSurf->Data);
	ptRealType k = 1.0;
	for(mword j=0; j<YRes; j++)
		for(mword i=0; i<XRes; i++)
		{
			ptRealType rr = k* (*source++);
			ptRealType rg = k* (*source++);
			ptRealType rb = k* (*source++);
			dword b = dword(255.0 * (1 - 1/(rr+1)) );
			dword g = dword(255.0 * (1 - 1/(rg+1)) );
			dword r = dword(255.0 * (1 - 1/(rb+1)) );

			if (b > 255) b = 255;
			if (g > 255) g = 255;
			if (r > 255) r = 255;

			*page++ = b + (g<<8) + (r<<16);
		}
}

void TestPhotonTracer()
{
	const int32_t PartTime = 10000;

	ptFramebuffer = new ptRealType [XRes * YRes * 3];
	

	float TT = Timer;
	while (Timer < PartTime)
	{
		dTime = (Timer - TT) / 100.0;
		TT = Timer;

		memset(VPage, 0, PageSize);

		PhotonTracerPrismTest();
		PhotonTracerRender();
		Flip(MainSurf);
//		Modulate(MainSurf,&Blur,0x707070,0x808080);
//		Flip(&Blur);
		mword x = Timer+100;
		while (Timer<x)
		{
			int pastrama = 1;
		}

		//while (Timer < TT + 50) continue;
		if (Keyboard[ScESC])
		{
			Timer = PartTime;
			break;
		}
	} Timer -= PartTime;

	delete [] ptFramebuffer;
}
