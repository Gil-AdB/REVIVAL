; CHAININT.ASM — 1998-byte-exact replacement for the Open Watcom C-library
; _chain_intr_ runtime routine.
;
; The OW2 clib version inserts, right before chaining to the previous handler:
;       mov  ebx,[ebp+38h]      ; saved EFLAGS
;       and  ebx,0fffffcffh     ; clear IF (bit 9) and TF (bit 8)
;       push ebx
;       popfd
; i.e. it does a POPFD (force-clearing the interrupt flag) before RETF-ing into
; the chained INT-8 handler.  The 1998 Watcom clib did NOT touch EFLAGS at all.
; Under DOSBox-X's DOS4GW that POPFD changes the interrupt state during the
; reflected real-mode BIOS INT-8 dispatch and corrupts / faults.  This module
; reproduces the 1998 routine exactly (no POPFD) and, because the linker only
; pulls a library module to satisfy an *undefined* symbol, defining _chain_intr_
; here suppresses the clib one entirely.

.386p
_TEXT   SEGMENT USE32 BYTE PUBLIC 'CODE'
        ASSUME  cs:_TEXT

        PUBLIC  _chain_intr_
_chain_intr_:
        mov     ecx,eax                 ; ecx = far-ptr offset  (arg was dx:eax)
        mov     eax,edx                 ; eax = far-ptr segment
        mov     esp,ebp                 ; discard caller's pushed return addr
        xchg    dword ptr [ebp+28h],ecx ; overwrite iret-frame return EIP
        xchg    dword ptr [ebp+2ch],eax ; overwrite iret-frame return CS
        pop     gs
        pop     fs
        pop     es
        pop     ds
        pop     edi
        pop     esi
        pop     ebp
        pop     ebx                     ; saved ESP slot (discarded)
        pop     ebx
        pop     edx
        retf                            ; -> previous INT-8 handler
_TEXT   ENDS

        END
