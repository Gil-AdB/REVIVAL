
.386p
_DATA	SEGMENT DWORD PUBLIC 'DATA'
align 4
Left_Section         	dd 0
Right_Section 	     	dd 0
Left_Section_Height  	dd 0
Right_Section_Height 	dd 0
Left_U 		     	dd 0.0 ; float
Delta_left_U 	     	dd 0.0 ; float
Left_V 		     	dd 0.0 ; float
Delta_left_V 	     	dd 0.0 ; float
Left_Z                  dd 0.0 ; float
Delta_left_Z            dd 0.0 ; float
Right_Z                 dd 0.0 ; float
Delta_right_Z           dd 0.0 ; float
Right_U                 dd 0.0 ; float
Delta_right_U           dd 0.0 ; float
Right_V                 dd 0.0 ; float
Delta_right_V           dd 0.0 ; float
RevHeight               dd 0.0 ; float
RevWidth                dd 0.0 ; float
u0                      dd 0
v0                      dd 0
u1                      dd 0
v1                      dd 0
du		      	dd 0
dv                      dd 0
TilDuDx                 dd 0   ; interleaved tiled du (FDS_TEX_SWIZZLE; gap bits pre-set to 1)
TilDvDx                 dd 0   ; interleaved tiled dv (FDS_TEX_SWIZZLE; gap bits pre-set to 1)
zcAcc                   dd 0   ; FDS_ZBUF: running 1/w depth code (memory-stepped, chains spans)
LineWidth               dd 0   ; FDS_ZBUF: original scanline width (FIDIV divisor)
ZdPx                    dd 0   ; FDS_ZBUF: per-pixel depth-code step
zcTmpR                  dd 0   ; FDS_ZBUF: right-endpoint code (clamp staging)
EXTRN _ZScaleF : DWORD         ; FDS_ZBUF: adaptive 1/w -> code scale (49152*wNear, C side)
EXTRN _ZLinScale : DWORD       ; FDS_ZLINEAR: fixed far-anchored linear-Z scale (C side)
enc0                    dd 0   ; FDS_ZLINEAR: encoded linear-Z at 16px checkpoint N (start)
enc1                    dd 0   ; FDS_ZLINEAR: encoded linear-Z at checkpoint N+1 (end)
zScTmp                  dd 0   ; FDS_ZLINEAR: FISTP scratch
zcounter                dd 0   ; FDS_ZBUF: span pixel count (frees ebp for the z pointer)
EXTRN _ZBufPtr : DWORD         ; FDS_ZBUF: (long)ZBuf (16-bit codes, px-parallel to VPage)
XWidth			dd 0
Height			dd 0
Longest			dd 0
; FDS_NGON: the chains hold up to 8 vertices (3 in, one per screen plane, plus slack).
; Left_Array and RIght_Array MUST stay adjacent and equally sized - the n-gon prologue
; copies both in one run. (The original spelling of RIght_Array is kept; the assembler is
; case-insensitive, so every [Right_Array] reference below resolves to it.)
Left_Array              dd 10 dup(0)
RIght_Array             dd 10 dup(0)
Left_X 			dd 0
Delta_Left_X 		dd 0
Right_X 		dd 0
Delta_Right_X 		dd 0
BitMap                  dd 0
Where                   dd 0
Top                     dd 0
Middle                  dd 0
Bottom                  dd 0
ScanLine		dd 0
TEBP			dd 0
Vertex_X  equ 0
Vertex_Y  equ 4
Vertex_Z  equ 8
Vertex_U  equ 12
Vertex_V  equ 16
One       dd  1.0
Sixteen   dd  16.0
Sixtyfive dd  256.0
; ---- FDS_SUBTEX: vertex X,Y arrive as 16.16 instead of snapped integers ----
xfracI    dd  0     ; (ceil(Left_X)<<16) - Left_X : exact edge -> first pixel (POSITIVE)
Inv1M     dd  0.00000095367431640625   ; 1/(65536*16): xfrac in units of the per-16px delta
PreY      dd  0     ; (ceil(Y1)<<16) - Y1 : vertex -> first scanline, in [0,65536)
CeilTop   dd  0     ; ceil(Y_top) — BOTH first sections start at the top vertex, and FLSNZ
PreYTop   dd  0     ; needs the same value, so compute it once per polygon, not three times
F65536    dd  65536.0
Inv65536  dd  0.0000152587890625
align 1
_DATA	ENDS

_TEXT	SEGMENT BYTE PUBLIC 'CODE'
	ASSUME	cs:_TEXT, ds:_DATA


IFDEF FDS_NGON
; ---------------------------------------------------------------------------------------
; N-gon entry. The left/right edge chains are built in C++ (NGon_BuildChains, FILLERS.CPP)
; and handed over ready to walk, so this skips the 3-vertex sort and the Longest side test
; entirely. Everything below FLSNZ is unchanged: the section machinery was ALREADY index
; driven -- Array[Section] is a section's start, Array[Section-1] its end, counting Section
; down to 1 -- so a 7-vertex chain walks on the same code a 3-vertex one did.
;
; NGonChains layout (KEEP IN SYNC with FILLERS.CPP):
;    0  void *Left[10]     bottom-first, top-last
;   40  void *Right[10]
;   80  long  NLeft        section count = vertices-1
;   84  long  NRight
;   88  void *Top
IFDEF FDS_TEXLIN
	PUBLIC	PT_TextureLin_32_N_
PT_TextureLin_32_N_:
ELSE
	PUBLIC	PT_Texture_32_N_
PT_Texture_32_N_:
ENDIF
mov [TEBP],ebp
;chains in ebx , bitmap in eax , where in ecx
mov [BitMap],eax
IFDEF FDS_TEX_SWIZZLE
IFDEF FDS_ZBUF
mov ds:[Texture2],eax           ; SMC base for the tiled z-tested fetch (ds: -- CS is read-only)
ENDIF
ENDIF
mov [Where],ecx
mov ecx,[ebx+80]
mov [Left_Section],ecx
mov ecx,[ebx+84]
mov [Right_Section],ecx
mov ecx,[ebx+88]
mov [Top],ecx
mov ebp,ecx                      ; FLSNZ reads the top vertex through ebp when !FDS_SUBTEX
mov eax,[ecx+Vertex_Y]           ; shared top ceil + prestep, same as the triangle entry
add eax,0FFFFh
sar eax,16
mov [CeilTop],eax
sal eax,16
sub eax,[ecx+Vertex_Y]
mov [PreYTop],eax
; Copy both chains in. A plain dword loop, not rep movsd: movsd needs es==ds, and es is not
; in this filler's #pragma aux modify list -- clobbering it would corrupt the caller.
mov esi,ebx
mov edi,offset Left_Array
mov ecx,20
@@NG_Copy:
mov eax,[esi]
add esi,4
mov [edi],eax
add edi,4
dec ecx
jnz @@NG_Copy
; Both chains start at the top vertex, so either can open with flat sections; skip them.
call CalcLeftSection
cmp ebx,0
jg @@NG_Lok
@@NG_Lnext:
mov ebx,[Left_Section]
dec ebx
jle TextureEnd
mov [Left_Section],ebx
call CalcLeftSection
cmp ebx,0
jle @@NG_Lnext
@@NG_Lok:
call CalcRightSection
cmp ebx,0
jg @@NG_Rok
@@NG_Rnext:
mov ebx,[Right_Section]
dec ebx
jle TextureEnd
mov [Right_Section],ebx
call CalcRightSection
cmp ebx,0
jle @@NG_Rnext
@@NG_Rok:
jmp FLSNZ
ENDIF

IFDEF FDS_TEXLIN
	; row-major twin for runtime-generated (dynamic) textures in the swizzle build
	PUBLIC	PT_TextureLin_32_
PT_TextureLin_32_:
ELSE
	PUBLIC	PT_Texture_32_
PT_Texture_32_:
ENDIF
mov [TEBP],ebp

;vtx in ebx , bitmap in eax , where in ecx

mov [BitMap],eax
IFDEF FDS_TEX_SWIZZLE
IFDEF FDS_ZBUF
mov ds:[Texture2],eax           ; SMC base for the tiled z-tested fetch (ds: -- CS is read-only)
ENDIF
ENDIF
mov [Where],ecx

mov ecx,ebx
mov esi,ebx
mov edi,ebx
add esi,20
add edi,40

;ecx - vertex 1
;esi - vertex 2
;edi - vertex 3

mov eax,[ecx+Vertex_Y]
cmp eax,[esi+Vertex_Y]
jle @@sort1
xchg ecx,esi
@@sort1:
mov eax,[ecx+Vertex_Y]
cmp eax,[edi+Vertex_Y]
jle @@sort2
xchg ecx,edi
@@sort2:
mov eax,[esi+Vertex_Y]
cmp eax,[edi+Vertex_Y]
jle @@sort3
xchg esi,edi
@@sort3:

mov ebx,[edi+Vertex_Y]
sub ebx,[ecx+Vertex_Y]
jz TextureEnd
IFDEF FDS_SUBTEX_MINH
; Option 2: reject exactly the triangles the INTEGER path rejected — those whose top and
; bottom vertices round onto the same scanline. Fractional Y gives them a nonzero height, so
; they now set up and rasterize a 1-scanline span; that is ~33% more triangles in city and is
; the whole measured cost of FDS_SUBTEX. Everything that survives keeps full sub-pixel
; accuracy; this only restores the old coverage decision.
mov eax,[edi+Vertex_Y]
add eax,8000h
sar eax,16
mov edx,[ecx+Vertex_Y]
add edx,8000h
sar edx,16
cmp eax,edx
je TextureEnd
ENDIF

;    int temp = ((v2->y - v1->y) << 16) / height;
;    int longest = temp * (v3->x - v1->x) + ((v1->x - v2->x) << 16);
;eax-longest

IFDEF FDS_SUBTEX
; Same sign test, but every term is already scaled by 65536, so the naive "sal 16" would
; overflow: build the 64-bit dividend with shld, and bring the 32.32 product back to 16.16.
mov eax,[esi+Vertex_Y]
mov ebp,[ecx+Vertex_X]
sub eax,[ecx+Vertex_Y]
sub ebp,[esi+Vertex_X]           ; already 16.16 - no shift
mov edx,eax
sar edx,31
shld edx,eax,16
sal eax,16
idiv ebx
mov ebx,[edi+Vertex_X]
sub ebx,[ecx+Vertex_X]
imul ebx                         ; 16.16 * 16.16 = 32.32
shrd eax,edx,16
add eax,ebp
mov [Longest],eax
ELSE
mov eax,[esi+Vertex_Y]
mov ebp,[ecx+Vertex_X]
sub eax,[ecx+Vertex_Y]
sub ebp,[esi+Vertex_X]
sal eax,16
sal ebp,16
mov edx,eax
sar edx,31
idiv ebx
mov ebx,[edi+Vertex_X]
sub ebx,[ecx+Vertex_X]
imul ebx
add eax,ebp
mov [Longest],eax
ENDIF
jz TextureEnd
jge LongestOverZero


mov [Top],ecx
IFDEF FDS_SUBTEX
; Both edges' first section begins at the TOP vertex, so its ceil and prestep are shared —
; and FLSNZ needs the same ceil for the scanline address. Compute once here instead of three
; times per polygon. (The MIDDLE vertex cannot be shared: the edge bends there, so its
; section genuinely needs its own prestep.)
mov eax,[ecx+Vertex_Y]
add eax,0FFFFh
sar eax,16
mov [CeilTop],eax
sal eax,16
sub eax,[ecx+Vertex_Y]
mov [PreYTop],eax
ENDIF
mov ebp,ecx
mov [Middle],esi
mov [Bottom],edi
mov [Right_Array],edi
mov [Right_Array+4],esi
mov [Right_Array+8],ecx
mov [Right_Section],2

mov [Left_Array],edi
mov [Left_Array+4],ecx
mov [Left_Section],1

call CalcLeftSection
cmp ebx,0
jle TextureEnd

call CalcRightSection
cmp ebx,0
jg FLSNZ  ; First Right Section Is Not Zero
mov eax,[Right_Section]
dec eax
mov [Right_Section],eax
call CalcRightSection
cmp ebx,0
jle TextureEnd
jmp FLSNZ

LongestOverZero:
mov [Top],ecx
IFDEF FDS_SUBTEX
; Both edges' first section begins at the TOP vertex, so its ceil and prestep are shared —
; and FLSNZ needs the same ceil for the scanline address. Compute once here instead of three
; times per polygon. (The MIDDLE vertex cannot be shared: the edge bends there, so its
; section genuinely needs its own prestep.)
mov eax,[ecx+Vertex_Y]
add eax,0FFFFh
sar eax,16
mov [CeilTop],eax
sal eax,16
sub eax,[ecx+Vertex_Y]
mov [PreYTop],eax
ENDIF
mov ebp,ecx
mov [Middle],esi
mov [Bottom],edi
mov [Left_Array],edi
mov [Left_Array+4],esi
mov [Left_Array+8],ecx
mov [Left_Section],2

mov [Right_Array],edi
mov [Right_Array+4],ecx
mov [Right_Section],1

call CalcRightSection
cmp ebx,0
jle TextureEnd

call CalcLeftSection
cmp ebx,0
jg FLSNZ  ; First Left Section Is Not Zero
mov eax,[Left_Section]
dec eax
mov [Left_Section],eax
call CalcLeftSection
cmp ebx,0
jle TextureEnd
FLSNZ:

IFDEF FDS_SUBTEX
mov edi,[CeilTop]                ; first scanline = ceil(Y_top), computed once above
ELSE
mov edi,[ebp+vertex_y]
ENDIF
mov edx,edi
sal edi,8
sal edx,10
add edi,[Where]
add edi,edx
mov [ScanLine],edi

OuterLoop:
;Inner Loop Start Here
mov eax,[Left_X]
mov ebp,[Right_X]
IFDEF FDS_SUBTEX
; fatmap2 S9/S10: spans run ceil(left_x) .. ceil(right_x)-1, HALF-OPEN. Adjacent polygons
; sharing an edge compute the same ceil and tile exactly, which is what makes this watertight
; and retires the right-inclusive overdraw hack. xfrac is the distance from the exact edge up
; to that first pixel, so the prestep below is POSITIVE and matches the start convention.
mov edx,eax
neg edx
and edx,0FFFFh
mov [xfracI],edx
add eax,0FFFFh
add ebp,0FFFFh
sar eax,16
mov edi,[ScanLine]
sar ebp,16
sub ebp,eax
ELSE
sar eax,16
mov edi,[ScanLine]
sar ebp,16
sub ebp,eax
ENDIF
jle UpdateValues
mov [XWidth],ebp
IFDEF FDS_ZBUF
mov [LineWidth],ebp
; FDS_ZLINEAR: encoded linear-Z computed per-16px checkpoint below, REUSING the perspective
; divide (1/Z) the texture already does -- no standalone z-block, no extra FDIV.
IFNDEF FDS_ZLINEAR
; endpoint codes first, CLAMPED to 16 bits (the adaptive scale can push codes past
; 65535 for geometry nearer than the scene estimate), then the exact step from the
; clamped ints - interior pixels stay within [min,max] of the endpoints.
FLD dword ptr [Right_Z]
FMUL dword ptr [_ZScaleF]
FISTP dword ptr [zcTmpR]        ; right-endpoint code
FLD dword ptr [Left_Z]
FMUL dword ptr [_ZScaleF]
FISTP dword ptr [zcAcc]         ; scanline-left code (pure FPU: eax is LIVE = Left_X>>16)
cmp dword ptr [zcTmpR],65534
jle @@ZcRok
mov dword ptr [zcTmpR],65534
@@ZcRok:
cmp dword ptr [zcAcc],65534
jle @@ZcLok
mov dword ptr [zcAcc],65534
@@ZcLok:
FILD dword ptr [zcTmpR]
FISUB dword ptr [zcAcc]
FIDIV dword ptr [LineWidth]
FISTP dword ptr [ZdPx]          ; per-pixel depth-code step
ENDIF
ENDIF
FLD dword ptr [Sixteen]
FILD dword ptr [XWidth]
FDIV                ;ST(6) - 1.0/Width
FLD dword ptr [Left_Z]   ;ST(5)
FLD dword ptr [Left_U]   ;ST(4)
FLD dword ptr [Left_V]   ;ST(3)
FLD dword ptr [Right_Z]  ;ST(2)
FLD dword ptr [Right_U]  ;ST(1)
FLD dword ptr [Right_V]  ;ST(0)
FSUB ST(0),ST(3) ;ST(1) = Delta V
FXCH ST(1)
FSUB ST(0),ST(4) ;ST(2) = Delta U
FXCH ST(2)
FSUB ST(0),ST(5) ;ST(0) = Delta Z
FMUL ST(0),ST(6)
FXCH ST(2)
FMUL ST(0),ST(6)
FXCH ST(1)
FMUL ST(0),ST(6)
FFREE ST(6)
IFDEF FDS_SUBTEX
; Sub-texel X prestep (fatmap2 S10), paired with the ceil span start: slide the start values
; from the exact fractional edge forward to the first pixel. Deltas are per-16-pixels, so the
; shift is Delta*xfrac/(65536*16) — no divide needed. Applied to the HOMOGENEOUS U,V,Z so every
; 16px checkpoint inherits it; gouraud R,G,B are left alone (sub-pixel light shift is invisible).
;   ST0=DeltaV ST1=DeltaU ST2=DeltaZ ST3=Left_V ST4=Left_U ST5=Left_Z  (+1 after each push)
FILD dword ptr [xfracI]
FMUL dword ptr [Inv1M]
FMUL ST(0),ST(1)
FADDP ST(4),ST(0)
FILD dword ptr [xfracI]
FMUL dword ptr [Inv1M]
FMUL ST(0),ST(2)
FADDP ST(5),ST(0)
FILD dword ptr [xfracI]
FMUL dword ptr [Inv1M]
FMUL ST(0),ST(3)
FADDP ST(6),ST(0)
ENDIF
lea edi,[edi+eax*4]
FLD One
FDIV ST(0),ST(6)                ; ST(0) = Z at checkpoint 0
IFDEF FDS_ZLINEAR
FLD ST(0)                       ; enc0 = 0FF80h - ZLinScale*Z (REUSE divide). eax dead here.
FMUL dword ptr [_ZLinScale]
FISTP dword ptr [zScTmp]
mov eax,0FF80h
sub eax,[zScTmp]
jg @@zclmp0                    ; clamp: enc<=0 (Z past ~1.1*FZP) wraps to a FALSE-NEAR
mov eax,1               ; word -> occludes near geom; clamp to farthest (1>cleared 0)
@@zclmp0:
mov [enc0],eax
ENDIF
mov cl,[Edi]
FMUL dword ptr [SixtyFive]
FLD ST(0)
FMUL ST(0),ST(6)
FISTP dword ptr [u0]
FMUL ST(0),ST(4)
FISTP dword ptr [v0]

;Left Z   ST(5)
;Left U   ST(4)
;Left V   ST(3)
;Delta Z  ST(2)
;Delta U  ST(1)
;Delta V  ST(0)

FXCH ST(3)  ;ST(0) = V    ST(3) = Delta V
FADD ST(0),ST(3)
FXCH ST(1)  ;ST(0) = Delta U    ST(1) = V
FXCH ST(4)  ;ST(0) = U    ST(4) = Delta U
FADD ST(0),ST(4)
FXCH ST(2)  ;ST(0) = Delta Z    ST(2) = U
FXCH ST(5)  ;ST(0) = Z    ST(5) = Delta Z
FADD ST(0),ST(5)

;Delta Z  ST(5)
;Delta U  ST(4)
;Delta V  ST(3)
;Left U   ST(2)
;Left V   ST(1)
;Left Z   ST(0)
mov ebx,0
mov ecx,[u0]
mov edx,[v0]
mov   bl, ch
mov   bh, dh
mov ecx,[BitMap]

FLD dword ptr [One]
FDIV ST(0),ST(1)                ; ST(0) = Z at checkpoint 1
IFDEF FDS_ZLINEAR
FLD ST(0)                       ; enc1 = 0FF80h - ZLinScale*Z (reuse divide)
FMUL dword ptr [_ZLinScale]
FISTP dword ptr [zScTmp]
mov eax,0FF80h
sub eax,[zScTmp]
jg @@zclmp1                    ; clamp: enc<=0 (Z past ~1.1*FZP) wraps to a FALSE-NEAR
mov eax,1               ; word -> occludes near geom; clamp to farthest (1>cleared 0)
@@zclmp1:
mov [enc1],eax
ENDIF
mov cl,[ecx+ebx*4]

FMUL dword ptr [SixtyFive]
FLD ST(0)
FMUL ST(0),ST(4)
FISTP dword ptr [u1]
FMUL ST(0),ST(2)
FISTP dword ptr [v1]


InnerOuterLoop:

;Delta Z  ST(5)
;Delta U  ST(4)
;Delta V  ST(3)
;Left U   ST(2)
;Left V   ST(1)
;Left Z   ST(0)

FADD ST(0),ST(5)
FXCH ST(1)
FADD ST(0),ST(3)
FXCH ST(2)
FADD ST(0),ST(4)
FXCH ST(2)
FXCH ST(1)
FLD dword ptr [One]
FDIV ST(0),ST(1)
mov esi,[u1]
mov edx,[v1]
sub esi,[u0]
sub edx,[v0]
sar esi,4
mov eax,[Xwidth]
sar edx,4
mov ebp,eax
sub eax,16
cmp ebp,16
jle @@NoOver16
mov ebp,16
@@NoOver16:
mov [dv],edx
mov [du],esi
mov [Xwidth],eax
IFDEF FDS_ZLINEAR
mov eax,[enc1]                  ; per-pixel encoded-Z step = (enc1-enc0)/16 (sar; partial-safe)
sub eax,[enc0]
sar eax,4
mov [ZdPx],eax
ENDIF
IFDEF FDS_TEX_SWIZZLE
	; Interleaved tiled accumulators (see BITRUE.ASM for the layout / carry-ripple notes):
	;   U: hi5@[27:31] gap(8) lo3@[16:18] guard frac15@[0:14]   loop mask 0F8077FFFh
	;   V: gap(5) int8@[19:26] gap(4)       frac15@[0:14]       loop mask 07F87FFFh
	; (Uacc+Vacc)>>16 = ((U>>3)<<11)|(V<<3)|(U&7) = the tiled index. Inputs are 8.8.
	mov eax,[du]
	mov ebx,eax
	and eax,0FFh
	shl eax,7                 ; du frac15
	mov ecx,ebx
	and ebx,0700h
	shl ebx,8                 ; du lo3 -> [16:18]
	or  eax,ebx
	and ecx,0F800h
	shl ecx,16                ; du hi5 -> [27:31]
	or  eax,ecx
	or  eax,07F88000h         ; gap+guard bits = 1 (carry ripple)
	mov [TilDuDx],eax
	mov eax,[dv]
	mov edx,eax
	and edx,0FF00h
	shl edx,11                ; dv int8 -> [19:26]
	and eax,0FFh
	shl eax,7                 ; dv frac15
	or  eax,edx
	or  eax,0F8078000h        ; gap bits = 1
	mov [TilDvDx],eax
	mov ecx,[u0]
	mov eax,ecx
	mov ebx,ecx
	and eax,0FFh
	shl eax,7
	and ebx,0700h
	shl ebx,8
	or  eax,ebx
	and ecx,0F800h
	shl ecx,16
	or  ecx,eax               ; ecx = interleaved U accumulator (gaps 0)
	mov edx,[v0]
	mov eax,edx
	and edx,0FF00h
	shl edx,11
	and eax,0FFh
	shl eax,7
	or  edx,eax               ; edx = interleaved V accumulator (gaps 0)
	mov esi,[BitMap]
IFDEF FDS_ZBUF
	mov esi,ebp               ; SMC freed esi: hold the count in a register (was zcounter)
	mov ebp,edi               ; this mapper walks edi FORWARD from the span start
	sub ebp,[Where]
	shr ebp,1
	add ebp,[_ZBufPtr]
IFDEF FDS_ZLINEAR
	mov eax,[enc0]           ; linear-Z: RE-ANCHOR the memory accumulator to this sub-span start
	mov [zcAcc],eax
ENDIF
ENDIF
ELSE
mov ebx,0
mov esi,[BitMap]
mov ecx,[u0]
mov edx,[v0]
;sar ecx,8
;sar edx,8
mov   bl, ch
mov   bh, dh
IFDEF FDS_ZBUF
mov [zcounter],ebp        ; count to memory: ebp becomes the z16 pointer
mov ebp,edi               ; edi = span start (this mapper walks it forward)
sub ebp,[Where]
shr ebp,1
add ebp,[_ZBufPtr]
IFDEF FDS_ZLINEAR
mov eax,[enc0]           ; linear-Z: RE-ANCHOR the memory accumulator to this sub-span start
mov [zcAcc],eax
ENDIF
ENDIF
ENDIF


;@@InnerLoop:
;		mov   eax, [edi]
;		add   edx, [dv]                 ; update v
;		add   eax, [esi+ebx*4]          ; get pixel from aligned texture map (oh sheloh)
;		add   ecx, [du]                 ; update u
;		and   eax,0fefefefeh            ; flag out pesky bits from ovfl. off. (wont work!)
;		mov   bl, ch
;		shr   eax,1
;		mov   [edi], eax                ; plot pixel
;		mov   bh, dh
;		add   edi,4
;		dec   ebp
; jnz   @@InnerLoop

 @@InnerLoop:
IFDEF FDS_TEX_SWIZZLE
IFDEF FDS_ZBUF
		; transparent pass: z TEST-NO-WRITE (skip pixels behind nearer opaques; the
		; back-to-front transparent order handles xpar-vs-xpar, so never write z).
		lea   ebx,[ecx+edx]
		shr   ebx,16                    ; tiled texel index
		mov   eax,[zcAcc]
		cmp   ax,[ebp]                  ; behind a nearer opaque? (strict: opaque codes
		jb    @@ZSkip                   ; are unbiased now; equal = coplanar decal, draws)
		; SMC texel fetch: mov ebx,[ebx*4 + Texture2]. A SIB with no base costs the same as
		; [esi+ebx*4] but frees esi, which now holds the loop counter -- turning a memory
		; read-modify-write (dec dword ptr) into a pairable register dec.
		db 08Bh,01Ch,09Dh
Texture2	dd ?
		mov   eax,[edi]                 ; framebuffer
		and   ebx,0fefefefeh
		and   eax,0fefefefeh
		shr   ebx,1
		shr   eax,1
		add   eax,ebx                   ; (fb+texel)/2
		mov   [edi],eax
@@ZSkip:
		mov   eax,[ZdPx]
		add   [zcAcc],eax               ; z step (memory: chains across spans for free)
		add   ecx,[TilDuDx]
		add   edx,[TilDvDx]
		and   ecx,0F8077FFFh
		and   edx,07F87FFFh
		add   ebp,2                     ; z pointer
		add   edi,4
		dec   esi
 jnz   @@InnerLoop
ELSE
		; tiled 50/50 blend: ecx/edx = interleaved u/v accumulators, ebx = index/texel,
		; eax = framebuffer scratch. Deltas from memory (regs are all spoken for).
		lea   ebx,[ecx+edx]
		shr   ebx,16                    ; tiled texel index
		add   ecx,[TilDuDx]             ; u += du (carry ripples through gap bits)
		add   edx,[TilDvDx]             ; v += dv
		and   ecx,0F8077FFFh            ; clear u gaps
		and   edx,07F87FFFh             ; clear v gaps
		mov   ebx,[esi+ebx*4]           ; texel (tiled layout)
		mov   eax,[edi]                 ; framebuffer (reflection under the water)
		and   ebx,0fefefefeh
		and   eax,0fefefefeh
		shr   ebx,1
		shr   eax,1                     ; "one less bit" each -> no cross-channel carry
		add   eax,ebx                   ; (fb+texel)/2, clean
		mov   [edi],eax                 ; plot pixel
		add   edi,4
		dec   ebp
 jnz   @@InnerLoop
ENDIF
ELSE
IFDEF FDS_ZBUF
		; row-major 50/50 blend + z TEST-NO-WRITE (dynamic transparents: vortex, portal).
		; Same strict policy as the tiled loop: skip only strictly-behind pixels.
		mov   eax,[zcAcc]
		cmp   ax,[ebp]                  ; behind a nearer opaque?
		jb    @@ZCSkip
		mov   eax, [edi]                ; framebuffer
		and   eax,0fefefefeh
		shr   eax,1
		mov   ebx, [esi+ebx*4]          ; texel
		and   ebx,0fefefefeh
		shr   ebx,1
		add   eax, ebx                  ; (fb+texel)/2, clean
		mov   [edi], eax                ; plot pixel
@@ZCSkip:
		mov   eax,[ZdPx]
		add   [zcAcc],eax               ; z step
		add   edx, [dv]                 ; update v
		add   ecx, [du]                 ; update u
		xor   ebx, ebx                  ; rebuild texel index for next pixel
		mov   bl, ch
		mov   bh, dh
		add   ebp,2                     ; z pointer
		add   edi,4
		dec   dword ptr [zcounter]
		jnz   @@InnerLoop
ELSE
		mov   eax, [edi]                ; framebuffer (reflection under the water)
		and   eax,0fefefefeh
		shr   eax,1                     ; fb>>1 (mask keeps the shift per-byte, <=127/byte)
		add   edx, [dv]                 ; update v
		add   ecx, [du]                 ; update u
		mov   ebx, [esi+ebx*4]          ; texel (index register is free after the fetch)
		and   ebx,0fefefefeh
		shr   ebx,1                     ; texel>>1 ("one less bit" each -> no cross-channel carry)
		add   eax, ebx                  ; (fb+texel)/2, clean
		mov   [edi], eax                ; plot pixel
		xor   ebx, ebx                  ; rebuild texel index for next pixel
		mov   bl, ch
		mov   bh, dh
		add   edi,4
		dec   ebp
 jnz   @@InnerLoop
ENDIF
ENDIF

mov eax,[u1]
mov ecx,[v1]
mov [u0],eax
mov [v0],ecx
IFDEF FDS_ZLINEAR
; shift enc0=enc1, then new enc1 from Z at the next checkpoint (ST(0)) -- reuse the divide.
mov eax,[enc1]
mov [enc0],eax
FLD ST(0)
FMUL dword ptr [_ZLinScale]
FISTP dword ptr [zScTmp]
mov eax,0FF80h
sub eax,[zScTmp]
jg @@zclmp2                    ; clamp: enc<=0 (Z past ~1.1*FZP) wraps to a FALSE-NEAR
mov eax,1               ; word -> occludes near geom; clamp to farthest (1>cleared 0)
@@zclmp2:
mov [enc1],eax
ENDIF
FMUL dword ptr [SixtyFive]
mov eax,[XWidth]
FLD ST(0)
FMUL ST(0),ST(4)
FISTP dword ptr [u1]
FMUL ST(0),ST(2)
FISTP dword ptr [v1]

cmp eax,0
jg InnerOuterLoop
FFREE ST(0)
FFREE ST(1)
FFREE ST(2)
FFREE ST(3)
FFREE ST(4)
FFREE ST(5)

;Inner Loop End Here

UpdateValues:
mov eax,[ScanLine]
mov ebx,[Left_Section_Height]
add eax,1280
dec ebx
mov [ScanLine],eax
mov [Left_Section_Height],ebx
jg @@Update_Left_UVXZ
; A section with zero scanlines is not the end of the chain -- an n-gon can have a flat
; edge anywhere, and a triangle can produce one from ceil(Y2)==ceil(Y1). Step past it.
@@NextLeftSection:
mov ebx,[Left_Section]
dec ebx
jle TextureEnd
mov [Left_Section],ebx
call CalcLeftSection
cmp ebx,0
jle @@NextLeftSection
jmp RightSectionCheck
@@Update_Left_UVXZ:
FLD dword ptr [Left_U]
FLD dword ptr [Left_V]
FXCH ST(1)
FADD dword ptr [Delta_Left_U]
FXCH ST(1)
FADD [Delta_Left_V]
FXCH ST(1)
FSTP dword ptr [Left_U]
FSTP dword ptr [Left_V]

FLD dword ptr [Left_Z]
mov eax,[Left_X]
FADD dword ptr [Delta_Left_Z]
add eax,[Delta_Left_X]
FSTP dword ptr [Left_Z]
mov [Left_X],eax

RightSectionCheck:
mov ebx,[Right_Section_Height]
dec ebx
mov [Right_Section_Height],ebx
jg @@Update_Right_UVXZ
@@NextRightSection:
mov ebx,[Right_Section]
dec ebx
jle TextureEnd
mov [Right_Section],ebx
call CalcRightSection
cmp ebx,0
jle @@NextRightSection
jmp OuterLoop

@@Update_Right_UVXZ:
FLD dword ptr [Right_U]
FLD dword ptr [Right_V]
FXCH ST(1)
FADD dword ptr [Delta_Right_U]
FXCH ST(1)
FADD [Delta_Right_V]
FXCH ST(1)
FSTP dword ptr [Right_U]
FSTP dword ptr [Right_V]

FLD dword ptr [Right_Z]
mov eax,[Right_X]
FADD dword ptr [Delta_Right_Z]
add eax,[Delta_Right_X]
FSTP dword ptr [Right_Z]
mov [Right_X],eax

jmp OuterLoop

TextureEnd:
mov ebp,[Tebp]
ret

CalcLeftSection proc near

mov eax,[Left_Section]
mov ecx,[Left_Array+eax*4]
sub eax,1
mov esi,[Left_Array+eax*4]
IFDEF FDS_SUBTEX
; ---- sub-pixel section setup (X,Y are 16.16) ----------------------------------------
; The section covers scanlines ceil(Y1)..ceil(Y2)-1; slopes come from the TRUE fractional
; height; and every interpolant is prestepped from the vertex to the first scanline. Without
; that prestep the attributes stay anchored at the vertex's fractional Y and are read up to a
; scanline early - the vertical half of the texture crawl. (The per-span X prestep in
; OuterLoop is the horizontal half, and the two are ONE decision: the prestep is only correct
; paired with the ceil span start. Floor + prestep measured worse in both sign directions,
; which is what made it look unwanted at first. See fatmap2 S9/S10 and MAPTEST A5.)
mov ebx,[esi+Vertex_Y]
sub ebx,[ecx+Vertex_Y]           ; dY (16.16, true height)
mov [Height],ebx
jle @@CLSzero
mov eax,[esi+Vertex_Y]
add eax,0FFFFh
sar eax,16                       ; ceil(Y2)
cmp ecx,[Top]
jne @@NoTopL
mov edx,[CeilTop]                ; shared: this section starts at the top vertex
sub eax,edx
mov ebx,eax
jle @@CLSzero
mov edx,[PreYTop]
mov [PreY],edx
jmp @@GotPreL
@@NoTopL:
mov edx,[ecx+Vertex_Y]
add edx,0FFFFh
sar edx,16                       ; ceil(Y1)
sub eax,edx
mov ebx,eax                      ; scanline count for this section
jle @@CLSzero
sal edx,16
sub edx,[ecx+Vertex_Y]
mov [PreY],edx                   ; prestep, 16.16 scanlines
@@GotPreL:
FILD dword ptr [Height]
FLD dword ptr [F65536]
FDIVR                            ; ST0 = 65536/dY16 = 1/dY in scanlines
FILD dword ptr [PreY]
FMUL dword ptr [Inv65536]        ; ST0 = prestep (scanlines)   ST1 = 1/dY
; X. A section can now be THINNER than a scanline, and then dX<<16/dY overflows idiv (it is
; a slope in pixels per scanline, and dY < 1 makes it unbounded). But such a section covers
; exactly one scanline, and UpdateValues moves on before ever applying the delta - so the
; slope is dead there and only the prestep matters. The prestep IS bounded: a section with
; >=1 scanline has ceil(Y1) < Y2, hence PreY < dY, so dX*PreY/dY < dX. One divide either way.
cmp ebx,1
jne @@MultiL
xor eax,eax
mov [Delta_Left_X],eax             ; single scanline: slope never applied
mov eax,[esi+Vertex_X]
sub eax,[ecx+Vertex_X]
imul dword ptr [PreY]            ; dX * PreY (32.32)
idiv dword ptr [Height]          ; / dY -> exact prestep, 16.16, |.| < |dX|
add eax,[ecx+Vertex_X]
mov [left_x],eax
jmp @@XDoneL
@@MultiL:
mov eax,[esi+Vertex_X]
sub eax,[ecx+Vertex_X]           ; dX (16.16)
mov edx,eax
sar edx,31
shld edx,eax,16                  ; 64-bit dX<<16 (dX is already scaled)
sal eax,16
idiv dword ptr [Height]          ; per-scanline dX, 16.16 (dY > 1 here, cannot overflow)
cmp edx,0
jge @@Flr1
dec eax
@@Flr1:
mov [Delta_Left_X],eax
imul dword ptr [PreY]            ; dXdy * prestep
shrd eax,edx,16
add eax,[ecx+Vertex_X]
mov [left_x],eax                   ; X on the first scanline, sub-pixel exact
@@XDoneL:
FLD dword ptr [esi+Vertex_Z]
FSUB dword ptr [ecx+Vertex_Z]
FMUL ST(0),ST(2)
FST dword ptr [Delta_Left_Z]
FMUL ST(0),ST(1)
FADD dword ptr [ecx+Vertex_Z]
FSTP dword ptr [Left_Z]
FLD dword ptr [esi+Vertex_U]
FSUB dword ptr [ecx+Vertex_U]
FMUL ST(0),ST(2)
FST dword ptr [Delta_Left_U]
FMUL ST(0),ST(1)
FADD dword ptr [ecx+Vertex_U]
FSTP dword ptr [Left_U]
FLD dword ptr [esi+Vertex_V]
FSUB dword ptr [ecx+Vertex_V]
FMUL ST(0),ST(2)
FST dword ptr [Delta_Left_V]
FMUL ST(0),ST(1)
FADD dword ptr [ecx+Vertex_V]
FSTP dword ptr [Left_V]
FSTP ST(0)
FSTP ST(0)
jmp @@CLSend
@@CLSzero:
xor ebx,ebx
jmp @@CLSend
ELSE
mov ebx,[esi+Vertex_Y]
sub ebx,[ecx+Vertex_Y]
jz @@CLSend
mov [Height],ebx
mov edx,[ecx+Vertex_X]
FILD dword ptr [Height]
sal edx,16
mov eax,[esi+Vertex_X]
sub eax,[ecx+Vertex_X]
FLD dword ptr [One]
sal eax,16
mov [left_x],edx
mov edx,eax
FDIVR
sar edx,31
idiv ebx
; watertight: idiv truncates toward zero; adjacent triangles sharing an edge
; need FLOOR so both walk the same x sequence (half-open spans then tile exactly)
cmp edx,0
jge @@Flr1
dec eax
@@Flr1:
FLD dword ptr [esi+Vertex_Z]
mov [Delta_Left_X],eax
FLD dword ptr [ecx+Vertex_Z]
FST [Left_Z]
FSUB
FMUL ST,ST(1)
FSTP dword ptr [Delta_Left_Z]

FLD dword ptr [esi+Vertex_U]
FLD dword ptr [ecx+Vertex_U]
FST [Left_U]
FSUB
FMUL ST,ST(1)
FSTP dword ptr [Delta_Left_U]

FLD dword ptr [esi+Vertex_V]
FLD dword ptr [ecx+Vertex_V]
FST [Left_V]
FSUB
FMUL
FSTP dword ptr [Delta_Left_V]
ENDIF
@@CLSend:
mov [Left_Section_Height],ebx
  ret
CalcLeftSection endp



CalcRightSection proc near

mov eax,[Right_Section]
mov ecx,[Right_Array+eax*4]
sub eax,1
mov esi,[Right_Array+eax*4]
IFDEF FDS_SUBTEX
; ---- sub-pixel section setup (X,Y are 16.16) ----------------------------------------
; The section covers scanlines ceil(Y1)..ceil(Y2)-1; slopes come from the TRUE fractional
; height; and every interpolant is prestepped from the vertex to the first scanline. Without
; that prestep the attributes stay anchored at the vertex's fractional Y and are read up to a
; scanline early - the vertical half of the texture crawl. (The per-span X prestep in
; OuterLoop is the horizontal half, and the two are ONE decision: the prestep is only correct
; paired with the ceil span start. Floor + prestep measured worse in both sign directions,
; which is what made it look unwanted at first. See fatmap2 S9/S10 and MAPTEST A5.)
mov ebx,[esi+Vertex_Y]
sub ebx,[ecx+Vertex_Y]           ; dY (16.16, true height)
mov [Height],ebx
jle @@CRSzero
mov eax,[esi+Vertex_Y]
add eax,0FFFFh
sar eax,16                       ; ceil(Y2)
cmp ecx,[Top]
jne @@NoTopR
mov edx,[CeilTop]                ; shared: this section starts at the top vertex
sub eax,edx
mov ebx,eax
jle @@CRSzero
mov edx,[PreYTop]
mov [PreY],edx
jmp @@GotPreR
@@NoTopR:
mov edx,[ecx+Vertex_Y]
add edx,0FFFFh
sar edx,16                       ; ceil(Y1)
sub eax,edx
mov ebx,eax                      ; scanline count for this section
jle @@CRSzero
sal edx,16
sub edx,[ecx+Vertex_Y]
mov [PreY],edx                   ; prestep, 16.16 scanlines
@@GotPreR:
FILD dword ptr [Height]
FLD dword ptr [F65536]
FDIVR                            ; ST0 = 65536/dY16 = 1/dY in scanlines
FILD dword ptr [PreY]
FMUL dword ptr [Inv65536]        ; ST0 = prestep (scanlines)   ST1 = 1/dY
; X. A section can now be THINNER than a scanline, and then dX<<16/dY overflows idiv (it is
; a slope in pixels per scanline, and dY < 1 makes it unbounded). But such a section covers
; exactly one scanline, and UpdateValues moves on before ever applying the delta - so the
; slope is dead there and only the prestep matters. The prestep IS bounded: a section with
; >=1 scanline has ceil(Y1) < Y2, hence PreY < dY, so dX*PreY/dY < dX. One divide either way.
cmp ebx,1
jne @@MultiR
xor eax,eax
mov [Delta_Right_X],eax             ; single scanline: slope never applied
mov eax,[esi+Vertex_X]
sub eax,[ecx+Vertex_X]
imul dword ptr [PreY]            ; dX * PreY (32.32)
idiv dword ptr [Height]          ; / dY -> exact prestep, 16.16, |.| < |dX|
add eax,[ecx+Vertex_X]
mov [Right_X],eax
jmp @@XDoneR
@@MultiR:
mov eax,[esi+Vertex_X]
sub eax,[ecx+Vertex_X]           ; dX (16.16)
mov edx,eax
sar edx,31
shld edx,eax,16                  ; 64-bit dX<<16 (dX is already scaled)
sal eax,16
idiv dword ptr [Height]          ; per-scanline dX, 16.16 (dY > 1 here, cannot overflow)
cmp edx,0
jge @@Flr2
dec eax
@@Flr2:
mov [Delta_Right_X],eax
imul dword ptr [PreY]            ; dXdy * prestep
shrd eax,edx,16
add eax,[ecx+Vertex_X]
mov [Right_X],eax                   ; X on the first scanline, sub-pixel exact
@@XDoneR:
FLD dword ptr [esi+Vertex_Z]
FSUB dword ptr [ecx+Vertex_Z]
FMUL ST(0),ST(2)
FST dword ptr [Delta_Right_Z]
FMUL ST(0),ST(1)
FADD dword ptr [ecx+Vertex_Z]
FSTP dword ptr [Right_Z]
FLD dword ptr [esi+Vertex_U]
FSUB dword ptr [ecx+Vertex_U]
FMUL ST(0),ST(2)
FST dword ptr [Delta_Right_U]
FMUL ST(0),ST(1)
FADD dword ptr [ecx+Vertex_U]
FSTP dword ptr [Right_U]
FLD dword ptr [esi+Vertex_V]
FSUB dword ptr [ecx+Vertex_V]
FMUL ST(0),ST(2)
FST dword ptr [Delta_Right_V]
FMUL ST(0),ST(1)
FADD dword ptr [ecx+Vertex_V]
FSTP dword ptr [Right_V]
FSTP ST(0)
FSTP ST(0)
jmp @@CRSend
@@CRSzero:
xor ebx,ebx
jmp @@CRSend
ELSE
mov ebx,[esi+Vertex_Y]
sub ebx,[ecx+Vertex_Y]
jz @@CRSend
mov [Height],ebx
mov edx,[ecx+Vertex_X]
FILD dword ptr [Height]
sal edx,16
mov eax,[esi+Vertex_X]
sub eax,[ecx+Vertex_X]
FLD dword ptr [One]
sal eax,16
mov [Right_X],edx
mov edx,eax
FDIVR
sar edx,31
idiv ebx
; watertight: idiv truncates toward zero; adjacent triangles sharing an edge
; need FLOOR so both walk the same x sequence (half-open spans then tile exactly)
cmp edx,0
jge @@Flr2
dec eax
@@Flr2:
FLD dword ptr [esi+Vertex_Z]
mov [Delta_Right_X],eax
FLD dword ptr [ecx+Vertex_Z]
FST [Right_Z]
FSUB
FMUL ST,ST(1)
FSTP dword ptr [Delta_Right_Z]

FLD dword ptr [esi+Vertex_U]
FLD dword ptr [ecx+Vertex_U]
FST [Right_U]
FSUB
FMUL ST,ST(1)
FSTP dword ptr [Delta_Right_U]

FLD dword ptr [esi+Vertex_V]
FLD dword ptr [ecx+Vertex_V]
FST [Right_V]
FSUB
FMUL
FSTP dword ptr [Delta_Right_V]
ENDIF
@@CRSend:
mov [Right_Section_Height],ebx
  ret
CalcRightSection endp


_TEXT	ENDS

END