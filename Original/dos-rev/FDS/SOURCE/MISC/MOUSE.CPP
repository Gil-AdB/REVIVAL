#include <Math.H>
#include <String.H>
#include <ConIO.H>
#include <StdLib.H>
#include <StdIO.H>
#include <DOS.H>
#include <i86.H>

#include "..\FDS_Defs.H"
#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"

static long M_x,M_y;

void InitMouse()
{
  union REGS r;

  // Reset mouse and recieve info
  r.w.ax = 0x0000;
  int386(0x33,&r,&r);

  if (r.w.ax!=0xFFFF)
  {
    printf("Shieat! mouse and/or driver unavailable.\n");
  }

  switch (r.w.bx)
  {
    case 0: printf("Abnormal amount of keys.\n"); break;
    case 2: printf("Two-keys mouse detected.\n"); break;
    case 3: printf("Three-keys mouse detected.\n"); break;
    case 0xFFFF: printf("Std. mouse detected.\n");
  }

  // quad mickey/sample rate
  r.w.ax = 0x000F;
  r.w.cx = 1;  //def = 8
  r.w.dx = 2;  //def = 16
  int386(0x33,&r,&r);

  M_x = M_y = 0;
}

void ShowMouse()
{
  union REGS r;
  r.w.ax = 0x0001;
  int386(0x33,&r,&r);
}

void HideMouse()
{
  union REGS r;
  r.w.ax = 0x0002;
  int386(0x33,&r,&r);
}

void UpdateMousePos()
{
  union REGS r;

  long x,y;
  r.w.ax = 0x000B;
  int386(0x33,&r,&r);
  x = (short)r.w.cx;
  y = (short)r.w.dx;

  M_x += x;
  M_y += y;

  // clip to v.surf
  if (M_x<0) M_x = 0;
  if (M_y<0) M_y = 0;
  if (M_x>XRes_1) M_x = XRes_1;
  if (M_y>YRes_1) M_y = YRes_1;
}

void DisplayMouse()
{
  UpdateMousePos();
  switch (BPP)
  {
    case 8: VPage[M_x+YOffs[M_y]] = 0xFF; break;
    case 16: ((Word *)VPage)[M_x+YOffs[M_y]] = 0xFFFF; break;
    case 32: ((DWord *)VPage)[M_x+YOffs[M_y]] = 0xFFFFFFFF; break;
  }
}