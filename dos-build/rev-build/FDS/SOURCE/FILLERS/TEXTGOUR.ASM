.586p
.mmx

_DATA	SEGMENT PARA PUBLIC 'DATA'
align 4
_XRes 			equ 1280
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
Left_R 		     	dd 0.0 ; float
Delta_left_R 	     	dd 0.0 ; float
Left_G 		     	dd 0.0 ; float
Delta_left_G 	     	dd 0.0 ; float
Left_B                  dd 0.0 ; float
Delta_left_B            dd 0.0 ; float
Right_R                 dd 0.0 ; float
Delta_right_R           dd 0.0 ; float
Right_G                 dd 0.0 ; float
Delta_right_G           dd 0.0 ; float
Right_B                 dd 0.0 ; float
Delta_right_B           dd 0.0 ; float
RevHeight               dd 0.0 ; float
RevWidth                dd 0.0 ; float
gour			dd 0
R                       dd 0
G                       dd 0
B                       dd 0
dr		      	dd 0
dg                      dd 0
dbb                     dd 0
uu			dd 0
vv			dd 0
u0                      dd 0
v0                      dd 0
u1                      dd 0
v1                      dd 0
du		      	dd 0
dv                      dd 0
XWidth			dd 0
Height			dd 0
Longest			dd 0
; FDS_NGON: chains hold up to 9 vertices (3 in, one per clip plane). Left_Array and
; RIght_Array MUST stay adjacent and equally sized - the n-gon prologue copies both in
; one run. (Original spelling kept; the assembler is case-insensitive.)
Left_Array              dd 10 dup(0)
RIght_Array             dd 10 dup(0)
Left_X 			dd 0
Delta_Left_X 		dd 0
Right_X 		dd 0
Delta_Right_X 		dd 0
BitMap                  dd 0
Where                   dd 0
EXTRN _g_TriIn  : DWORD        ; triangles handed to this mapper
EXTRN _g_TriDraw: DWORD        ; ...that survive setup and actually rasterize
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
Vertex_R  equ 20
Vertex_G  equ 24
Vertex_B  equ 28
OneTwo    dd  0.0625
One       dd  1.0
Sixteen   dd  16.0
Sixtyfive dd  256.0
; ---- FDS_SUBTEX: vertex X,Y arrive as 16.16 instead of snapped integers ----
xfracI    dd  0     ; (ceil(Left_X)<<16) - Left_X : exact edge -> first pixel (POSITIVE)
Inv1M     dd  0.00000095367431640625   ; 1/(65536*16): xfrac in units of the per-16px delta
PreY      dd  0     ; (ceil(Y1)<<16) - Y1 : vertex -> first scanline, in [0,65536)
CeilTop   dd  0     ; ceil(Y_top) — BOTH first sections start at the top vertex, and FLSNZ
PreYTop   dd  0     ; needs the same value, so compute it once per polygon, not three times
dZ16      dd  0     ; FDS_GOURCHUNK: the per-16px deltas live in MEMORY, not on the x87
dU16      dd  0     ; stack. They are loop-invariant, and a memory-operand FADD has the same
dV16      dd  0     ; 1-cyc throughput on P5 -- so this is free and cuts the live stack 6->3,
                    ; halving what SweepSpan has to spill around every MMX chunk.
F65536    dd  65536.0
Inv65536  dd  0.0000152587890625
D1				dd  0
D2				dd  0
D3				dd  0
Teax      dd  0
Tebx      dd  0
TempFB		db	0
align 8
MMXTemp			dq  0   ; staging for the fused-gouraud MMX light/step loads
TilDuDx                 dd 0   ; interleaved tiled du (FDS_TEX_SWIZZLE; gap bits pre-set)
TilDvDx                 dd 0   ; interleaved tiled dv
LineStart               dd 0   ; scanline pixel start, for the fused gouraud sweep
LineWidth               dd 0   ; scanline pixel count, for the fused gouraud sweep
IFDEF FDS_GOURCHUNK
; FDS_GOURCHUNK=<n>: run the MMX gouraud sweep every n textured pixels instead of once per
; scanline, so the VPage lines the texture pass just wrote are still hot when it reads them
; back. The emms + x87 teardown/rebuild is the SAME mechanism the per-scanline sweep already
; uses -- this only changes how often it runs, so n = whole scanline reproduces it exactly.
SweepPtr        dd 0            ; next pixel address to modulate
ChunkAcc        dd 0            ; pixels textured since the last sweep
LightAcc        dq 0            ; MMX light accumulator carried across chunks
XSave           dd 6 dup(0)     ; the 6 live x87 values, spilled across the MMX block
ENDIF
zcAcc                   dd 0   ; FDS_ZBUF: running 1/w depth code across the scanline
zcLine0                 dd 0   ; FDS_ZBUF: (retired: sweep ownership now uses OwnLine)
OwnLine                 db 512 dup(0)  ; FDS_ZBUF: per-scanline ownership bits, indexed by
                                       ; (zptr>>1)&511 (row-consistent, 320<512 = no alias).
                                       ; Pixel loop SETS on draw, the sweep CONSUMES back to
                                       ; 0 - so it is all-zero again for the next face.
ZdPx                    dd 0   ; FDS_ZBUF: exact per-pixel depth-code step (1/w is screen-linear)
zcTmpR                  dd 0   ; FDS_ZBUF: right-endpoint code (clamp staging)
EXTRN _ZScaleF : DWORD         ; FDS_ZBUF: adaptive 1/w -> code scale (49152*wNear, C side)
EXTRN _ZLinScale : DWORD       ; FDS_ZLINEAR: fixed far-anchored linear-Z scale (C side)
enc0                    dd 0   ; FDS_ZLINEAR: encoded linear-Z at 16px checkpoint N (start)
enc1                    dd 0   ; FDS_ZLINEAR: encoded linear-Z at checkpoint N+1 (end)
zScTmp                  dd 0   ; FDS_ZLINEAR: FISTP scratch for the enc computation
EXTRN _ZBufPtr : DWORD         ; FDS_ZBUF: (long)ZBuf, 16-bit codes, px-parallel to VPage
align 1
_DATA	ENDS

_TEXT	SEGMENT BYTE PUBLIC 'CODE'
	ASSUME	cs:_TEXT, ds:_DATA


IFDEF FDS_NGON
; ---------------------------------------------------------------------------------------
; N-gon entry for the FUSED textured-gouraud mapper. Chains are built in C++ (see
; NGon_BuildChains in FILLERS.CPP, which documents the struct this reads).
;    0  void *Left[10]   40  void *Right[10]   80  long NLeft   84  long NRight   88  void *Top
	PUBLIC PG_Texture_32_N_
PG_Texture_32_N_:
mov [TEBP],ebp
inc dword ptr [_g_TriIn]
;chains in ebx , bitmap in eax , where in ecx
mov [BitMap],eax
; SMC self-patch of the in-code texture base -- the triangle entry does this below, and the
; n-gon entry jumps straight to FLSNZ, so it MUST do it too. Skipping it leaves the PREVIOUS
; face's texture patched into the inner loop: wrong texture, varying per face and per frame.
IFDEF FDS_TEX_SWIZZLE
mov ds:[Texture2],eax           ; SMC base for the tiled inner-loop fetch
ELSE
mov ds:[Texture1],eax           ; priming fetch base (pre-loop)
mov ds:[Texture],eax            ; SMC base for the row-major inner-loop fetch
ENDIF
mov [Where],ecx
mov ecx,[ebx+80]
mov [Left_Section],ecx
mov ecx,[ebx+84]
mov [Right_Section],ecx
mov ecx,[ebx+88]
mov [Top],ecx
mov ebp,ecx
mov eax,[ecx+Vertex_Y]
add eax,0FFFFh
sar eax,16
mov [CeilTop],eax
sal eax,16
sub eax,[ecx+Vertex_Y]
mov [PreYTop],eax
; plain dword loop, not rep movsd: movsd needs es==ds and es is not in the modify list.
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

	PUBLIC PG_Texture_32_
PG_Texture_32_:

mov [TEBP],ebp
inc dword ptr [_g_TriIn]

;vtx in ebx , bitmap in eax , where in ecx

mov [BitMap],eax
; SMC self-patch of the in-code texture base. Force DS (writable) — OW2 wasm's ASSUME cs:_TEXT
; otherwise emits a CS: write, and CS is execute/read-only under DOS/4GW (same linear base as
; DS, so the DS write patches the identical code location the fetch reads).
IFDEF FDS_TEX_SWIZZLE
mov ds:[Texture2],eax           ; SMC base for the tiled inner-loop fetch
ELSE
mov ds:[Texture1],eax           ; priming fetch base (pre-loop)
mov ds:[Texture],eax            ; SMC base for the row-major inner-loop fetch
ENDIF
mov [Where],ecx

mov ecx,ebx
mov esi,ebx
mov edi,ebx
add esi,32
add edi,64

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


comment &
mov eax,[ecx+Vertex_Y]
sub eax,[esi+Vertex_Y]
jz @@firsttwoequals

mov eax,[esi+Vertex_Y]
sub eax,[edi+Vertex_Y]
jz @@lasttwoequals

mov ebp,[ecx+Vertex_X]
sub ebp,[esi+Vertex_X]

jz TextureEnd
jge LongestOverZero
jmp LongestBelowZero
@@firsttwoequals:
mov ebp,[ecx+Vertex_X]
sub ebp,[esi+Vertex_X]
jz TextureEnd
jge LongestOverZero
jmp LongestBelowZero

@@lasttwoequals:
mov ebp,[esi+Vertex_X]
sub ebp,[edi+Vertex_X]
jz TextureEnd
jge LongestOverZero


LongestBelowZero:
&
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
inc dword ptr [_g_TriDraw]

IFDEF FDS_SUBTEX
mov edi,[CeilTop]                ; first scanline = ceil(Y_top), computed once above
ELSE
mov edi,[ebp+vertex_y]
ENDIF
mov eax,_Xres
mul edi
mov edi,eax
add edi,[Where]
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
inc ebp                 ; right-INCLUSIVE spans (legacy): 1px of overdraw the z-buffer resolves
ENDIF
jle UpdateValues
mov [XWidth],ebp
mov [LineWidth],ebp             ; fused gouraud: remember full scanline extent
IFDEF FDS_ZBUF
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
FISTP dword ptr [zcAcc]         ; scanline-left depth code
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
FISTP dword ptr [ZdPx]          ; per-pixel depth-code step (exact: 1/w screen-linear)
ENDIF
ENDIF

FLD dword ptr [SixtyFive]
FILD dword ptr [XWidth]
FDIV                ;ST(6) - 1.0/Width
FLD ST(0)
FLD dword ptr [Left_R]   ;ST(5)
FLD dword ptr [Left_G]   ;ST(4)
FLD dword ptr [Left_B]   ;ST(3)
FLD dword ptr [Right_R]  ;ST(2)
FLD dword ptr [Right_G]  ;ST(1)
FLD dword ptr [Right_B]  ;ST(0)
FSUB ST(0),ST(3) ;ST(1) = Delta B
FXCH ST(1)
FSUB ST(0),ST(4) ;ST(2) = Delta G
FXCH ST(2)
FSUB ST(0),ST(5) ;ST(0) = Delta R
FMUL ST(0),ST(6)
FXCH ST(2)
FMUL ST(0),ST(6)
FXCH ST(1)
FMUL ST(0),ST(6)
;FFREE ST(6)
FISTP dword ptr [dbb]
FISTP dword ptr [dg]
FISTP dword ptr [dr]
lea edi,[edi+eax*4]
mov [LineStart],edi             ; fused gouraud: scanline pixel start
IFDEF FDS_GOURCHUNK
mov [SweepPtr],edi
mov dword ptr [ChunkAcc],0
ENDIF
;FLD One
;FDIV ST(0),ST(6)
mov cl,[Edi]
FLD dword ptr [SixtyFive]
FLD ST(0)
FMUL ST(0),ST(4)
FISTP dword ptr [R]
FLD ST(0)
FMUL ST(0),ST(3)
FISTP dword ptr [G]
FMUL
FISTP dword ptr [B]
FFREE ST(0)
FFREE ST(1)
FFREE ST(2)
FINCSTP
FINCSTP
FINCSTP
FLD dword ptr [OneTwo]
FMUL
;FILD dword ptr [XWidth]
;FDIV                ;ST(6) - 1.0/Width

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
; ST0=DeltaV ST1=DeltaU ST2=DeltaZ ST3=Left_V ST4=Left_U ST5=Left_Z -> park the deltas.
FSTP dword ptr [dV16]
FSTP dword ptr [dU16]
FSTP dword ptr [dZ16]
; now ST0=Left_V ST1=Left_U ST2=Left_Z
FLD One
FDIV ST(0),ST(3)                ; ST(0) = Z at checkpoint 0
IFDEF FDS_ZLINEAR
FLD ST(0)                       ; enc0 = 0FF80h - ZLinScale*Z (REUSE divide, no extra FDIV)
FMUL dword ptr [_ZLinScale]
FISTP dword ptr [zScTmp]
mov eax,0FF80h
sub eax,[zScTmp]
jg @@zclmp0                    ; clamp: enc<=0 (Z past ~1.1*FZP) wraps to a FALSE-NEAR
mov eax,1               ; word -> occludes near geom; clamp to farthest (1>cleared 0)
@@zclmp0:
mov [enc0],eax
ENDIF
;mov cl,[Edi]
FMUL dword ptr [SixtyFive]
FLD ST(0)
FMUL ST(0),ST(3)
FISTP dword ptr [u0]
FMUL ST(0),ST(1)
FISTP dword ptr [v0]

;Left Z   ST(5)
;Left U   ST(4)
;Left V   ST(3)
;Delta Z  ST(2)
;Delta U  ST(1)
;Delta V  ST(0)

; (V,U,Z) -> advance each to checkpoint 1 and land on (Z,V,U), the loop's layout.
FADD dword ptr [dV16]
FXCH ST(1)
FADD dword ptr [dU16]
FXCH ST(2)
FADD dword ptr [dZ16]

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


IFDEF FDS_GOURCHUNK
mov eax,[B]                     ; seed the carried light (lane0=B lane1=G lane2=R)
mov word ptr [LightAcc],ax
mov eax,[G]
mov word ptr [LightAcc+2],ax
mov eax,[R]
mov word ptr [LightAcc+4],ax
mov word ptr [LightAcc+6],0
ENDIF
InnerOuterLoop:

;Delta Z  ST(5)
;Delta U  ST(4)
;Delta V  ST(3)
;Left U   ST(2)
;Left V   ST(1)
;Left Z   ST(0)

FADD dword ptr [dZ16]
FXCH ST(1)
FADD dword ptr [dV16]
FXCH ST(2)
FADD dword ptr [dU16]
FXCH ST(2)
FXCH ST(1)
FLD dword ptr [One]
FDIV ST(0),ST(1)
;mov esi,[u0]
;mov edx,[v0]
;mov [uu],esi
;mov [vv],edx


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
mov [Xwidth],eax
IFDEF FDS_ZLINEAR
; per-pixel encoded-Z step = (enc1-enc0)/16 (sar 4; correct for the partial last sub-span too).
; edx is dv (live); sar touches only eax (reloaded below).
mov eax,[enc1]
sub eax,[enc0]
sar eax,4
mov [ZdPx],eax
ENDIF

; ---- texture sub-pass: BITRUE-style span machinery (esi=du 8.8, edx=dv 8.8) ----
IFDEF FDS_TEX_SWIZZLE
	; Interleaved tiled accumulators (see BITRUE.ASM for layout / carry-ripple notes).
	mov eax,esi
	mov ebx,esi
	and eax,0FFh
	shl eax,7                 ; du frac15
	mov ecx,esi
	and ebx,0700h
	shl ebx,8                 ; du lo3 -> [16:18]
	or  eax,ebx
	and ecx,0F800h
	shl ecx,16                ; du hi5 -> [27:31]
	or  eax,ecx
	or  eax,07F88000h         ; gap+guard bits = 1 (carry ripple)
	mov [TilDuDx],eax
	mov eax,edx
	mov ebx,edx
	and ebx,0FF00h
	shl ebx,11                ; dv int8 -> [19:26]
	and eax,0FFh
	shl eax,7                 ; dv frac15
	or  eax,ebx
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
	or  ecx,eax               ; ecx = interleaved U accumulator
	mov ebx,[v0]
	mov eax,ebx
	and ebx,0FF00h
	shl ebx,11
	and eax,0FFh
	shl eax,7
	or  ebx,eax               ; ebx = interleaved V accumulator
IFDEF FDS_ZBUF
IFDEF FDS_ZLINEAR
	mov eax,[enc0]           ; linear-Z: RE-ANCHOR to this 16px sub-span's start checkpoint enc
ELSE
	mov eax,[zcAcc]           ; hyperbolic: chained 1/w code (UV deltas come from memory below)
ENDIF
	mov edx,edi               ; edi = span START address here (the counter transform that
	                          ; makes esi negative runs AFTER this block - esi is still du!)
	sub edx,[Where]           ; -> pixel byte offset (px*4)
	shr edx,1                 ; -> px*2
	add edx,[_ZBufPtr]        ; edx = &z16[first pixel of span]
ELSE
	mov eax,[TilDuDx]
	mov edx,[TilDvDx]
ENDIF
ELSE
; Create EAX / EBX / ECX / EDX (classic packed 8.8 scheme, as in BITRUE)
mov eax,esi
shl eax,24
mov ax,dx
mov ebx,[u0]
shl ebx,24
mov ecx,[v0]
mov bx,cx
mov ecx,[u0]
shr ecx,8
mov edx,esi
shr edx,8
ENDIF
; counter / dest advance
mov esi,ebp
shl esi,2
add edi,esi
xor esi,-1
inc esi
IFDEF FDS_TEX_SWIZZLE
	lea ebp,[ecx+ebx]         ; prime pixel 0's combined index
ELSE
and ecx,0ffffh
mov ch,bh
	db 08Bh,02Ch,08Dh ; |
	Texture1 dd ?      ; | <-- mov ebp,[TEXTURE+ecx*4]
ENDIF
@@InnerLoop:
IFDEF FDS_TEX_SWIZZLE
IFDEF FDS_ZBUF
	; z-test first: skip the fetch+store for occluded pixels (front-to-back pays here).
	; FIRST-WINS: jbe (equal skips) + unbiased write - see BITRUE.ASM; at far 16-bit code
	; collapse, draw-on-equal made the FARTHEST face win every tie (reverse painter's).
	cmp ax,[edx]              ; 16-bit z test
	jbe @@ZSkip
	mov [edx],ax              ; z write (unbiased)
	shr ebp,16                ; tiled texel index
	add ecx,[TilDuDx]         ; u += du (carry ripples through gaps)
	add ebx,[TilDvDx]         ; v += dv
	and ecx,0F8077FFFh        ; clear u gaps
	db 08Bh,02Ch,0ADh         ; mov ebp,[ebp*4 + Texture2]
	Texture2 dd ?
	and ebx,07F87FFFh         ; clear v gaps
	mov [edi+esi],ebp         ; plot
	mov ebp,edx               ; ownership bit for the gouraud sweep: OwnLine[(zptr>>1)&511]
	shr ebp,1                 ; (ebp is free until the lea below reloads it)
	and ebp,511
	mov byte ptr [OwnLine+ebp],1
	lea ebp,[ecx+ebx]         ; next pixel's combined index
	add eax,[ZdPx]            ; z step
	add edx,2                 ; z pointer
	add esi,4
	jnz @@InnerLoop
	jmp @@SpanDone
@@ZSkip:
	add ecx,[TilDuDx]         ; still advance the UV accumulators
	add ebx,[TilDvDx]
	and ecx,0F8077FFFh
	and ebx,07F87FFFh
	lea ebp,[ecx+ebx]
	add eax,[ZdPx]            ; z step
	add edx,2                 ; z pointer
	add esi,4
	jnz @@InnerLoop
@@SpanDone:
	mov [zcAcc],eax           ; carry the depth code into the next span
ELSE
	shr ebp,16                ; tiled texel index
	add ecx,eax               ; u += du (carry ripples through gaps)
	add ebx,edx               ; v += dv
	and ecx,0F8077FFFh        ; clear u gaps
	db 08Bh,02Ch,0ADh         ; mov ebp,[ebp*4 + Texture2]
	Texture2 dd ?
	and ebx,07F87FFFh         ; clear v gaps
	mov [edi+esi],ebp         ; plot
	lea ebp,[ecx+ebx]         ; next pixel's combined index
	add esi,4
	jnz @@InnerLoop
ENDIF
ELSE
	add ebx,eax
	mov [edi+esi],ebp
	db 08Bh,02Ch,08Dh ; |
	Texture dd ?      ; | <-- mov ebp,[TEXTURE+ecx*4]
	mov ch,bh
	adc cl,dl
	add esi,4
	jnz @@InnerLoop
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

IFDEF FDS_GOURCHUNK
; eax holds the remaining span width here -- preserve it across the sweep.
push eax
mov eax,[ChunkAcc]
add eax,16
mov [ChunkAcc],eax
cmp eax,FDS_GOURCHUNK
jl @@NoChunkYet
call SweepSpan
@@NoChunkYet:
pop eax
ENDIF
cmp eax,0
jg InnerOuterLoop
IFDEF FDS_GOURCHUNK
mov dword ptr [ChunkAcc],1000000    ; flush the tail (SweepSpan clamps to what is left)
call SweepSpan
ENDIF
FFREE ST(0)
FFREE ST(1)
FFREE ST(2)

IFDEF FDS_NOSWEEP
jmp UpdateValues                ; diagnostic: raw texture out of the fused path
ENDIF
IFDEF FDS_GOURCHUNK
jmp UpdateValues                ; already swept in chunks above
ENDIF
; ---- fused gouraud sub-pass: modulate the just-textured scanline while it is L1-hot ----
; Lane order: pixel bytes are B,G,R,0 little-endian -> word lane0 = BLUE. (First attempt
; copied GOURMMXT's [r]->lane0 literally and produced an R/B swap - its edge code feeds
; its 'r' variable from the opposite end of the vertex fields.)
mov edi,[LineStart]
mov esi,[LineWidth]
lea ebp,[edi+esi*4]             ; scanline end
mov edi,esi
shl edi,2
xor edi,-1
inc edi                         ; edi = -width*4
emms
mov eax,[B]
mov word ptr [MMXTemp],ax
mov eax,[G]
mov word ptr [MMXTemp+2],ax
mov eax,[R]
mov word ptr [MMXTemp+4],ax
movq mm0,[MMXTemp]              ; light (3 words, 8.8): lane0=B lane1=G lane2=R (BGR0 pixel)
mov eax,[dbb]
mov word ptr [MMXTemp],ax
mov eax,[dg]
mov word ptr [MMXTemp+2],ax
mov eax,[dr]
mov word ptr [MMXTemp+4],ax
movq mm1,[MMXTemp]              ; per-pixel light step
IFDEF FDS_ZBUF
mov ecx,[LineStart]
sub ecx,[Where]
shr ecx,1
add ecx,[_ZBufPtr]              ; ecx = &z16[scanline start]; (ecx>>1)&511 indexes OwnLine
ENDIF
@@GourSweep:
movq mm3,mm0
IFDEF FDS_ZBUF
IFNDEF FDS_NOOWN
; ownership test: modulate ONLY pixels this face's texture pass drew. Code-replay compare
; broke at equal 16-bit codes (far collapse): an occluded face skipped every pixel yet
; matched every code, re-modulating the nearer face's final colors = dark far geometry.
mov eax,ecx
shr eax,1
and eax,511
cmp byte ptr [OwnLine+eax],0
je @@GNext
mov byte ptr [OwnLine+eax],0    ; consume: line is all-zero again for the next face
ENDIF
ENDIF
punpcklbw mm2,[edi+ebp]         ; texel bytes -> high bytes of words
psrlw mm3,8                     ; light 8.8 -> int
psrlw mm2,8                     ; -> texel words
pmullw mm2,mm3
psrlw mm2,7                     ; GOURMMXT scale: light 128 = 1.0
packuswb mm2,mm2
movd [edi+ebp],mm2
IFDEF FDS_ZBUF
@@GNext:
add ecx,2
ENDIF
paddw mm0,mm1
add edi,4
jnz @@GourSweep
emms

;Inner Loop End Here

UpdateValues:
mov eax,[ScanLine]
mov ebx,[Left_Section_Height]
add eax,_XRes
dec ebx
mov [ScanLine],eax
mov [Left_Section_Height],ebx
jg @@Update_Left_UVXZ
; A zero-scanline section is not the end of the chain -- an n-gon can have a flat edge
; anywhere, and a triangle can produce one from ceil(Y2)==ceil(Y1). Step past it.
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
FADD dword ptr [Delta_Left_Z]
FSTP dword ptr [Left_Z]


FLD dword ptr [Left_R]
FLD dword ptr [Left_G]
FXCH ST(1)
FADD dword ptr [Delta_Left_R]
FXCH ST(1)
FADD [Delta_Left_G]
FXCH ST(1)
FSTP dword ptr [Left_R]
FSTP dword ptr [Left_G]


FLD dword ptr [Left_B]
mov eax,[Left_X]
FADD dword ptr [Delta_Left_B]
add eax,[Delta_Left_X]
FSTP dword ptr [Left_B]
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
FADD dword ptr [Delta_Right_Z]
FSTP dword ptr [Right_Z]


FLD dword ptr [Right_R]
FLD dword ptr [Right_G]
FXCH ST(1)
FADD dword ptr [Delta_Right_R]
FXCH ST(1)
FADD [Delta_Right_G]
FXCH ST(1)
FSTP dword ptr [Right_R]
FSTP dword ptr [Right_G]


FLD dword ptr [Right_B]
mov eax,[Right_X]
FADD dword ptr [Delta_Right_B]
add eax,[Delta_Right_X]
FSTP dword ptr [Right_B]
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
; scanline early - the vertical half of the texture crawl. R,G,B are deliberately NOT
; prestepped: a sub-scanline shift in gouraud light is imperceptible, and skipping it returns
; those three to the original 6-op sequence (section setup is ~70k calls/frame in city, so
; per-attribute ops are the whole cost of this feature). (The per-span X prestep in
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
FLD dword ptr [esi+Vertex_R]
FLD dword ptr [ecx+Vertex_R]
FST dword ptr [Left_R]
FSUB
FMUL ST,ST(2)
FSTP dword ptr [Delta_Left_R]
FLD dword ptr [esi+Vertex_G]
FLD dword ptr [ecx+Vertex_G]
FST dword ptr [Left_G]
FSUB
FMUL ST,ST(2)
FSTP dword ptr [Delta_Left_G]
FLD dword ptr [esi+Vertex_B]
FLD dword ptr [ecx+Vertex_B]
FST dword ptr [Left_B]
FSUB
FMUL ST,ST(2)
FSTP dword ptr [Delta_Left_B]
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
FMUL ST,ST(1)
FSTP dword ptr [Delta_Left_V]

FLD dword ptr [esi+Vertex_R]
FLD dword ptr [ecx+Vertex_R]
FST [Left_R]
FSUB
FMUL ST,ST(1)
FSTP dword ptr [Delta_Left_R]

FLD dword ptr [esi+Vertex_G]
FLD dword ptr [ecx+Vertex_G]
FST [Left_G]
FSUB
FMUL ST,ST(1)
FSTP dword ptr [Delta_Left_G]


FLD dword ptr [esi+Vertex_B]
FLD dword ptr [ecx+Vertex_B]
FST [Left_B]
FSUB
FMUL
FSTP dword ptr [Delta_Left_B]
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
; scanline early - the vertical half of the texture crawl. R,G,B are deliberately NOT
; prestepped: a sub-scanline shift in gouraud light is imperceptible, and skipping it returns
; those three to the original 6-op sequence (section setup is ~70k calls/frame in city, so
; per-attribute ops are the whole cost of this feature). (The per-span X prestep in
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
FLD dword ptr [esi+Vertex_R]
FLD dword ptr [ecx+Vertex_R]
FST dword ptr [Right_R]
FSUB
FMUL ST,ST(2)
FSTP dword ptr [Delta_Right_R]
FLD dword ptr [esi+Vertex_G]
FLD dword ptr [ecx+Vertex_G]
FST dword ptr [Right_G]
FSUB
FMUL ST,ST(2)
FSTP dword ptr [Delta_Right_G]
FLD dword ptr [esi+Vertex_B]
FLD dword ptr [ecx+Vertex_B]
FST dword ptr [Right_B]
FSUB
FMUL ST,ST(2)
FSTP dword ptr [Delta_Right_B]
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
FMUL ST,ST(1)
FSTP dword ptr [Delta_Right_V]


FLD dword ptr [esi+Vertex_R]
FLD dword ptr [ecx+Vertex_R]
FST [Right_R]
FSUB
FMUL ST,ST(1)
FSTP dword ptr [Delta_Right_R]

FLD dword ptr [esi+Vertex_G]
FLD dword ptr [ecx+Vertex_G]
FST [Right_G]
FSUB
FMUL ST,ST(1)
FSTP dword ptr [Delta_Right_G]


FLD dword ptr [esi+Vertex_B]
FLD dword ptr [ecx+Vertex_B]
FST [Right_B]
FSUB
FMUL
FSTP dword ptr [Delta_Right_B]
ENDIF
@@CRSend:
mov [Right_Section_Height],ebx
  ret
CalcRightSection endp


IFDEF FDS_GOURCHUNK
; ---------------------------------------------------------------------------------------
; Modulate the pixels textured since the last call. Same MMX body as the per-scanline sweep;
; the only new work is spilling the six live x87 values, because MMX aliases the x87 stack.
; pushad/popad because the caller keeps edi (framebuffer walk) and ebp live across iterations.
SweepSpan proc near
pushad
; ST0=Left Z  ST1=Left V  ST2=Left U  (the deltas live in memory now)
FSTP dword ptr [XSave]
FSTP dword ptr [XSave+4]
FSTP dword ptr [XSave+8]

mov edi,[SweepPtr]
mov esi,[LineStart]
mov eax,[LineWidth]
lea esi,[esi+eax*4]
sub esi,edi
sar esi,2                       ; pixels left in this scanline
jle @@SSrestore                 ; nothing left (also the tail-flush no-op case)
mov eax,[ChunkAcc]
cmp eax,esi
jle @@SShave
mov eax,esi                     ; clamp: never sweep past the span end
@@SShave:
mov esi,eax

IFDEF FDS_ZBUF
mov ecx,edi
sub ecx,[Where]
shr ecx,1
add ecx,[_ZBufPtr]              ; ecx = &z16[chunk start]
ENDIF
lea ebp,[edi+esi*4]             ; chunk end == next chunk start
mov edi,esi
shl edi,2
xor edi,-1
inc edi                         ; edi = -count*4
movq mm0,[LightAcc]             ; light carried from the previous chunk
mov eax,[dbb]
mov word ptr [MMXTemp],ax
mov eax,[dg]
mov word ptr [MMXTemp+2],ax
mov eax,[dr]
mov word ptr [MMXTemp+4],ax
movq mm1,[MMXTemp]
@@SSweep:
movq mm3,mm0
IFDEF FDS_ZBUF
IFNDEF FDS_NOOWN
mov eax,ecx
shr eax,1
and eax,511
cmp byte ptr [OwnLine+eax],0
je @@SSnext
mov byte ptr [OwnLine+eax],0
ENDIF
ENDIF
punpcklbw mm2,[edi+ebp]
psrlw mm3,8
psrlw mm2,8
pmullw mm2,mm3
psrlw mm2,7
packuswb mm2,mm2
movd [edi+ebp],mm2
IFDEF FDS_ZBUF
@@SSnext:
add ecx,2
ENDIF
paddw mm0,mm1
add edi,4
jnz @@SSweep
movq [LightAcc],mm0
emms
mov [SweepPtr],ebp
mov dword ptr [ChunkAcc],0
@@SSrestore:
FLD dword ptr [XSave+8]
FLD dword ptr [XSave+4]
FLD dword ptr [XSave]
popad
ret
SweepSpan endp
ENDIF

_TEXT	ENDS

END