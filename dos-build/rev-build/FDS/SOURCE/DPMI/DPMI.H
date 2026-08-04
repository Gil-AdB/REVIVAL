#pragma pack(1)
typedef unsigned long DWord;

// Register structures
struct _RMWORDREGS {
  unsigned short ax, bx, cx, dx, si, di, cflag;
};

struct _RMBYTEREGS {
  unsigned char   al, ah, bl, bh, cl, ch, dl, dh;
};

typedef union {
  struct  _RMWORDREGS x;
  struct  _RMBYTEREGS h;
} RMREGS;

typedef struct {
  unsigned short  es;
  unsigned short  cs;
  unsigned short  ss;
  unsigned short  ds;
} RMSREGS;

typedef struct {
  long    edi;
  long    esi;
  long    ebp;
  long    reserved;
  long    ebx;
  long    edx;
  long    ecx;
  long    eax;
  short   flags;
  short   es,ds,fs,gs,ip,cs,sp,ss;
} _RMREGS;

// Module memory block
// Total size [32+Name size] bytes.
struct Module_MB
{
  char *Name;
  DWord Address;
  DWord Handle;  //For low blocks, this will be [Selector:Segment].
  DWord Size;
  Module_MB *Next,*Prev,*Father,*Child,*Brother; //Brother I can see the light!
  // This holds Linked list, Hierarchial tree and Binary tree at the same
  // time. Linked list used for [de]Allocation,h.tree for info/delete,
};

struct Aligned_MB
{
  long Pos; //Position in aligned memory space.
  Aligned_MB *Prev,*Next;
};

Module_MB *DPMI_Core;
DWord DPMI_Core_Handle,DPMI_Index,DPMI_InitFlag;
void *DPMI_Main;
Module_MB *DPMI_MBPool;
Module_MB *DPMI_Free; //Directly points to the next free block
char *DPMI_HeapSlack,*DPMI_Aligned;
Aligned_MB *DPMI_AMB,*DPMI_AFree;

//char *DPMI_Heap; //Internal Heap (for Aligned memory support)
//DWord DPMI_Heap_Handle;

#pragma pack()
extern void * D_New(DWord Size);
extern void D_Del(void *Target);
extern void * D_Resize(void *Target,DWord Size);
extern void DPMI_Init_Core();
extern void DPMI_Deinit_Core();
extern void DPMI_Init_MB(DWord Amount);
extern void * DPMI_NewModule(Module_MB *Father,DWord Size,char *Name);
extern void * DPMI_New(void *Father,DWord Size,char *Name);
extern void * DPMI_Assisted_New(Module_MB *Core,void *Father,DWord Size,char *Name);
extern void DPMI_Destroy(Module_MB *Target);
extern void DPMI_RecDestroy(Module_MB *Target);
extern void DPMI_Delete(void *Target);
extern void DPMI_Assisted_Delete(Module_MB *Core,void *Target);
extern Module_MB * DPMI_Get_MMB(void *Target);
extern DWord DPMI_Get_Handle(void *Target);
extern void DPMI_Message(char *ErrMsg);
extern void DPMI_Warning(char *ErrMsg);
extern void DPMI_Error(char *ErrMsg);
extern DWord DPMI_Free_Memory(void);
extern void DPMI_allocRealSeg(int size,int *sel,int *r_seg);
extern void DPMI_freeRealSeg(unsigned sel);
extern int DPMI_int86(int intno, RMREGS *in, RMREGS *out);
extern int DPMI_int86x(int intno, RMREGS *in, RMREGS *out, RMSREGS *sregs);
extern int DPMI_allocSelector(void);
extern DWord DPMI_mapPhysicalToLinear(DWord physAddr,DWord limit);
extern void DPMI_setSelectorBase(int sel,DWord linAddr);
extern void DPMI_setSelectorLimit(int sel,DWord limit);