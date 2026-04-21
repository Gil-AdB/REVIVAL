#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <conio.h>
#include <math.h>

#include "MidasDLL.H"

#include "..\FDS_Defs.H"
#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"

#pragma aux SetText =\
"mov ax,3"\
"int 10h"\
modify exact [eax];

/* #define the following to make the demo attempt autodetection first before
   running setup. Note that you should _ALWAYS_ give the user the possibility
   to run manual setup somehow, as autodetection can fail. In this example,
   we'll run setup if any command line argument has been given (you can
   document it as you wish - "-setup" or whatever :) */
/*#define ATTEMPT_DETECT*/


/* Music playing position information - updated by UpdateInfo: */
unsigned        position;               /* Current position */
unsigned        pattern;                /* Current pattern number */
unsigned        row;                    /* Current row number */
int             syncInfo;               /* Music synchronization info */
MIDASmodule module;                     /* Der Module */
MIDASmodulePlayHandle playHandle;       /* Das Playing Handle */
char *XM;
/* Music file name ;) */
//char            *moduleName = "..\\data\\templsun.xm";


/****************************************************************************\
*
* Function:     void MIDASerror(void)
*
* Description:  Displays most recent MIDAS error and exits to DOS
*
\****************************************************************************/

void MIDASerror(void)
{
    SetText();
    printf("MIDAS error: %s\n", MIDASgetErrorMessage(MIDASgetLastError()));
    MIDASclose(); /*!!!!!*/
    exit(EXIT_FAILURE);
}



/****************************************************************************\
*
* Function:     void UpdateInfo(void)
*
* Description:  Updates song playing information (defined at the beginning of
*               this file)
*
* Note:         To use the position information from Assembler, do the
*               following (almost - get the idea?)
*
*               INCLUDE "midas.inc"
*               ...
*               GLOBAL  UpdateInfo : LANG
*               ...
*               EXTRN   position : dword
*               EXTRN   pattern : dword
*               EXTRN   row : dword
*               EXTRN   syncInfo : dword
*               ...
*               CODESEG
*               ...
*               PROC    SuperRoutine    NEAR
*               ...
*               call    UpdateInfo C
*               cmp     [position],17
*               je      @@design
*               ...
*
\****************************************************************************/

void MIDAS_CALL UpdateInfo(void)
{
    /* MIDAS_CALL is cdecl for Watcom, empty for DJGPP. Helps calling this
       from assembler, otherwise unnecessary */

    static MIDASplayStatus status;

    /* Get playback status: */
    if ( !MIDASgetPlayStatus(playHandle, &status) )
        MIDASerror();

    /* Store interesting information in easy-to-access variables: */
    position = status.position;
    pattern = status.pattern;
    row = status.row;
    syncInfo = status.syncInfo;
}


void MIDAS_CALL TimerCallback(void)
{
  Timer++;
}

void Install_MIDAS()
{
  MIDASstartup();
  #ifdef ATTEMPT_DETECT
  if (!MIDASdetectSoundCard())
  #endif
  {
    if ( !MIDASconfig() )
    {
      /* Configuration failed. Check if it is an error - if yes, report,
         otherwise exit normally: */
      if ( MIDASgetLastError() )
      {
        MIDASerror();
      }
      else
      {
        printf("User exit!\n");
        exit(1);
      }
    }
  }
  if ( !MIDASinit() )
    MIDASerror();
}

void Initialize_Module(char *XMFile)
{
  char T0,T1;
  unsigned long Size = *(unsigned long *)XMFile;
/*  T0=XMFile[0];
  T1=XMFile[1];
  XMFile[0]=XMFile[3];
  XMFile[1]=XMFile[2];
  XMFile[2]=T1;
  XMFile[3]=T0;*/
  if ( (module = MIDASloadModule(XMFile)) == NULL )
    MIDASerror();

  if ( !MIDASsetTimerCallbacks(100000, FALSE, &TimerCallback, NULL, NULL) )
    MIDASerror();
}

void Play_Module()
{
  Timer = 0;
  if ( (playHandle = MIDASplayModule(module, TRUE)) == 0 )
      MIDASerror();

}

void ShutDown()
{
    /* Stop playing module: */
    if ( !MIDASstopModule(playHandle) )
        MIDASerror();

    /* Deallocate the module: */
  if ( !MIDASfreeModule(module) )
    MIDASerror();

    /* Remove timer callback: */
  if ( !MIDASremoveTimerCallbacks() )
    MIDASerror();

    /* And close MIDAS: */
  if ( !MIDASclose() )
    MIDASerror();
}
