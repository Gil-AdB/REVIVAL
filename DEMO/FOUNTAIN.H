#define FntFactor 1.5
#define FntInnerPcls int(1000 * FntFactor)
#define FntOuterPcls int(2000 * FntFactor)
#define FntSpiralPclsOff (FntInnerPcls*3+FntOuterPcls)
#define FntSpiralPcls int(500 * FntFactor)

#define FntSpawnOutPcl int(400.0 * FntFactor)
#define FntSpawnInnerPcl int(80.0 * FntFactor)

#define FntSpawnOutDisp 1.0
#define FntSpawnOutPow 30.0
#define FntSpawnOutPowTilt 5.0
#define FntSpawnOutPowRand 23.5
#define FntSpawnOutPowFreq 0.04

#define FntSpawnOutGrav 20.0

#define FntLightningTime1 500
#define FntLightningTime2 1500
#define FntLightningTime3 2200
#define FntLightningTime4 2700
#define FntLightningTime5 3200
#define FntLightningTime6 3500
//#define FntLightningTime1 500
//#define FntLightningTime2 1500
//#define FntLightningTime3 2200
//#define FntLightningTime4 2700
//#define FntLightningTime5 3200
//#define FntLightningTime6 3500

#define FntLightningPcls  int(400 * FntFactor)

//#define FntPhotonStart 4500
//#define FntPhotonAtVortex 4650
#define FntVortexSwarm 4000
#define FntVortexMajorSwarm 4200

#define FntVortexStart 4400
#define FntVortexMax 4700


extern void Load_Vortex();
extern void Iterate_Photonbeam_Texture();
extern void Noise_Sample();
extern void Add_Vortex_ToScene();