.586p
.mmx
; Fused texture+gouraud grid mapper, MMX-scheduled (the "smart register and MMX
; scheduling" wobbler). Integer pipe runs WOBTR.ASM's proven packed (Vint<<8|Uint)
; addressing + fetch VERBATIM; MMX pipe interpolates the cell light and modulates each
; texel. The two streams interleave per pixel so they pair on the P55C (MMX issues in
; either pipe). Replaces Grid_Texture_MapASM + the separate full-screen modulate pass
; for the gouraud wobbler layer (GLAT plane).
;
; Light math: GridPoint R/G/B hold weight*254 (longs; see DOSRECOVER). Target =
; texel*(R>>8)>>8 = texel*R>>16. Light words are kept as R>>1 (fits SIGNED 16:
; max 254*255/2 = 32385), texel words are pre-doubled (paddw t,t), and pmulhw yields
; (2t * R/2) >> 16 = t*R>>16 exactly. Per-pixel light step words = (Right-Left)>>4
; ((>>3 for the 8-px run) then >>1 to match the halved light scale).
_DATA	SEGMENT PARA PUBLIC 'DATA'
align 8
MMXTempW								dq  0
ScanLine								dd  0
Delta_Left_U            dd  0
Delta_Left_V            dd  0
Delta_Right_U           dd  0
Delta_Right_V           dd  0
Left_U									dd  0
Left_V                  dd  0
Right_U                 dd  0
Right_V                 dd  0
Left_R                  dd  0
Left_G                  dd  0
Left_B                  dd  0
Right_R                 dd  0
Right_G                 dd  0
Right_B                 dd  0
Delta_Left_R            dd  0
Delta_Left_G            dd  0
Delta_Left_B            dd  0
Delta_Right_R           dd  0
Delta_Right_G           dd  0
Delta_Right_B           dd  0
du                      dd  0
dv                      dd  0
TilDuDx                 dd  0   ; interleaved tiled du (gap bits pre-set to 1)
TilDvDx                 dd  0   ; interleaved tiled dv (gap bits pre-set to 1)
PointsList              dd  0
Texture		        			dd  0
x                       dd  0
y								        dd  0
Tebp       							dd  0
counter									dd  0

ScreenWidth							equ 320
PointsWidth							equ ScreenWidth/8
EXTRN _WobPointsHeight  : DWORD
u												equ 0
v												equ 4
r												equ 8
g												equ 12
b												equ 16
ElementSize             equ 20    ; GridPoint = {long U,V,R,G,B}
align 1
_DATA	ENDS

_TEXT	SEGMENT BYTE PUBLIC 'CODE'
	ASSUME	cs:_TEXT, ds:_DATA

	PUBLIC	Grid_Texture_MapG_MMX_
Grid_Texture_MapG_MMX_:
; ecx = Texture (row-major 256x256 dwords), esi = GridPoint array, edi = dest page
mov [Tebp],ebp
mov [Texture],ecx
mov [ScanLine],edi
mov eax,[_WobPointsHeight]
mov [y],eax
mov [x],PointsWidth
pxor mm7,mm7                      ; constant zero for byte->word unpacks

@@DrawLoop:
; ---- cell setup: left/right edge interpolants (U,V verbatim WOBTR; + R,G,B) ----
mov eax,[esi+u]
mov ebx,[esi+v]
mov [Left_U],eax
mov [Left_V],ebx
mov ecx,[esi+ElementSize+ElementSize*PointsWidth+u]
mov edx,[esi+ElementSize+ElementSize*PointsWidth+v]
sub ecx,eax
sub edx,ebx
sar ecx,3
sar edx,3
mov [Delta_Left_U],ecx
mov [Delta_Left_V],edx

mov eax,[esi+r]
mov ecx,[esi+ElementSize+ElementSize*PointsWidth+r]
mov [Left_R],eax
sub ecx,eax
sar ecx,3
mov [Delta_Left_R],ecx
mov eax,[esi+g]
mov ecx,[esi+ElementSize+ElementSize*PointsWidth+g]
mov [Left_G],eax
sub ecx,eax
sar ecx,3
mov [Delta_Left_G],ecx
mov eax,[esi+b]
mov ecx,[esi+ElementSize+ElementSize*PointsWidth+b]
mov [Left_B],eax
sub ecx,eax
sar ecx,3
mov [Delta_Left_B],ecx

mov eax,[esi+ElementSize+u]
mov ebx,[esi+ElementSize+v]
mov [Right_U],eax
mov [Right_V],ebx
mov ecx,[esi+ElementSize*2+ElementSize*PointsWidth+u]
mov edx,[esi+ElementSize*2+ElementSize*PointsWidth+v]
sub ecx,eax
sub edx,ebx
sar ecx,3
sar edx,3
mov [Delta_Right_U],ecx
mov [Delta_Right_V],edx

mov eax,[esi+ElementSize+r]
mov ecx,[esi+ElementSize*2+ElementSize*PointsWidth+r]
mov [Right_R],eax
sub ecx,eax
sar ecx,3
mov [Delta_Right_R],ecx
mov eax,[esi+ElementSize+g]
mov ecx,[esi+ElementSize*2+ElementSize*PointsWidth+g]
mov [Right_G],eax
sub ecx,eax
sar ecx,3
mov [Delta_Right_G],ecx
mov eax,[esi+ElementSize+b]
mov ecx,[esi+ElementSize*2+ElementSize*PointsWidth+b]
mov [Right_B],eax
sub ecx,eax
sar ecx,3
mov [Delta_Right_B],ecx

add esi,ElementSize
mov [PointsList],esi
mov edi,[ScanLine]
mov [counter],8

@@Inner8:
; ---- per row: MMX light start + step (lanes: 0=B 1=G 2=R, BGR0 pixel bytes) ----
mov eax,[Left_B]
sar eax,1
mov word ptr [MMXTempW],ax
mov eax,[Left_G]
sar eax,1
mov word ptr [MMXTempW+2],ax
mov eax,[Left_R]
sar eax,1
mov word ptr [MMXTempW+4],ax
mov word ptr [MMXTempW+6],0
movq mm0,[MMXTempW]               ; light (words = weight*254 >> 1)
mov eax,[Right_B]
sub eax,[Left_B]
sar eax,4
mov word ptr [MMXTempW],ax
mov eax,[Right_G]
sub eax,[Left_G]
sar eax,4
mov word ptr [MMXTempW+2],ax
mov eax,[Right_R]
sub eax,[Left_R]
sar eax,4
mov word ptr [MMXTempW+4],ax
mov word ptr [MMXTempW+6],0
movq mm1,[MMXTempW]               ; light step per pixel

; ---- per row: packed UV accumulators (VERBATIM WOBTR scheme) ----
mov eax,[Left_U]
mov ebx,[Left_V]
mov ecx,[Right_U]
mov edx,[Right_V]
sub ecx,eax
sub edx,ebx
sar ecx,3
mov ebx,0
sar edx,3
mov [du],ecx
mov [dv],edx
cmp edx,0
jg @@Positive
mov ecx,00FF0000h
jmp @@Cont
@@Positive:
mov ecx,00000000h
@@Cont:
mov edx,0
mov bl,byte ptr [Left_U]
shl ebx,24
mov bh,byte ptr [Left_V+1]
mov eax,0
mov bl,byte ptr [Left_V]
mov al,byte ptr [du]
mov ebp,0
shl eax,24
or  ecx,eax
mov dl,byte ptr [Left_U+1]
mov dh,bh
mov eax,0
mov al,byte ptr [du+1]
mov cl,byte ptr [dv]
mov ch,byte ptr [dv+1]
mov ebp,eax
mov esi,[Texture]

; ---- 8 pixels: integer fetch/advance interleaved with MMX modulate ----
    mov   eax,[esi+edx*4]
    movd  mm2,eax
    mov   dh,0
    punpcklbw mm2,mm7
    add   ebx,ecx
    paddw mm2,mm2
    adc   edx,ebp
    pmulhw mm2,mm0
    mov   dh,bh
    packuswb mm2,mm2
    movd  [edi],mm2
    paddw mm0,mm1

    mov   eax,[esi+edx*4]
    movd  mm2,eax
    mov   dh,0
    punpcklbw mm2,mm7
    add   ebx,ecx
    paddw mm2,mm2
    adc   edx,ebp
    pmulhw mm2,mm0
    mov   dh,bh
    packuswb mm2,mm2
    movd  [edi+4],mm2
    paddw mm0,mm1

    mov   eax,[esi+edx*4]
    movd  mm2,eax
    mov   dh,0
    punpcklbw mm2,mm7
    add   ebx,ecx
    paddw mm2,mm2
    adc   edx,ebp
    pmulhw mm2,mm0
    mov   dh,bh
    packuswb mm2,mm2
    movd  [edi+8],mm2
    paddw mm0,mm1

    mov   eax,[esi+edx*4]
    movd  mm2,eax
    mov   dh,0
    punpcklbw mm2,mm7
    add   ebx,ecx
    paddw mm2,mm2
    adc   edx,ebp
    pmulhw mm2,mm0
    mov   dh,bh
    packuswb mm2,mm2
    movd  [edi+12],mm2
    paddw mm0,mm1

    mov   eax,[esi+edx*4]
    movd  mm2,eax
    mov   dh,0
    punpcklbw mm2,mm7
    add   ebx,ecx
    paddw mm2,mm2
    adc   edx,ebp
    pmulhw mm2,mm0
    mov   dh,bh
    packuswb mm2,mm2
    movd  [edi+16],mm2
    paddw mm0,mm1

    mov   eax,[esi+edx*4]
    movd  mm2,eax
    mov   dh,0
    punpcklbw mm2,mm7
    add   ebx,ecx
    paddw mm2,mm2
    adc   edx,ebp
    pmulhw mm2,mm0
    mov   dh,bh
    packuswb mm2,mm2
    movd  [edi+20],mm2
    paddw mm0,mm1

    mov   eax,[esi+edx*4]
    movd  mm2,eax
    mov   dh,0
    punpcklbw mm2,mm7
    add   ebx,ecx
    paddw mm2,mm2
    adc   edx,ebp
    pmulhw mm2,mm0
    mov   dh,bh
    packuswb mm2,mm2
    movd  [edi+24],mm2
    paddw mm0,mm1

    mov   eax,[esi+edx*4]
    movd  mm2,eax
    punpcklbw mm2,mm7
    paddw mm2,mm2
    pmulhw mm2,mm0
    packuswb mm2,mm2
    movd  [edi+28],mm2

; ---- row epilogue: step edges (U,V verbatim WOBTR; + R,G,B) ----
add edi,ScreenWidth*4
mov eax,[Left_U]
mov ebx,[Left_V]
mov ecx,[Delta_Left_U]
mov edx,[Delta_Left_V]
add eax,ecx
add ebx,edx
mov [Left_U],eax
mov [Left_V],ebx
mov eax,[Right_U]
mov ebx,[Right_V]
mov ecx,[Delta_Right_U]
mov edx,[Delta_Right_V]
add eax,ecx
add ebx,edx
mov [Right_U],eax
mov [Right_V],ebx
mov eax,[Left_R]
add eax,[Delta_Left_R]
mov [Left_R],eax
mov eax,[Left_G]
add eax,[Delta_Left_G]
mov [Left_G],eax
mov eax,[Left_B]
add eax,[Delta_Left_B]
mov [Left_B],eax
mov eax,[Right_R]
add eax,[Delta_Right_R]
mov [Right_R],eax
mov eax,[Right_G]
add eax,[Delta_Right_G]
mov [Right_G],eax
mov eax,[Right_B]
add eax,[Delta_Right_B]
mov [Right_B],eax
mov esi,[counter]
dec esi
mov [counter],esi
jnz @@Inner8

; ---- cell / row closers (verbatim WOBTR) ----
mov eax,[ScanLine]
add eax,32
mov [ScanLine],eax
mov esi,[PointsList]
mov ebx,[x]
dec ebx
mov [x],ebx
jnz @@DrawLoop
mov ecx,[ScanLine]
mov ebx,[y]
add ecx,(8-1)*ScreenWidth*4
add esi,ElementSize
mov [x],PointsWidth
mov [PointsList],esi
mov [ScanLine],ecx
dec ebx
mov [y],ebx
jnz @@DrawLoop
emms
mov ebp,[Tebp]
ret

; =========================================================================================
; Grid_Texture_MapG_MMX_T_ : tiled twin of the routine above.
;
; Identical light math, identical edge interpolation, identical output — the ONLY difference
; is how a texel is addressed. The texture is pre-reordered into 8-wide vertical strips
; (Wob_SwizzleImage), so index = ((u>>3)<<11)|(v<<3)|(u&7) and a ONE-TEXEL VERTICAL step
; moves 32 bytes (one cache line) instead of 1024. The wobble varies u and v in both screen
; axes over a 256KB texture — 16x the P55C's L1 — so the row-major form misses on close to
; every fetch; this is the 53% slice of a GLAT frame.
;
; Addressing is the FATMAP2 interleaved-accumulator form BITRUE.ASM already uses: U and V are
; pre-scattered into the index's bit positions with the gap bits set to 1 in the DELTAS so
; each field's carry ripples into the next, and (Uacc+Vacc)>>16 IS the tiled index. The gaps
; are cleared again by the two ANDs. Per pixel that is 2 adds, 2 ands, an lea and a shr,
; which pair into the V pipe under the MMX dependency chain (movd->punpck->paddw->pmulhw->
; packuswb, ~7 cycles of latency) — i.e. essentially free, and it buys the misses back.
;   U: hi5@[27:31]  lo3@[16:18]  frac@[7:14]   acc mask 0F8077FFFh   delta gaps 07F88000h
;   V:              int8@[19:26] frac@[7:14]   acc mask 07F87FFFh   delta gaps 0F8078000h
; frac sits at bit 7, not 8, so that the sum of BOTH fracs (max 0FF00h) cannot carry into
; the lo3 field. Deltas keep only the low 16 bits of du/dv — exactly what the row-major form
; does (it masks at the fetch), so wrapping is identical for the plane's multi-million U/V.
;
; Registers in the inner 8: esi=Texture ebx=Vacc ecx=Uacc edx=tdu eax=tdv ebp=texel index
; (pre-shifted one pixel ahead so the index is never generated in the cycle before its use —
; the same AGI dodge the packed-byte loop above makes with its trailing "mov dh,bh").
; =========================================================================================
PIXT	MACRO	off
	movd  mm2,[esi+ebp*4]           ; fetch (index prepared 3 instrs back)
	add   ecx,edx                   ; u += du, carry ripples through the gaps
	punpcklbw mm2,mm7
	add   ebx,eax                   ; v += dv
	paddw mm2,mm2                   ; pre-double so pmulhw yields t*R>>16
	and   ecx,0F8077FFFh            ; clear u gaps
	and   ebx,07F87FFFh             ; clear v gaps
	pmulhw mm2,mm0                  ; modulate by the cell light
	lea   ebp,[ecx+ebx]             ; combined index for the NEXT pixel
	paddw mm0,mm1                   ; step the light
	shr   ebp,16
	packuswb mm2,mm2
	movd  [edi+off],mm2
	ENDM

	PUBLIC	Grid_Texture_MapG_MMX_T_
Grid_Texture_MapG_MMX_T_:
; ecx = Texture (8-wide-strip tiled 256x256 dwords), esi = GridPoint array, edi = dest page
mov [Tebp],ebp
mov [Texture],ecx
mov [ScanLine],edi
mov eax,[_WobPointsHeight]
mov [y],eax
mov [x],PointsWidth
pxor mm7,mm7                      ; constant zero for byte->word unpacks

@@DrawLoopT:
; ---- cell setup: left/right edge interpolants (verbatim from the row-major routine) ----
mov eax,[esi+u]
mov ebx,[esi+v]
mov [Left_U],eax
mov [Left_V],ebx
mov ecx,[esi+ElementSize+ElementSize*PointsWidth+u]
mov edx,[esi+ElementSize+ElementSize*PointsWidth+v]
sub ecx,eax
sub edx,ebx
sar ecx,3
sar edx,3
mov [Delta_Left_U],ecx
mov [Delta_Left_V],edx

mov eax,[esi+r]
mov ecx,[esi+ElementSize+ElementSize*PointsWidth+r]
mov [Left_R],eax
sub ecx,eax
sar ecx,3
mov [Delta_Left_R],ecx
mov eax,[esi+g]
mov ecx,[esi+ElementSize+ElementSize*PointsWidth+g]
mov [Left_G],eax
sub ecx,eax
sar ecx,3
mov [Delta_Left_G],ecx
mov eax,[esi+b]
mov ecx,[esi+ElementSize+ElementSize*PointsWidth+b]
mov [Left_B],eax
sub ecx,eax
sar ecx,3
mov [Delta_Left_B],ecx

mov eax,[esi+ElementSize+u]
mov ebx,[esi+ElementSize+v]
mov [Right_U],eax
mov [Right_V],ebx
mov ecx,[esi+ElementSize*2+ElementSize*PointsWidth+u]
mov edx,[esi+ElementSize*2+ElementSize*PointsWidth+v]
sub ecx,eax
sub edx,ebx
sar ecx,3
sar edx,3
mov [Delta_Right_U],ecx
mov [Delta_Right_V],edx

mov eax,[esi+ElementSize+r]
mov ecx,[esi+ElementSize*2+ElementSize*PointsWidth+r]
mov [Right_R],eax
sub ecx,eax
sar ecx,3
mov [Delta_Right_R],ecx
mov eax,[esi+ElementSize+g]
mov ecx,[esi+ElementSize*2+ElementSize*PointsWidth+g]
mov [Right_G],eax
sub ecx,eax
sar ecx,3
mov [Delta_Right_G],ecx
mov eax,[esi+ElementSize+b]
mov ecx,[esi+ElementSize*2+ElementSize*PointsWidth+b]
mov [Right_B],eax
sub ecx,eax
sar ecx,3
mov [Delta_Right_B],ecx

add esi,ElementSize
mov [PointsList],esi
mov edi,[ScanLine]
mov [counter],8

@@Inner8T:
; ---- per row: MMX light start + step (verbatim) ----
mov eax,[Left_B]
sar eax,1
mov word ptr [MMXTempW],ax
mov eax,[Left_G]
sar eax,1
mov word ptr [MMXTempW+2],ax
mov eax,[Left_R]
sar eax,1
mov word ptr [MMXTempW+4],ax
mov word ptr [MMXTempW+6],0
movq mm0,[MMXTempW]               ; light (words = weight*254 >> 1)
mov eax,[Right_B]
sub eax,[Left_B]
sar eax,4
mov word ptr [MMXTempW],ax
mov eax,[Right_G]
sub eax,[Left_G]
sar eax,4
mov word ptr [MMXTempW+2],ax
mov eax,[Right_R]
sub eax,[Left_R]
sar eax,4
mov word ptr [MMXTempW+4],ax
mov word ptr [MMXTempW+6],0
movq mm1,[MMXTempW]               ; light step per pixel

; ---- per row: interleave du/dv into the tiled bit positions, gaps set to 1 ----
mov eax,[Right_U]
sub eax,[Left_U]
sar eax,3                         ; du (8.8)
mov ebx,eax
mov ecx,eax
and ebx,0FFh
shl ebx,7                         ; du frac -> [7:14]
and ecx,0700h
shl ecx,8                         ; du lo3  -> [16:18]
or  ebx,ecx
and eax,0F800h
shl eax,16                        ; du hi5  -> [27:31]
or  ebx,eax
or  ebx,07F88000h                 ; gap+guard bits = 1
mov [TilDuDx],ebx

mov eax,[Right_V]
sub eax,[Left_V]
sar eax,3                         ; dv (8.8)
mov ebx,eax
and eax,0FF00h
shl eax,11                        ; dv int8 -> [19:26]
and ebx,0FFh
shl ebx,7                         ; dv frac -> [7:14]
or  ebx,eax
or  ebx,0F8078000h                ; gap bits = 1
mov [TilDvDx],ebx

; ---- per row: interleave the span start into the two accumulators (gaps 0) ----
mov ecx,[Left_U]
mov eax,ecx
mov ebx,ecx
and eax,0FFh
shl eax,7
and ebx,0700h
shl ebx,8
or  eax,ebx
and ecx,0F800h
shl ecx,16
or  ecx,eax                       ; ecx = U accumulator

mov ebx,[Left_V]
mov eax,ebx
and ebx,0FF00h
shl ebx,11
and eax,0FFh
shl eax,7
or  ebx,eax                       ; ebx = V accumulator

mov edx,[TilDuDx]                 ; both deltas live in registers across the run
mov eax,[TilDvDx]
mov esi,[Texture]
lea ebp,[ecx+ebx]
shr ebp,16                        ; prime pixel 0's index

; ---- 8 pixels: tiled fetch/advance interleaved with the MMX modulate ----
	PIXT 0
	PIXT 4
	PIXT 8
	PIXT 12
	PIXT 16
	PIXT 20
	PIXT 24
; last pixel: no advance needed (the row epilogue re-primes from Left_U/Left_V)
	movd  mm2,[esi+ebp*4]
	punpcklbw mm2,mm7
	paddw mm2,mm2
	pmulhw mm2,mm0
	packuswb mm2,mm2
	movd  [edi+28],mm2

; ---- row epilogue: step the edges (verbatim) ----
add edi,ScreenWidth*4
mov eax,[Left_U]
mov ebx,[Left_V]
mov ecx,[Delta_Left_U]
mov edx,[Delta_Left_V]
add eax,ecx
add ebx,edx
mov [Left_U],eax
mov [Left_V],ebx
mov eax,[Right_U]
mov ebx,[Right_V]
mov ecx,[Delta_Right_U]
mov edx,[Delta_Right_V]
add eax,ecx
add ebx,edx
mov [Right_U],eax
mov [Right_V],ebx
mov eax,[Left_R]
add eax,[Delta_Left_R]
mov [Left_R],eax
mov eax,[Left_G]
add eax,[Delta_Left_G]
mov [Left_G],eax
mov eax,[Left_B]
add eax,[Delta_Left_B]
mov [Left_B],eax
mov eax,[Right_R]
add eax,[Delta_Right_R]
mov [Right_R],eax
mov eax,[Right_G]
add eax,[Delta_Right_G]
mov [Right_G],eax
mov eax,[Right_B]
add eax,[Delta_Right_B]
mov [Right_B],eax
dec dword ptr [counter]
jnz @@Inner8T

; ---- cell / row closers (verbatim) ----
mov eax,[ScanLine]
add eax,32
mov [ScanLine],eax
mov esi,[PointsList]
mov ebx,[x]
dec ebx
mov [x],ebx
jnz @@DrawLoopT
mov ecx,[ScanLine]
mov ebx,[y]
add ecx,(8-1)*ScreenWidth*4
add esi,ElementSize
mov [x],PointsWidth
mov [PointsList],esi
mov [ScanLine],ecx
dec ebx
mov [y],ebx
jnz @@DrawLoopT
emms
mov ebp,[Tebp]
ret
_TEXT	ENDS

END
