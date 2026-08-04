#define FntInnerPcls 500
#define FntOuterPcls 500
#define FntSpiralPclsOff 2000 //500+500*3
#define FntSpiralPcls 1000

#define FntSpawnOutPcl 30.0
// Inner (circling) emitters. Was a fixed 12 per FRAME, i.e. frame-rate dependent: the pool
// is FntInnerPcls(500) with a Charge-3.5/dt = 3.5s life, so it sustains 500/3.5 = 143/s.
// 12/frame is 120/s at 10fps (fits) but 240/s at 20fps - the pool saturates, the
// `if (P==PE) break` silently stops spawning and the stream stutters.
// Structure now follows master (DEMO/FOUNTAIN.CPP): ONE shared carry computed once, then
// each emitter spawns Spwn2*3. Rate is calibrated to the ORIGINAL DOS density (40*3 = 120/s
// per emitter = the old 12/frame at 10fps), NOT master's 360/s - master runs a 1500-particle
// pool per emitter, the DOS build keeps 500 and cannot sustain that.
#define FntSpawnInPcl 40.0

#define FntSpawnOutDisp 1.0
#define FntSpawnOutPow 28.0
#define FntSpawnOutPowTilt 0.0
#define FntSpawnOutPowRand 23.5
#define FntSpawnOutPowFreq 0.005

#define FntSpawnOutGrav 14.0

#define FntLightningTime1 500
#define FntLightningTime2 1500
#define FntLightningTime3 2200
#define FntLightningTime4 2700
#define FntLightningTime5 3200
#define FntLightningTime6 3500

#define FntLightningPcls  400

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