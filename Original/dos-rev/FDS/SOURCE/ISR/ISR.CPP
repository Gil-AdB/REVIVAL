#include <StdIO.H>
#include <StdLib.H>
#include <ConIO.H>
#include <DOS.H>


#include "..\FDS_Defs.H"
#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"

//Externals
long Timer,Old_Int_Count,Old_Int_Rate;
char Keyboard[128];
long KeyTT;
long Timer_Freq = 18; //Timer frequency in Hertz

char *Keybuf;
long Kbsize = 255,Kbpos = 0;
char LastKey = 0;

void (__interrupt __far *Prev_Int_8)();
void __interrupt __far Timer_Rtn()
{
  Timer++;
  Old_Int_Count++;
  _chain_intr( Prev_Int_8 );
//  outp(0x20,0x20);
}

void Install_8()
{
   Prev_Int_8 = _dos_getvect( 0x8 );
   _dos_setvect( 0x8, Timer_Rtn );
   Timer = 0;
   Old_Int_Count = 0;
   Keybuf = new char[2560];
}

void Restore_8()
{
   _dos_setvect( 0x8, Prev_Int_8 );
   Timer_Freq = 18;
}

void Set_Rate(unsigned int Rate)
{
   outp(0x43, 0x34);
   outp(0x40, Rate && 0x00ff);
   outp(0x40, Rate >> 8);
   Timer_Freq = Rate_Div/Rate;
//   Old_Int_Rate = (Rate_Div / Rate) / 18.20676;
   Old_Int_Rate = 54;
}

char Read_KBStack()
{
  long I;
  char c;
  if (!Kbpos) return 0;
  c = Keybuf[0];
  for(I=0;I<Kbpos-1;I++)
    Keybuf[I] = Keybuf[I+1];
  Kbpos--;
  return c;
}

void (__interrupt __far *Prev_Int_9)();

void (__interrupt __far Keyboard_handler )()
{
  char kbrecieve,I;
  kbrecieve = inp(0x60);
  if (kbrecieve<128)
  {
    Keyboard[kbrecieve] = 1;
/*    if (Kbpos<Kbsize)
    {
      Keybuf[Kbpos] = kbrecieve;
      Kbpos++;
    }
    KeyTT = Timer;
    LastKey = kbrecieve;*/
  }
  else Keyboard[kbrecieve-128] = 0;

/*  if (Keyboard[ScESC]==1) {
  Terminate_Engine();
  exit(0);
  }*/

/*  if (Keyboard[LastKey]&&(Timer-KeyTT)>Timer_Freq*1.0)
  {
    if (Kbpos<Kbsize)
    {
      Keybuf[Kbpos] = kbrecieve;
      Kbpos++;
    }
  }*/
  outp(0x20,0x20);
}

void Install_9()
{
  Prev_Int_9 = _dos_getvect( 0x9 );
  _dos_setvect( 0x9, Keyboard_handler );
  KeyTT=0;
}

void Restore_9()
{
  _dos_setvect( 0x9, Prev_Int_9 );
}

void Keyboard_HTest()
{
  long I;
  for(I=0;I<128;I++)
    if (Keyboard[I]) VPage[I]=63;
}

char Keypressed()
{
  long I;
  char C;
  C = 0;
  I=128;
  while (I--) if (Keyboard[I]) C = 1;
  return(C);
}
