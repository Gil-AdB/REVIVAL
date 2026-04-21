/*

		 Flood Demo System - LightWave scene reader
		 ------------------------------------------

*/

#pragma pack (1)				// No data alligning

#include <Memory.H>
#include <Math.H>
#include <StdLib.H>
//#include <DOS.H>
#include <StdIO.H>
#include <IO.H>
#include <string.h>

//#define ObjectDebugMode
//#define SceneDebugMode
//#define SaveDebugMode

#include "LWREAD.H"			// Header file


void SaveFLD(FldScene *Scene, char *FileName);
void ReadLWObject(char *FileName, FldObject **O);

void SwapYZ(Vector *V)
{
/*	float Temp;
	Temp=V->Y;
	V->Y=V->Z;
	V->Z=Temp;*/
}

char *DestroyPath(char *FName)
{
	char *Temp;
	int a=strlen(FName),b=0;

	Temp=new char[256];
	memset(Temp,0,256);
	while (a-- && FName[a]!='\\' && FName[a]!='/') Temp[b++]=FName[a];
	for (a=0;a<b;a++) FName[a]=Temp[b-a-1]; // Flip it over!
	FName[a]=0;
	return FName;
}

//#include "LWORead.CPP"  // Object reader
//#include "FLDSave.CPP"	// Flood file saver

FILE		 	*LWS;   			// Current scene file
FldScene 	*Scene;       // Current scene
FldObject *CurObj;			// Current object
FldLight	*CurLight;		// Current light
FldCamera	*Camera;			// Current camera
char			 ReadNext;

char 	 *ReadWord(void);		    // Forward declaration
char 	 *ReadString(void);	    // Forward declaration
FldEnv *ReadEnvelope(unsigned int i); // Forward decleration

void NextLine(void)
{
char ch;

	ch=fgetc(LWS);
	while (ch!=10 && !feof(LWS)) ch=fgetc(LWS);
}

void ReadFirstFrame(void)
{
	fscanf(LWS,"%i",&Scene->FirstFrame);
#ifdef SceneDebugMode
	printf("First frame: %i\n",Scene->FirstFrame);
#endif
}

void ReadLastFrame(void)
{
	fscanf(LWS,"%i",&Scene->LastFrame);
#ifdef SceneDebugMode
	printf("Last frame: %i\n",Scene->LastFrame);
#endif
}

void ReadFrameStep(void)
{
	fscanf(LWS,"%i",&Scene->FrameStep);
#ifdef SceneDebugMode
	printf("Frame step: %i\n",Scene->FrameStep);
#endif
}

void ReadFPS(void)
{
	fscanf(LWS,"%f",&Scene->FramesPerSecond);
#ifdef SceneDebugMode
	printf("Frames per second: %f\n",Scene->FramesPerSecond);
#endif
}

static int objNum = 1;
static int lightNum = 1;

void AddObject(void)
{
char *ObjName;

	ObjName=ReadString();
	ObjName=DestroyPath(ObjName);
	printf("Reading object: %d \"%s\"...\n",objNum++, ObjName);
	if (!Scene->Object)
	{
		Scene->Object=new FldObject;
		CurObj=Scene->Object;
	}
	else
	{
		CurObj->Next=new FldObject;
		CurObj=CurObj->Next;
	}
	memset(CurObj,0,sizeof(FldObject));
	CurObj->Name=ObjName;
	if (!Scene->Object) Scene->Object=CurObj;
	ReadLWObject(ObjName,&CurObj);
}

/*void AddNullObject(void)
{
char *ObjName;

	ObjName=ReadWord();
	printf("Adding NULL object: \"%s\"\n",ObjName);
	if (!Scene->Object)
	{
		Scene->Object=new FldObject;
		CurObj=Scene->Object;
	}
	else
	{
		CurObj->Next=new FldObject;
		CurObj=CurObj->Next;
	}
	if (!Scene->Object) Scene->Object=CurObj;
	memset(CurObj,0,sizeof(FldObject));
	CurObj->Name=ObjName;
}*/

void AddNullObject(void)
{
	char *ObjName;

	ObjName=ReadString();
	ObjName=DestroyPath(ObjName);
	printf("Reading object: %d \"%s\"...\n",objNum++, ObjName);
	if (!Scene->Object)
	{
		Scene->Object=new FldObject;
		CurObj=Scene->Object;
	}
	else
	{
		CurObj->Next=new FldObject;
		CurObj=CurObj->Next;
	}
	memset(CurObj,0,sizeof(FldObject));
	CurObj->Name=ObjName;
	if (!Scene->Object) Scene->Object=CurObj;
}

void ReadObjectMotion(void)
{
unsigned long Keys,Channels;
unsigned long a,b;

	NextLine();
	fscanf(LWS,"%i",&Channels);
	NextLine();
	fscanf(LWS,"%i",&Keys);
	NextLine();
	CurObj->KF=new FldKeyFrame[Keys];
	CurObj->Keys=Keys;
	for (a=0;a<Keys;a++)
	{
		fscanf(LWS,"%f",&CurObj->KF[a].Position.X);
		fscanf(LWS,"%f",&CurObj->KF[a].Position.Y);
		fscanf(LWS,"%f",&CurObj->KF[a].Position.Z);
		fscanf(LWS,"%f",&CurObj->KF[a].Rotation.X);
		fscanf(LWS,"%f",&CurObj->KF[a].Rotation.Y);
		fscanf(LWS,"%f",&CurObj->KF[a].Rotation.Z);
		fscanf(LWS,"%f",&CurObj->KF[a].Scale.X);
		fscanf(LWS,"%f",&CurObj->KF[a].Scale.Y);
		fscanf(LWS,"%f",&CurObj->KF[a].Scale.Z);
		SwapYZ(&CurObj->KF[a].Position);
		SwapYZ(&CurObj->KF[a].Rotation);
		SwapYZ(&CurObj->KF[a].Scale);
		fscanf(LWS,"%f",&CurObj->KF[a].FrameNumber);
		fscanf(LWS,"%f",&CurObj->KF[a].LinearValue);
		fscanf(LWS,"%f",&CurObj->KF[a].Tension);
		fscanf(LWS,"%f",&CurObj->KF[a].Continuity);
		fscanf(LWS,"%f",&CurObj->KF[a].Bias);

#ifdef SceneDebugMode
	printf("Frame: %.2f\n",CurObj->KF[a].FrameNumber);
	printf("Position: X:%.3f  Y:%.3f  Z:%.3f\n",
					CurObj->KF[a].Position.X,
					CurObj->KF[a].Position.Y,
					CurObj->KF[a].Position.Z);
	printf("Rotation: X:%.3f  Y:%.3f  Z:%.3f\n",
					CurObj->KF[a].Rotation.X,
					CurObj->KF[a].Rotation.Y,
					CurObj->KF[a].Rotation.Z);
	printf("Scale:    X:%.3f  Y:%.3f  Z:%.3f\n",
					CurObj->KF[a].Scale.X,
					CurObj->KF[a].Scale.Y,
					CurObj->KF[a].Scale.Z);
	printf("Linear value: %.3f\n",CurObj->KF[a].LinearValue);
	printf("Spline info:Tension: %.3f  Continuity: %.3f  Bias: %.3f\n",
					CurObj->KF[a].Tension,CurObj->KF[a].Continuity,CurObj->KF[a].Bias);
#endif
	}
}

void ReadEndBehavior(void)
{
unsigned int Temp;

	fscanf(LWS,"%i",&Temp);
	switch (Temp)
	{
		case 0: Temp=256;	 break;
		case 1: Temp=512;	 break;
		case 2: Temp=1024; break;
	}
#ifdef SceneDebugMode
	printf("End behavior: %i\n",Temp);
#endif
	if ((!Scene->Camera) && (!Scene->Light)) // Still in object section
		CurObj->Flags|=Temp;
	else if (!Scene->Camera)						 		 // Still in light section
		CurLight->Flags|=Temp;
	else
		Camera->Flags|=Temp;
}

void ReadPivotPoint(void)
{
	fscanf(LWS,"%f %f %f",
				&CurObj->PivotPoint.X,&CurObj->PivotPoint.Y,&CurObj->PivotPoint.Z);
	CurObj->Flags|=2;	// Object has a pivot point
	SwapYZ(&CurObj->PivotPoint);
#ifdef SceneDebugMode
	printf("Pivot point: X:.3%f Y:%.3f %.3f\n",
				 CurObj->PivotPoint.X,CurObj->PivotPoint.Y,CurObj->PivotPoint.Z);
#endif
}

void ReadPathAlignment(void)
{
	unsigned int Temp;

	fscanf(LWS, "%i", &Temp);

	if (Temp == 1) {
		Temp = Object_AlignToPath;
		if ((!Scene->Camera) && (!Scene->Light)) // Still in object section
			CurObj->Flags |= Temp;
		else if (!Scene->Camera)						 		 // Still in light section
			CurLight->Flags |= Temp;
		else
			Camera->Flags |= Temp;
	}
#ifdef SceneDebugMode
	printf("Path Alignment: %i\n", Temp);
#endif
}


void ReadParentObject(void)
{
	unsigned int Temp;
	fscanf(LWS,"%i",&Temp);
#ifdef SceneDebugMode
	printf("Parent: %i\n",Temp);
#endif
	if ((!Scene->Camera) && (!Scene->Light)) // Still in object section
	{
		CurObj->Parent=Temp;
		CurObj->Flags|=1; 		// Object has parent
	}
	else if (!Scene->Camera)						 		 // Still in light section
	{
		CurLight->Parent=Temp;
		CurLight->Flags|=1; 	// Light has parent
	}
	else
	{
		Camera->Parent=Temp;
		Camera->Flags|=1;     // Camera has parent
	}
}

void ReadPolygonSize(void)
{
//	fscanf(LWS,"%f",&CurObj->PolygonSize);
#ifdef SceneDebugMode
	printf("Polygon size: \n"); //,CurObj->PolygonSize);
#endif
	CurObj->Flags|=4;	// Object has a polygon size
	CurObj->PolygonSize=ReadEnvelope(1);
}

void ReadAmbientColor(void)
{
#ifdef SceneDebugMode
	printf("Ambient color: ");
#endif
	Scene->AmbientColor=ReadEnvelope(3);
}

void ReadAmbientIntensity(void)
{
#ifdef SceneDebugMode
	printf("Ambient intensity: ");
#endif
	Scene->AmbientIntensity=ReadEnvelope(1);
}

void AddLight(void)
{
#ifdef SceneDebugMode
	printf("Adding light...\n");
#endif
	if (!Scene->Light)
	{
		Scene->Light=new FldLight;
		CurLight=Scene->Light;
	}
	else
	{
		CurLight->Next=new FldLight;
		CurLight=CurLight->Next;
	}
	memset(CurLight,0,sizeof(FldLight));
	ReadNext=0;
}

void ReadLightName(void)
{
	CurLight->Name=ReadString();

	printf("Reading Light: %d \"%s\"...\n", lightNum++, CurLight->Name);

#ifdef SceneDebugMode
	printf("Light name: %s\n",CurLight->Name);
#endif
}

void ReadLightMotion(void)
{
unsigned long Keys,Channels;
unsigned long a,b;

	NextLine();
	fscanf(LWS,"%i",&Channels);
	NextLine();
	fscanf(LWS,"%i",&Keys);
	NextLine();
	CurLight->KF=new FldKeyFrame[Keys];
	CurLight->Keys=Keys;
	for (a=0;a<Keys;a++)
	{
		fscanf(LWS,"%f",&CurLight->KF[a].Position.X);
		fscanf(LWS,"%f",&CurLight->KF[a].Position.Y);
		fscanf(LWS,"%f",&CurLight->KF[a].Position.Z);
		fscanf(LWS,"%f",&CurLight->KF[a].Rotation.X);
		fscanf(LWS,"%f",&CurLight->KF[a].Rotation.Y);
		fscanf(LWS,"%f",&CurLight->KF[a].Rotation.Z);
		fscanf(LWS,"%f",&CurLight->KF[a].Scale.X);
		fscanf(LWS,"%f",&CurLight->KF[a].Scale.Y);
		fscanf(LWS,"%f",&CurLight->KF[a].Scale.Z);
		SwapYZ(&CurLight->KF[a].Position);
		SwapYZ(&CurLight->KF[a].Rotation);
		SwapYZ(&CurLight->KF[a].Scale);

		fscanf(LWS,"%f",&CurLight->KF[a].FrameNumber);
		fscanf(LWS,"%f",&CurLight->KF[a].LinearValue);
		fscanf(LWS,"%f",&CurLight->KF[a].Tension);
		fscanf(LWS,"%f",&CurLight->KF[a].Continuity);
		fscanf(LWS,"%f",&CurLight->KF[a].Bias);

#ifdef SceneDebugMode
	printf("Frame: %.2f\n",CurObj->KF[a].FrameNumber);
	printf("Position: X:%.3f  Y:%.3f  Z:%.3f\n",
					CurLight->KF[a].Position.X,
					CurLight->KF[a].Position.Y,
					CurLight->KF[a].Position.Z);
	printf("Rotation: X:%.3f  Y:%.3f  Z:%.3f\n",
					CurLight->KF[a].Rotation.X,
					CurLight->KF[a].Rotation.Y,
					CurLight->KF[a].Rotation.Z);
	printf("Scale:    X:%.3f  Y:%.3f  Z:%.3f\n",
					CurLight->KF[a].Scale.X,
					CurLight->KF[a].Scale.Y,
					CurLight->KF[a].Scale.Z);
	printf("Linear value: %.3f\n",CurLight->KF[a].LinearValue);
	printf("Spline info:Tension: %.3f  Continuity: %.3f  Bias: %.3f\n",
					CurLight->KF[a].Tension,CurLight->KF[a].Continuity,CurLight->KF[a].Bias);
#endif
	}
}

void ReadTargetObject(void)
{
	int Temp;
	fscanf(LWS,"%i",&Temp);
	if (!Scene->Camera)
	{
		CurLight->TargetObject=Temp;
		CurLight->Flags|=2; 	// Light has a target object
	}
	else
	{
		Camera->TargetObject=Temp;
		Camera->Flags|=2;			// Camera has a target object
	}
#ifdef SceneDebugMode
	printf("Target object: %i\n",Temp);
#endif
}

void ReadLightColor(void)
{
#ifdef SceneDebugMode
	printf("Light color: ");
#endif
	int R, G, B;
	fscanf(LWS, "%i", &R);
	fscanf(LWS, "%i", &G);
	fscanf(LWS, "%i", &B);

	CurLight->Color.Red	= R;
	CurLight->Color.Green	= G;
	CurLight->Color.Blue	= B;
}

void ReadLightIntensity(void)
{
#ifdef SceneDebugMode
	printf("Light intensity: ");
#endif
	CurLight->Intensity=ReadEnvelope(1);
}

void ReadLightType(void)
{
	int Temp;

	fscanf(LWS,"%i",&Temp);
	switch (Temp)
	{
		case 0:	CurLight->Flags|=32; break;
		case 1:	CurLight->Flags|=64; break;
		case 2:	CurLight->Flags|=128; break;
	}
#ifdef SceneDebugMode
	printf("Light type: %i\n",Temp);
#endif
}

void ReadLightFalloff(void)
{
#ifdef SceneDebugMode
	printf("Light falloff: ");
#endif
	int Temp;

	CurLight->Falloff=ReadEnvelope(1); // not an envelope, an integer. fixed in read envelope
	CurLight->Flags|=4;
}

void ReadLightConeAngle(void)
{
#ifdef SceneDebugMode
	printf("Light cone angle: ");
#endif
	CurLight->ConeAngle=ReadEnvelope(1);
	CurLight->Flags|=8;
}

void ReadLightRange(void)
{
#ifdef SceneDebugMode
	printf("Light Range: ");
#endif
	CurLight->Range = ReadEnvelope(1);
	CurLight->Flags |= 16;
}


void ReadLightLensFlareFlag(void)
{
int Temp;

	fscanf(LWS,"%i",&Temp);
	CurLight->Flags |= Light_LensFlare;//Temp<<4;
#ifdef SceneDebugMode
	printf("Light lens-flare flag: %i\n",Temp);
#endif
}

void ReadCameraMotion(void)
{
unsigned long Keys,Channels;
unsigned long a,b;

	printf("Adding camera...\n");
	Camera=new FldCamera;
	memset(Camera,0,sizeof(FldCamera));
	Scene->Camera=Camera;
	NextLine();
	fscanf(LWS,"%i",&Channels);
	NextLine();
	fscanf(LWS,"%i",&Keys);
	NextLine();
	Camera->KF=new FldKeyFrame[Keys];
	Camera->Keys=Keys;
	for (a=0;a<Keys;a++)
	{
		fscanf(LWS,"%f",&Camera->KF[a].Position.X);
		fscanf(LWS,"%f",&Camera->KF[a].Position.Y);
		fscanf(LWS,"%f",&Camera->KF[a].Position.Z);
		fscanf(LWS,"%f",&Camera->KF[a].Rotation.X);
		fscanf(LWS,"%f",&Camera->KF[a].Rotation.Y);
		fscanf(LWS,"%f",&Camera->KF[a].Rotation.Z);
		fscanf(LWS,"%f",&Camera->KF[a].Scale.X);
		fscanf(LWS,"%f",&Camera->KF[a].Scale.Y);
		fscanf(LWS,"%f",&Camera->KF[a].Scale.Z);
		SwapYZ(&Camera->KF[a].Position);
		SwapYZ(&Camera->KF[a].Rotation);
		SwapYZ(&Camera->KF[a].Scale);

		fscanf(LWS,"%f",&Camera->KF[a].FrameNumber);
		fscanf(LWS,"%f",&Camera->KF[a].LinearValue);
		fscanf(LWS,"%f",&Camera->KF[a].Tension);
		fscanf(LWS,"%f",&Camera->KF[a].Continuity);
		fscanf(LWS,"%f",&Camera->KF[a].Bias);

#ifdef SceneDebugMode
	printf("Frame: %.2f\n",CurObj->KF[a].FrameNumber);
	printf("Position: X:%.3f  Y:%.3f  Z:%.3f\n",
					Camera->KF[a].Position.X,
					Camera->KF[a].Position.Y,
					Camera->KF[a].Position.Z);
	printf("Rotation: X:%.3f  Y:%.3f  Z:%.3f\n",
					Camera->KF[a].Rotation.X,
					Camera->KF[a].Rotation.Y,
					Camera->KF[a].Rotation.Z);
	printf("Scale:    X:%.3f  Y:%.3f  Z:%.3f\n",
					Camera->KF[a].Scale.X,
					Camera->KF[a].Scale.Y,
					Camera->KF[a].Scale.Z);
	printf("Linear value: %.3f\n",Camera->KF[a].LinearValue);
	printf("Spline info:Tension: %.3f  Continuity: %.3f  Bias: %.3f\n",
					Camera->KF[a].Tension,Camera->KF[a].Continuity,Camera->KF[a].Bias);
#endif
	}
}

void ReadCameraZoomFactor(void)
{
#ifdef SceneDebugMode
	printf("Zoom factor: ");
#endif
	Camera->ZoomFactor=ReadEnvelope(1);
}

struct
{
	const char *ID;
	void (*Reader)(void);
} Function[]=
{
// Scene section
	{"FirstFrame"	 ,		ReadFirstFrame},
	{"PreviewLastFrame"		 , ReadLastFrame},
	{"FrameStep"			 , ReadFrameStep},
	{"FramesPerSecond" , ReadFPS},
// Object section
	{"LoadObject"			 , AddObject},
	{"AddNullObject"   , AddNullObject},
	{"ObjectMotion"		 , ReadObjectMotion},
	{"EndBehavior"		 , ReadEndBehavior},	// Also refers to lights and camera
	{"PivotPoint"			 , ReadPivotPoint},
	{"ParentObject"		 , ReadParentObject}, // Also refers to lights and camera
	{"PolygonSize"		 , ReadPolygonSize},  			// Envelope
	{"PathAlignment"	 , ReadPathAlignment},
	// Light section
	{"AmbientColor"		 , ReadAmbientColor},				// Envelope
	{"AmbIntensity"		 , ReadAmbientIntensity}, 	// Envelope
	{"AddLight"				 , AddLight},
	{"LightName"			 , ReadLightName},
	{"LightMotion"		 , ReadLightMotion},
	{"TargetObject"		 , ReadTargetObject}, // Also refers to camera
	{"LightColor"			 , ReadLightColor},         // Envelope
	{"LgtIntensity"		 , ReadLightIntensity},     // Envelope
	{"LightType"			 , ReadLightType},
	{"LightFalloff"				 , ReadLightFalloff},      	// Envelope
	{"ConeAngle"			 , ReadLightConeAngle},    	// Envelope
	{"LightRange"				, ReadLightRange},    	// Envelope
	{"LensFlare" 			 , ReadLightLensFlareFlag},
// Camera section
	{"CameraMotion"		 , ReadCameraMotion},
	{"ZoomFactor"			 , ReadCameraZoomFactor},   // Envelope

};

FldEnv *ReadEnvelope(unsigned int i)
{
unsigned long Keys,Channels;
unsigned long a,b,j;
char *Temp,*Temp2=NULL;
FldEnv *Env;

	Env=new FldEnv;
	memset(Env,0,sizeof(FldEnv));

	Temp=new char[256];
	Temp[0]=fgetc(LWS);
	Temp[1]=fgetc(LWS);
	Temp[2]=0;
	if (Temp[1]!='(')	// This is NOT an envelope!
	{
		if (Temp[1]!=' ' && Temp[1] != '\n')
		{
			Temp2=ReadWord();
			strcat(Temp,Temp2);
		}
		Env->Key=new FldEnvKey;
		Env->Channels = 1;
		memset(Env->Key,0,sizeof(FldEnvKey));
		Env->Key->Channel=new float[i];
		Env->Keys=1;
		Env->Key->Channel[0]=atof(Temp);
		Env->EndBehavior=512;
		for (j=1;j<i;j++) fscanf(LWS,"%f",&Env->Key->Channel[j]);
		ReadNext=0;
#ifdef SceneDebugMode
		printf("Single key: ");
		for (j=0;j<i;j++) printf("%f ",Env->Key->Channel[j]);
		printf("\n");
#endif
		delete Temp;
		delete Temp2;
		return Env;
	}
#ifdef SceneDebugMode
	printf("Envelope:\n");
#endif
	delete Temp;
	NextLine();
	fscanf(LWS,"%i",&Channels);
	NextLine();
	fscanf(LWS,"%i",&Keys);
	NextLine();
	if (Channels!=i) printf("**Warning! Incompatible channels!\n**");
	Env->Key=new FldEnvKey[Keys];
	for (j=0;j<Keys;j++)
		Env->Key[j].Channel=new float[Channels];
	Env->Keys=Keys;
	Env->Channels=Channels;
	for (a=0;a<Keys;a++)
	{
#ifdef SceneDebugMode
	printf("Key %i: ",a+1);
#endif
		for (b=0;b<Channels;b++)
		{
			fscanf(LWS,"%f",&Env->Key[a].Channel[b]);
#ifdef SceneDebugMode
	printf("#%i: %.3f",b+1,Env->Key[a].Channel[b]);
#endif
			printf("\n");
		}
		fscanf(LWS,"%f",&Env->Key[a].FrameNumber);
		fscanf(LWS,"%f",&Env->Key[a].LinearValue);
		fscanf(LWS,"%f",&Env->Key[a].Tension);
		fscanf(LWS,"%f",&Env->Key[a].Continuity);
		fscanf(LWS,"%f",&Env->Key[a].Bias);
#ifdef SceneDebugMode
	printf("     Frame: %.2f\n",Env->Key[a].FrameNumber);
	printf("     Linear value: %.3f\n",Env->Key[a].LinearValue);
	printf("     Spline info:Tension: %.3f  Continuity: %.3f  Bias: %.3f\n",
					Env->Key[a].Tension,Env->Key[a].Continuity,Env->Key[a].Bias);
#endif
	}
	NextLine();
	Temp=ReadWord();
	if (strcmp(Temp,"EndBehavior")!=0)
		printf("**Warning! End behavior read error!**\n");
	fscanf(LWS,"%i",&Env->EndBehavior);
	switch (Env->EndBehavior)
	{
		case 0: Env->EndBehavior=256;  break;
		case 1: Env->EndBehavior=512;  break;
		case 2: Env->EndBehavior=1024; break;
	}
#ifdef SceneDebugMode
	printf("End behavior: %i\n",Env->EndBehavior);
#endif
	return Env;
}

signed char FindID(char *ID)
{
int a;

	for (a=0;a<sizeof(Function)/sizeof(Function[0]);a++)
		if (strcmp(Function[a].ID,ID)==0)
			return a;
	return -1;
}

char *ReadWord(void)
{
char *Temp;
char  ch;
int   a=0;

	Temp=new char[256];
	memset(Temp,0,256);
	ch=fgetc(LWS);
	while (ch!=32 && ch!=13 && ch!=10 && a<254)
	{
		Temp[a++]=ch;
		ch=fgetc(LWS);
	}
	Temp[a]=0;
	Temp[a+1]=ch;
	return Temp;
}

char *ReadString(void)
{
char *Temp;
char  ch;
int   a=0;

	Temp=new char[256];
	memset(Temp,0,256);
	ch=fgetc(LWS);
	while (ch!=13 && ch!=10 && a<255)
	{
		Temp[a++]=ch;
		ch=fgetc(LWS);
	}
	Temp[a]=0;
	return Temp;
}

void ReadSceneData(void)
{
char 			 *ID = NULL;
signed int CurFunc;

	while (!feof(LWS))
	{
		ID=ReadWord();
		CurFunc=FindID(ID);
		if (CurFunc>=0)
		{
			Function[CurFunc].Reader();
		// Some functions don't require next line reading, i.e. "AddLight"
		}
		if (ReadNext && strcmp(ID, "") != 0) NextLine(); else ReadNext = 1;
	}
	delete(ID);
}

void ReadLWScene(char *FileName)
{
char Signature[5];
unsigned int Version;

	printf("Parsing scene file: \"%s\"...\n",FileName);
	if (!(LWS=fopen(FileName,"rt")))
	{
		fclose(LWS);
		printf("Error! Unable to open scene file \"%s\"!\n",FileName);
		exit(1);
	}
	Scene=new FldScene;
	memset(Scene,0,sizeof(FldScene));
// Read signature
	fscanf(LWS,"%4c\n",Signature);
	Signature[4]=0;
	if (strcmp(Signature,"LWSC")!=0)
	{
		printf("Bad scene file!\n");
		exit(2);
	} else printf("Scene file signature confirmed...\n");
	fscanf(LWS,"%i\n",&Version);
#ifdef SceneDebugMode
	printf("Scene file version: %i\n",Version);
#endif
	ReadNext=1;
	ReadSceneData();
	printf("\nDone reading scene file!\n");
}

int main(int argc,char *argv[])
{
	printf("\n\nFlood's Lightwave reader in action...\n\n");
	if (argc<=1)
	{
		printf("  Syntax: LWSRead <Scene file> [<Flood file>]\n");
		printf("  LWSRead reads a lightwave scene+object files.\n");
		printf("  If <Flood file> is specified (.FLD extension), LWSRead will\n");
		printf("  convert the scene file to the flood file specified\n");
		printf("  I.E.: LWSRead MyScene.LWS NewScene.FLD\n");
		printf("  Note! Make sure that all objects are in the current directory\n");
		printf("        and prevent the usage of M$'s Winzol95 long file names\n");
		exit(1);
	}
	ReadLWScene(argv[1]);
	if (argc>2) SaveFLD(Scene,argv[2]);
}
