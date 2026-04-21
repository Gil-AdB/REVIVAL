// Silvatar's DPMI based Memory manager. Requires a previous DPMI host,
// aka. 386 Integrated DOS-Extender. DPMI unit is not selfcontained.
// Version 1.25

// I'll prolly insert HEAP management soon.

// Ver 1.00 --> 1.10 : Aligned buffer support,Fixed a bug in Linked List.
// Ver 1.10 --> 1.12 : Fixed a bug in module deallocation process.
// Ver 1.12 --> 1.15 : Added DOS/Conv. memory management. Lame,but it works.
// Ver 1.15 --> 1.2  : Normal calling conventions.
// Ver 1.2  --> 1.21 : Added VCPI->DPMI mapping functions,and removed
//                     Selector manipulation functions.
// Ver 1.21 --> 1.25 : Win95 Compatability (hope so)
// Remaining probs : prolly linked list is still buggy, fix it,
//                   and aligned buffer should be more dynamic.
//                   For some reason, modular unit takes 32Kb without giving it back
//                   in the end of the dealloc process.

#include <Mem.H>
#include <DOS.H>
#include <StdLib.H>
#include <StdIO.H>

#include "..\FDS_Defs.H"
#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"


Module_MB *DPMI_Core;
DWord DPMI_Core_Handle,DPMI_Index,DPMI_InitFlag;
void *DPMI_Main;
Module_MB *DPMI_MBPool;
Module_MB *DPMI_Free; //Directly points to the next free block
char *DPMI_HeapSlack,*DPMI_Aligned;
Aligned_MB *DPMI_AMB,*DPMI_AFree;


void * D_New(DWord Size)
{
  union REGS R;
  char Msg[160];
  long Address;
  memset(&R,0,sizeof(REGS));
  Size += 4;
  R.w.ax = 0x501;
  R.w.bx = Size>>16;
  R.w.cx = Size&0xFFFF;
  int386(0x31,&R,&R);
  if (R.w.cflag)
  {
    DPMI_Warning(Msg);
    return NULL;
  }
  Address = (R.w.bx<<16)+R.w.cx;

  *((long *)Address) = (R.w.si<<16)+R.w.di;
  return (void *)(Address+4);
}

void D_Del(void *Target)
{
  union REGS R;
  char Msg[160];
  long Handle = *(((long *)Target)-1);
  memset(&R,0,sizeof(REGS));
  R.w.ax = 0x502;
  R.w.si = Handle>>16;
  R.w.di = Handle&0xFFFF; // &0xFFFF can be removed
  int386(0x31,&R,&R);
  if (R.w.cflag)
  {
    DPMI_Warning("Failed while attempting to free a block.");
    return;
  }
}

void * D_Resize(void *Target,DWord Size)
{
  union REGS R;
  char Msg[160];
  DWord Address;
  memset(&R,0,sizeof(REGS));
  Size+=4;
  R.w.ax = 0x503;
  R.w.bx = Size>>16;
  R.w.cx = Size&0xFFFF; // &0xFFFF can be removed
  Address = *(((long *)Target)-1); // Get previous handle
  R.w.si = Address>>16;
  R.w.di = Address&0xFFFF;
  int386(0x31,&R,&R);
  if (R.w.cflag)
  {
    sprintf(Msg,"Failed while attempting to Resize a memory block to %d. (FREE=%d)",Size-4,DPMI_Free_Memory());
    DPMI_Warning(Msg);
    return NULL;
  }
  Address = (R.w.bx<<16)+R.w.cx;
  *((long *)Address) = (R.w.si<<16)+R.w.di;
  return (char *)(Address+4);
}



void * Memory_Allocate(Module_MB *MB,DWord Size)
{
  union REGS R;
  char Msg[160];
  if (MB->Size)
  {
    sprintf(Msg,"Module %s already allocated, ignoring request.",MB->Name);
    DPMI_Warning(Msg);
    return (char *)MB->Address;
  }

  memset(&R,0,sizeof(REGS));
  R.w.ax = 0x501;
  R.w.bx = Size>>16;
  R.w.cx = Size&0xFFFF; // &0xFFFF can be removed
  int386(0x31,&R,&R);
  if (R.w.cflag)
  {
    sprintf(Msg,"Failed while attempting to allocate %d Bytes for module %s,returning NULL.",Size,MB->Name);
    DPMI_Warning(Msg);
    return NULL;
  }
  MB->Address = (R.w.bx<<16)+R.w.cx;
  MB->Handle = (R.w.si<<16)+R.w.di;
  MB->Size = Size;
  sprintf(Msg,"Allocated %d Bytes for Module %s.",Size,MB->Name);
  DPMI_Message(Msg);
  return (char *)MB->Address;
}

void * Low_Memory_Allocate(Module_MB *MB,DWord Size)
{
  union REGS R;
  char Msg[320];
  DWord Segment,Selector;
  memset(&R,0,sizeof(REGS));
  R.w.ax = 0x100;
  R.w.bx = Size>>4;
  int386(0x31,&R,&R);
  if (R.w.cflag)
  {
    sprintf(Msg,"Failed to allocate LOW Memory block of %d Paragraphs.\n DOS -- ERROR code = %d,Largest block = %d.",Size,R.w.ax,R.w.bx);
    DPMI_Warning(Msg);
    return NULL;
  }
  Segment = R.w.ax;
  Selector = R.w.dx;
  memset(&R,0,sizeof(REGS));
  R.w.ax = 0x006;
  R.w.bx = Selector;
  int386(0x31,&R,&R);
  if (R.w.cflag)
  {
    DPMI_Warning("Could not convert Segmented memory Selector to Linear address.");
    return NULL;
  }
  MB->Address = (R.w.cx<<16)+R.w.dx;
  MB->Handle = (Selector<<16)+Segment;
  MB->Size = Size-(Size&0xF);
  sprintf(Msg,"Allocated %d Bytes for Module %s.",MB->Size,MB->Name);
  DPMI_Message(Msg);
  return (char *)MB->Address;
}

void Memory_Free(Module_MB *MB)
{
  union REGS R;
  char Msg[160];
  memset(&R,0,sizeof(REGS));
  R.w.ax = 0x502;
  R.w.si = MB->Handle>>16;
  R.w.di = MB->Handle&0xFFFF; // &0xFFFF can be removed
  int386(0x31,&R,&R);
  if (R.w.cflag)
  {
    sprintf(Msg,"Failed while attempting to free %d Bytes at module %s,aborting.",MB->Size,MB->Name);
    DPMI_Warning(Msg);
    return;
  }
  sprintf(Msg,"Freed %d Bytes at module %s",MB->Size,MB->Name);
  DPMI_Message(Msg);
  MB->Address = 0;
  MB->Handle = 0;
  MB->Size = 0;
}


void Low_Memory_Free(Module_MB *MB)
{
  union REGS R;
  char Msg[160];
  memset(&R,0,sizeof(REGS));
  R.w.ax = 0x101;
  R.w.dx = MB->Handle>>16; // Selector from Highword.
  int386(0x31,&R,&R);
  if (R.w.cflag)
  {
    sprintf(Msg,"Failed while attempting to free %d Bytes at LOW module %s,aborting.\n  DOS -- Error code - %d",MB->Size,MB->Name,R.w.ax);
    DPMI_Warning(Msg);
    return;
  }
  sprintf(Msg,"Freed %d Bytes at module %s",MB->Size,MB->Name);
  DPMI_Message(Msg);
  MB->Address = 0;
  MB->Handle = 0;
  MB->Size = 0;
}


char * Memory_Resize(Module_MB *MB,DWord Size)
{
  union REGS R;
  char Msg[160];
  memset(&R,0,sizeof(REGS));
  R.w.ax = 0x503;
  R.w.bx = Size>>16;
  R.w.cx = Size&0xFFFF; // &0xFFFF can be removed
  R.w.si = MB->Handle>>16;
  R.w.di = MB->Handle&0xFFFF;
  int386(0x31,&R,&R);
  if (R.w.cflag)
  {
    sprintf(Msg,"Failed while attempting to Resize module %s from %d to %d Bytes,aborting.",MB->Name,MB->Size,Size);
    DPMI_Warning(Msg);
    return NULL;
  }
  sprintf(Msg,"Resized module %s from %d to %d Bytes.",MB->Name,MB->Size,Size);
  MB->Size = Size;
  DPMI_Message(Msg);
  MB->Address = (R.w.bx<<16)+R.w.cx;
  MB->Handle = (R.w.si<<16)+R.w.di;
  return (char *)MB->Address;
}

void DPMI_Message(char *Msg)
{
  char *S=new char[10+strlen(Msg)];
  strcpy(S,"<DPMI>: ");
  strcat(S,Msg);
//  printf("%s\n",S);
  delete S;
}


void DPMI_Warning(char *ErrMsg)
{
  char *S=new char[17+strlen(ErrMsg)];
  strcpy(S,"<DPMI> Warning: ");
  strcat(S,ErrMsg);
//  printf("%s\n",S);
  delete S;
}


void DPMI_Error(char *ErrMsg)
{
  char *S=new char[21+strlen(ErrMsg)];
  strcpy(S,"<DPMI> Fatal Error: ");
  strcat(S,ErrMsg);
//  printf("%s\n",S);
  delete S;
  exit(-1);
}

DWord DPMI_Free_Memory(void)
{
  DWord Buffer[12]; //48 bytes for reserved stuff.
  union REGS  r;
  struct SREGS sr;

  memset(&r,0,sizeof(r));
  memset(&sr,0,sizeof(sr));

  r.x.eax = 0x500;
  sr.es = FP_SEG(Buffer);
  r.x.edi = FP_OFF(Buffer); //hey,it's sort of ES:EDI
  int386x(0x31,&r,&r,&sr);

  return Buffer[0];
}

void DPMI_allocRealSeg(int size,int *sel,int *r_seg)
{
  union REGS      r;

  r.w.ax = 0x100;                 // DPMI allocate DOS memory
  r.w.bx = (size + 0xF) >> 4;     // number of paragraphs
  int386(0x31, &r, &r);
  if (r.w.cflag)
    DPMI_Error("DPMI_allocRealSeg failed!");
  *sel = r.w.dx;                  // Protected mode selector
  *r_seg = r.w.ax;                // Real mode segment
}

void DPMI_freeRealSeg(unsigned sel)
{
  union REGS  r;

  r.w.ax = 0x101;                 // DPMI free DOS memory
  r.w.dx = sel;                   // DX := selector from 0x100
  int386(0x31, &r, &r);
}


#define IN(reg)     rmregs.e##reg = in->x.reg
#define OUT(reg)    out->x.reg = rmregs.e##reg

int DPMI_int86(int intno, RMREGS *in, RMREGS *out)
{
  _RMREGS         rmregs;
  union REGS      r;
  struct SREGS    sr;

  memset(&rmregs, 0, sizeof(rmregs));
  IN(ax); IN(bx); IN(cx); IN(dx); IN(si); IN(di);

  segread(&sr);
  r.w.ax = 0x300;                 // DPMI issue real interrupt
  r.h.bl = intno;
  r.h.bh = 0;
  r.w.cx = 0;
  sr.es = sr.ds;
  r.x.edi = (unsigned)&rmregs;
  int386x(0x31, &r, &r, &sr);     // Issue the interrupt

  OUT(ax); OUT(bx); OUT(cx); OUT(dx); OUT(si); OUT(di);
  out->x.cflag = rmregs.flags & 0x1;
  return out->x.ax;
}

int DPMI_int86x(int intno, RMREGS *in, RMREGS *out, RMSREGS *sregs)
{
  _RMREGS         rmregs;
  union REGS      r;
  struct SREGS    sr;

  memset(&rmregs, 0, sizeof(rmregs));
  IN(ax); IN(bx); IN(cx); IN(dx); IN(si); IN(di);
  rmregs.es = sregs->es;
  rmregs.ds = sregs->ds;

  segread(&sr);
  r.w.ax = 0x300;                 // DPMI issue real interrupt
  r.h.bl = intno;
  r.h.bh = 0;
  r.w.cx = 0;
  sr.es = sr.ds;
  r.x.edi = (unsigned)&rmregs;
  int386x(0x31, &r, &r, &sr);     // Issue the interrupt

  OUT(ax); OUT(bx); OUT(cx); OUT(dx); OUT(si); OUT(di);
  sregs->es = rmregs.es;
  sregs->cs = rmregs.cs;
  sregs->ss = rmregs.ss;
  sregs->ds = rmregs.ds;
  out->x.cflag = rmregs.flags & 0x1;
  return out->x.ax;
}

/*int DPMI_allocSelector(void)
{
  int         sel;
  union REGS  r;

  r.w.ax = 0;                     // DPMI allocate selector
  r.w.cx = 1;                     // Allocate a single selector
  int386(0x31, &r, &r);
  if (r.x.cflag)
    DPMI_Error("DPMI_allocSelector() failed!");
  sel = r.w.ax;

  r.w.ax = 9;                     // DPMI set access rights
  r.w.bx = sel;
  r.w.cx = 0x8092;                // 32 bit page granular
  int386(0x31, &r, &r);
  return sel;
}*/

void *DPMI_Map_P2L(DWord physAddr,DWord limit)
{
  union REGS  r;
  char Str[160];

  r.w.ax = 0x800;                 // DPMI map physical to linear
  r.w.bx = physAddr >> 16;
  r.w.cx = physAddr & 0xFFFF;
  r.w.si = limit >> 16;
  r.w.di = limit & 0xFFFF;
  int386(0x31, &r, &r);
  if (r.x.cflag) {
    sprintf(Str,"Failed to map a VCPI block [%xH] to DPMI Linear address (Block Limit=%xH)!",physAddr,limit);
    DPMI_Warning(Str);
  }
  return (void *)(((DWord)r.w.bx << 16) + r.w.cx);
}

void DPMI_Del_P2L(void *Linear)
{
  union REGS R;
  char Str[160];
  R.w.ax = 0x801;
  R.w.bx = (DWord)Linear>>16;
  R.w.cx = (DWord)Linear&0xFFFF;
  int386(0x31, &R, &R);
  if (R.x.cflag) {
    sprintf(Str,"Failed to free a VCPI=>DPMI mapped block at %xH!",(DWord)Linear);
    DPMI_Warning(Str);
  }
}


/*void DPMI_setSelectorBase(int sel,DWord linAddr)
{
  union REGS  r;

  r.w.ax = 7;                     // DPMI set selector base address
  r.w.bx = sel;
  r.w.cx = linAddr >> 16;
  r.w.dx = linAddr & 0xFFFF;
  int386(0x31, &r, &r);
  if (r.x.cflag)
    DPMI_Error("DPMI_setSelectorBase() failed!");
}

void DPMI_setSelectorLimit(int sel,DWord limit)
{
  union REGS  r;

  r.w.ax = 8;                     // DPMI set selector limit
  r.w.bx = sel;
  r.w.cx = limit >> 16;
  r.w.dx = limit & 0xFFFF;
  int386(0x31, &r, &r);
  if (r.x.cflag)
    DPMI_Error("DPMI_setSelectorLimit() failed!");
}*/

// High level DPMI structure.

// Constructive algorithm of building a DPMI "network".
void DPMI_Init_Core()
{
  // First, allocate enough space for main memory module.
  union REGS R;
  memset(&R,0,sizeof(REGS));
  R.w.ax = 0x501;
  R.w.bx = 0;
  R.w.cx = sizeof(Module_MB);
  int386(0x31,&R,&R);
  if (R.w.cflag)
    DPMI_Error("Failed while attempting to Initialize Core module!!");

  DPMI_Core = (Module_MB *)((R.w.bx<<16)+R.w.cx);
  DPMI_Core_Handle = (R.w.si<<16)+R.w.di;
  memset(DPMI_Core,0,sizeof(Module_MB));
  DPMI_Core->Name = strdup("DPMI Core Module"); // Ignore this for a moment.
  printf("Silvatar's Integrated DPMI memory Abstraction unit, Version 1.21.\n");
  DPMI_Message("Unit Initialized.");
  DPMI_InitFlag = 1;
}

void DPMI_Init_MB(DWord Amount)
{
  DWord I;
  union REGS R;
  Module_MB *MB,*MBEnd;
  // Now, allocate enough space for all other modules, these will be
  // stored in DPMI_MBPool.
  DPMI_Core->Size = I = sizeof(Module_MB)*Amount;
  memset(&R,0,sizeof(REGS));
  R.w.ax = 0x501;
  R.w.bx = I>>16;
  R.w.cx = I&0xFFFF;
  int386(0x31,&R,&R);
  if (R.w.cflag)
    DPMI_Error("Failed while attempting to Initialize memory module heap!!");

  DPMI_MBPool = (Module_MB *)((R.w.bx<<16)+R.w.cx);
  DPMI_Core->Address = (R.w.bx<<16)+R.w.cx;
  DPMI_Main = (void *)DPMI_Core->Address;
  DPMI_Core->Handle = (R.w.si<<16)+R.w.di;
  memset(DPMI_MBPool,0,I);
  DPMI_Free = DPMI_MBPool;
  MBEnd = DPMI_MBPool+Amount;
  for(MB=DPMI_MBPool;MB<MBEnd-1;MB++)
  {
    MB->Next = MB+1;
    (MB+1)->Prev = MB;
  }
  DPMI_Index = 0x80000000;
  // Index for FORMAL blocks only. can't collide with pointers. (2Gb)
  // Now, DPMI_MBPool can be used as an Array. The handle is in the DPMI core
  // variable. use the Core to deallocate DPMI_MBPool. don't forget to
  // eliminate DPMI_Core itself, too. attempting a recoursive command on
  // DPMI_Core will result in eliminating ALL memory allocations, including
  // the blocks themselves. recall this function afterwards.
}

void * DPMI_NewModule(Module_MB *Father,DWord Size,char *Name)
{
  Module_MB *Bin,*Bin2;
  DWord Adr;
  DPMI_Free->Name = strdup(Name);  // those unfortunate strdups.
  if (Size) {
    if (strstr(DPMI_Free->Name,"-low")) Low_Memory_Allocate(DPMI_Free,Size);
      else Memory_Allocate(DPMI_Free,Size);
  } else {
    DPMI_Free->Address = DPMI_Index++;
    goto NOALLOC; //No allocation : Formal declaration of a block only.
  }

  if (!DPMI_Free->Address)
  {
    delete DPMI_Free->Name;
    DPMI_Free->Name = NULL;
    DPMI_Warning("Unable to allocate memory. Returning NULL.");
    return NULL;
  }
  // Update hierarchial tree.
  NOALLOC:if (Father->Child) DPMI_Free->Brother = Father->Child;
  Father->Child = DPMI_Free;
  DPMI_Free->Father = Father; // BICSTL
  // Update binary tree
  Adr = DPMI_Free->Address;
/*  Bin = DPMI_Core;
  do
  {
    if (Adr>Bin->Address)
      if (Bin->Right) Bin=Bin->Right; else {Bin->Right=DPMI_Free; goto OUT;}
    else
      if (Bin->Left) Bin=Bin->Left; else {Bin->Left=DPMI_Free; goto OUT;}
  } while (1);*/
  // Update Linked list.
  OUT:DPMI_Free = DPMI_Free->Next;
  // return address.
  return (void *)Adr;
}

// Full Width Tree scanning routine. Seeks a given Allocated Pointer address.
Module_MB * DPMI_Tree_SEEK(Module_MB *Base,DWord Seek)
{
  Module_MB *Temp;
  // Recourse main
  if (Base->Address==Seek) return Base;
  // Recourse path
  if (Base->Brother) {Temp = DPMI_Tree_SEEK(Base->Brother,Seek); if (Temp) return Temp;}
  if (Base->Child) {Temp = DPMI_Tree_SEEK(Base->Child,Seek); if (Temp) return Temp;}
  return NULL;
}

void * DPMI_New(void *Father,DWord Size,char *Name)
{
  Module_MB *MB;
  // Binary tree seek
/*  while (MB&&(DWord)MB->Address!=Father)
    if (MB->Address>=Father) MB=MB->Left; else MB=MB->Right;*/
  MB = DPMI_Tree_SEEK(DPMI_Core,(DWord)Father);
  if (MB) return DPMI_NewModule(MB,Size,Name);
  DPMI_Warning("No valid pointer specified as father module, aborting.");
  return NULL;
}

void * DPMI_Assisted_New(Module_MB *Core,void *Father,DWord Size,char *Name)
{
  Module_MB *MB;
  // Binary tree seek
/*  while (MB&&(DWord)MB->Address!=Father)
    if (MB->Address>=Father) MB=MB->Left; else MB=MB->Right;*/
  MB = DPMI_Tree_SEEK(Core,(DWord)Father);
  if (MB) return DPMI_NewModule(MB,Size,Name);
  DPMI_Warning("No valid pointer specified as father module, aborting.");
  return NULL;
}


void DPMI_Destroy(Module_MB *Target)
{
  Module_MB *MB,*Father=Target->Father;
  char Msg[160];
  sprintf(Msg,"Destroying Module at %s",Target->Name);
  DPMI_Message(Msg);
  // if (Target->Child) DPMI_RecDestroy(Target->Child);
  // Update Hierarchial tree.
  // exclude from 'family' ;)
  if (Father->Child==Target) Father->Child = Target->Brother;
  else
  {
    MB = Father->Child;
    while (MB->Brother!=Target&&MB->Brother) MB=MB->Brother;
    MB->Brother = Target->Brother;
  }
  // Update Binary tree
  // DUNNO.
  // Kill module
  if (Target->Size) if (strstr(Target->Name,"-low")) Low_Memory_Free(Target); else Memory_Free(Target);
  // Update Linked List (LOUSY TRICKS ;)
  Target->Child = Target->Brother = Target->Father = NULL; //kill family bonds

  // this is really confusing...
  //(Got trouble regarding nodes with no PREV...fix it some time)
  Target->Prev->Next = Target->Next;
  Target->Next->Prev = Target->Prev;

  DPMI_Free->Prev->Next = Target;
  Target->Prev = DPMI_Free->Prev;

  Target->Next = DPMI_Free;
  DPMI_Free->Prev = Target;
}

// DPMI
void DPMI_RecDestroy(Module_MB *Target)
{
  if (Target->Child) DPMI_RecDestroy(Target->Child);
  if (Target->Brother) DPMI_RecDestroy(Target->Brother);
  DPMI_Destroy(Target);
}

void DPMI_Delete(void *Target)
{
  Module_MB *MB,*MB2;
  // BINARY searching
  // Could use Guided searching
  // (keep modules in application and recourse from them).
/*  MB2 = DPMI_Tree_SEEK(DPMI_Core,Father);
  MB = MB2->Child;
  while (MB&&MB->Address!=Target) MB=MB->Brother;*/
  MB = DPMI_Tree_SEEK(DPMI_Core,(long)Target);
/*  while (MB&&(DWord)MB->Address!=Target)
    if (MB->Address>=Target) MB=MB->Left; else MB=MB->Right;

  while (MB2&&(DWord)MB2->Address!=Father)
    if (MB2->Address>=Father) MB2=MB2->Left; else MB2=MB2->Right;*/

  if (MB) {if (MB->Child) DPMI_RecDestroy(MB->Child); DPMI_Destroy(MB); return;}
  DPMI_Warning("No valid pointer specified for deletion, aborting.");
}

void DPMI_Assisted_Delete(Module_MB *Core,void *Target)
{
  Module_MB *MB,*MB2;
//  MB = DPMI_Tree_SEEK(Core,Father); // Faster than to scan all DPMI_Core.
//  while (MB&&MB->Address!=Target) MB=MB->Brother;
/*  while (MB&&(DWord)MB->Address!=Target)
    if (MB->Address>=Target) MB=MB->Left; else MB=MB->Right;

  while (MB2&&(DWord)MB2->Address!=Father)
    if (MB2->Address>=Father) MB2=MB2->Left; else MB2=MB2->Right;*/
  MB = DPMI_Tree_SEEK(Core,(long)Target);

  if (MB) {if (MB->Child) DPMI_RecDestroy(MB->Child); DPMI_Destroy(MB); return;}
  DPMI_Warning("No valid pointer specified for deletion, aborting.");
}

void * DPMI_Resize(void *Target,DWord Size)
{
  Module_MB *MB;
  MB = DPMI_Tree_SEEK(DPMI_Core,(DWord)Target);
  if (MB) Memory_Resize(MB,Size);
  return (char *)MB->Address;
}

char * DPMI_Assisted_Resize(Module_MB *Core,void *Target,DWord Size)
{
  Module_MB *MB;
  MB = DPMI_Tree_SEEK(Core,(DWord)Target);
  if (MB) Memory_Resize(MB,Size);
  return (char *)MB->Address;
}


void DPMI_Deinit_Core()
{
  union REGS R;
  if (DPMI_Core->Child) DPMI_RecDestroy(DPMI_Core->Child);
  Memory_Free(DPMI_Core);
  memset(&R,0,sizeof(REGS));
  R.w.ax = 0x502;
  R.w.si = DPMI_Core_Handle>>16;
  R.w.di = DPMI_Core_Handle&0xFFFF;
  int386(0x31,&R,&R);
  if (R.w.cflag)
  {
    DPMI_Error("Failed to De-initialize DPMI core module!!");
    return;
  }
  DPMI_Message("Unit De-initialized.");
  DPMI_Core = DPMI_MBPool = NULL;
  DPMI_Core_Handle = 0;
  DPMI_InitFlag = 0;
}

Module_MB * DPMI_Get_MMB(void *Target)
{
  return DPMI_Tree_SEEK(DPMI_Core,(DWord)Target);
}

DWord DPMI_Get_Handle(void *Target)
{
  return (DPMI_Tree_SEEK(DPMI_Core,(DWord)Target))->Handle;
}


// == HEAP ==  routines

/*void Init_Heap()
{
  union REGS R;
  long Free = DPMI_Memory_Free();
  if (Free<0x100000) {
    DPMI_Warning("Unable to Initialize Heap: Insufficient memory.");
    return;
  }
  Free-=0x100000;
  memset(&R,0,sizeof(REGS));
  R.w.ax = 0x501;
  R.w.bx = Free>>16;
  R.w.cx = Free&0xFFFF;
  int386(0x31,&R,&R);
  if (R.w.cflag)
  {
    DPMI_Error("Unable to Allocate memory for Heap!");
  }
  DPMI_Heap = (char *)((R.w.bx<<16)+R.w.cx);
  DPMI_Heap_Handle = (R.w.si<<16)+R.w.di;
}*/

// == Aligned Buffer routines ==

// Wastes (Amount+1)x64Kb of memory.
void Init_Aligned(long Amount)
{
  long I;
  char Msg[160];
  Aligned_MB *MB,*MBEnd;
  DPMI_HeapSlack = (char *)D_New(Amount+1<<16);
  if (!DPMI_HeapSlack) {DPMI_Warning("Failed to Initialize Aligned Buffer! [H.Slack allocation error]"); return;}
  DPMI_Aligned = (char *)((long)DPMI_HeapSlack+0x10000-((long)DPMI_HeapSlack&0xFFFF));
//  printf("HS = %d,Al = %d\n",DPMI_HeapSlack,DPMI_Aligned);
  DPMI_AMB = (Aligned_MB *)D_New(Amount+1);
  if (!DPMI_AMB) {DPMI_Warning("Failed to Initialize Aligned Buffer! [AMB table allocation error]"); return;}
  DPMI_AFree = DPMI_AMB+1;
  MBEnd = DPMI_AMB+Amount+1;
  I=(long)DPMI_Aligned;
  for(MB=DPMI_AFree;MB<MBEnd;MB++)
  {
    MB->Pos = I;
    I+=65536;
    MB->Next = MB+1;
    MB->Prev = MB-1;
  }
  DPMI_AMB->Next = DPMI_AFree;
  DPMI_AMB->Prev = NULL;
  (DPMI_AMB+Amount)->Next = NULL;
  sprintf(Msg,"Aligned Buffer initialized. (%dKB)",Amount+1<<6);
  DPMI_Message(Msg);
}

char * Get_Aligned_Block()
{
  long Address = DPMI_AFree->Pos;
//  char Str[160];
//  sprintf(Str,"Allocated Aligned Block %d [ADR %xh.]",Address-(long)DPMI_Aligned>>16,Address);
/// DPMI_Message(Str);
  DPMI_AFree = DPMI_AFree->Next;
  return (char *)Address;
}

void Remove_Aligned_Block(void *Target)
{
  Aligned_MB *MB = DPMI_AMB->Next;
  while(MB&&MB->Pos!=(long)Target) MB=MB->Next;
  if (MB->Pos==(long)Target)
  {
    if (MB->Prev) MB->Prev->Next = MB->Next;
    MB->Next->Prev = MB->Prev;

    DPMI_AFree->Prev->Next = MB;
    MB->Prev = DPMI_AFree->Prev;

    DPMI_AFree->Prev = MB;
    MB->Next = DPMI_AFree;
  } else DPMI_Warning("Couldn't find target for deletion in Aligned buffer.");
//  printf("Target at %d\n",(long)Target);
}

void Deinit_Aligned()
{
  DPMI_Message("Aligned Buffer De-initialized.");
  D_Del(DPMI_HeapSlack);
  D_Del(DPMI_AMB);
}
