comment &
The BiTrue Mapper! ;)
this piece of shit is just a quick job , i need to put sub texeling and sub
pixeling , push a faster inner loop with cache considiration , but that's
for later.
the transperncy Mappper needs shr's by 1 textures , and ANDed with
7fff7fff7fff7fffh or something like that ;) (opps , that was actually 7f7f7f7fh)
btw , the transperncy mapper and the normal mapper are almost the same thing.
in the CPP source , there is a DeJPEG example , and 32-->24 BPP fliper and
stuff like that.
i'll include also the unarjer.
in case you couldn't find out yourself , bitrue.asm is the normal mapper , and
the BiTTrue.asm is the transperncy mapper.

Diffuse Out.
&


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
TilDuDx                 dd 0   ; interleaved tiled du (FDS_TEX_SWIZZLE; gap bits pre-set to 1)
TilDvDx                 dd 0   ; interleaved tiled dv (FDS_TEX_SWIZZLE; gap bits pre-set to 1)
zcAcc                   dd 0   ; FDS_ZBUF: running 1/w depth code across the scanline
LineWidth               dd 0   ; FDS_ZBUF: original scanline width (FIDIV divisor)
ZdPx                    dd 0   ; FDS_ZBUF: exact per-pixel depth-code step (1/w screen-linear)
zcTmpR                  dd 0   ; FDS_ZBUF: right-endpoint code (clamp staging)
zcounter                dd 0   ; FDS_ZBUF: classic-loop pixel count (regs all spoken for)
; ---- FDS_ZLINEAR: IX-style linear-Z (enc = 0FF80h - zScale*Z), per-16px re-anchored ----
enc0                    dd 0   ; encoded linear-Z at 16px checkpoint N   (sub-span START)
enc1                    dd 0   ; encoded linear-Z at 16px checkpoint N+1 (sub-span END)
encStep                 dd 0   ; per-pixel encoded-Z step within the sub-span
zScTmp                  dd 0   ; FISTP scratch for the enc computation
EXTRN _ZScaleF : DWORD         ; FDS_ZBUF: adaptive 1/w -> code scale (49152*wNear, C side)
EXTRN _ZLinScale : DWORD       ; FDS_ZLINEAR: fixed linear scale = 0FF00h/(FZP*1.1), C side
EXTRN _ZBufPtr : DWORD         ; FDS_ZBUF: (long)ZBuf (16-bit codes, px-parallel to VPage)
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
PreY      dd  0     ; (ceil(Y1)<<16) - Y1 : vertex -> first scanline, in [0,65536)
CeilTop   dd  0     ; ceil(Y_top) — BOTH first sections start at the top vertex, and FLSNZ
PreYTop   dd  0     ; needs the same value, so compute it once per polygon, not three times
xfracI    dd  0     ; -(Left_X & 0FFFFh)  : exact left edge -> first pixel
F65536    dd  65536.0
Inv65536  dd  0.0000152587890625
Inv1M     dd  0.00000095367431640625   ; 1/(65536*16): xfrac in units of the per-16px delta
align 1
_DATA	ENDS

_TEXT	SEGMENT BYTE PUBLIC 'CODE'
	ASSUME	cs:_TEXT, ds:_DATA


IFDEF FDS_NGON
; ---------------------------------------------------------------------------------------
; N-gon entry. The left/right edge chains are built in C++ (NGon_BuildChains, FILLERS.CPP)
; and handed over ready to walk, so this skips the 3-vertex sort and the Longest side test.
; Everything below FLSNZ is unchanged: the section machinery was ALREADY index driven --
; Array[Section] is a section's start, Array[Section-1] its end, counting Section down to
; 1 -- so a 9-vertex chain walks on the same code a 3-vertex one did.
;
; NGonChains layout (KEEP IN SYNC with FILLERS.CPP):
;    0  void *Left[10]     bottom-first, top-last
;   40  void *Right[10]
;   80  long  NLeft        section count = vertices-1
;   84  long  NRight
;   88  void *Top
IFDEF FDS_TEXLIN
	PUBLIC	P_TextureLin_32_N_
P_TextureLin_32_N_:
ELSE
	PUBLIC	P_Texture_32_N_
P_Texture_32_N_:
ENDIF
mov [TEBP],ebp
;chains in ebx , bitmap in eax , where in ecx
mov [BitMap],eax
; SMC self-patch of the two in-code texture-base disp32s -- the triangle entry does this
; below, and the n-gon entry jumps straight to FLSNZ, so it MUST do it too. Skipping it
; leaves the PREVIOUS face's texture patched in: wrong texture, varying per face and frame.
IFDEF FDS_TEX_SWIZZLE
mov ds:[Texture2],eax           ; SMC base for the tiled inner-loop fetch
ELSE
mov ds:[Texture1],eax           ; priming fetch base (pre-loop; unreachable under z)
IFDEF FDS_ZBUF
mov ds:[TextureZ],eax           ; z-classic inner-loop fetch (LIN + FDS_ZBUF build)
ELSE
mov ds:[Texture],eax            ; SMC base for the row-major inner-loop fetch
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
	PUBLIC	P_TextureLin_32_
P_TextureLin_32_:
ELSE
	PUBLIC	P_Texture_32_
P_Texture_32_:
ENDIF
mov [TEBP],ebp

;vtx in ebx , bitmap in eax , where in ecx

mov [BitMap],eax
; SMC self-patch of the two in-code texture-base disp32s. Force DS (writable): OW2 wasm's
; ASSUME cs:_TEXT otherwise makes these CS: writes, and CS is execute/read-only under DOS/4GW
; (DS shares CS's base, so this patches the same code location the fetches read).
IFDEF FDS_TEX_SWIZZLE
mov ds:[Texture2],eax           ; SMC base for the tiled inner-loop fetch
ELSE
mov ds:[Texture1],eax           ; priming fetch base (pre-loop; unreachable under z)
IFDEF FDS_ZBUF
mov ds:[TextureZ],eax           ; z-classic inner-loop fetch (LIN + FDS_ZBUF build)
ELSE
mov ds:[Texture],eax            ; SMC base for the row-major inner-loop fetch
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
; fatmap2 S9/S10 convention: spans run ceil(left_x) .. ceil(right_x)-1, HALF-OPEN. Two polygons
; sharing an edge compute the same ceil, so they tile exactly — that is what makes the span
; watertight, and it retires the right-inclusive "inc ebp" overdraw hack below (1 px per span).
; xfrac is the distance from the exact edge UP to that first pixel, so the prestep is POSITIVE.
; (Bolting a prestep onto the old FLOOR start was a mismatched pair — that is why both signs
; measured worse earlier. Start convention and prestep sign have to agree.)
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
inc ebp                 ; right-INCLUSIVE spans: hairline t-junction gaps become 1px of
                        ; overdraw, which the z-buffer resolves (transparent mappers stay
                        ; half-open so seams never double-blend)
ENDIF
jle UpdateValues
mov [XWidth],ebp
IFDEF FDS_ZBUF
mov [LineWidth],ebp
; FDS_ZLINEAR: the encoded linear-Z is computed per-16px checkpoint below, REUSING the
; perspective divide (1/Z) the texture already does -- no standalone z-block, no extra FDIV.
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
; Sub-texel X prestep (fatmap2 S10), now correctly paired with the ceil start above: slide the
; span's start values from the exact fractional edge forward to the first pixel. Deltas here
; are per-16-pixels, so the shift is Delta*xfrac/(65536*16) — no divide, the 1/XWidth is
; already inside Delta. Applied to the HOMOGENEOUS U,V,Z so every 16px checkpoint inherits it;
; the per-scanline Left_* accumulators in memory are untouched.
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
FDIV ST(0),ST(6)                ; ST(0) = Z (=1/Left_Z) at checkpoint 0
IFDEF FDS_ZLINEAR
; enc0 = 0FF80h - ZLinScale*Z, REUSING the perspective divide above (no extra FDIV). eax is
; dead here (Left_X>>16 was consumed by the lea). Integer ops overlap the FMUL/FISTP latency.
FLD ST(0)
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
FLD ST(0)                       ; enc1 = 0FF80h - ZLinScale*Z (reuse divide). eax free here.
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
mov [Xwidth],eax
IFDEF FDS_ZLINEAR
; per-pixel encoded-Z step = (enc1-enc0)/16. sar 4 (not idiv): correct even for the last
; partial sub-span, since enc1 is the +16px checkpoint and drawing w<16 px reaches the right
; enc. edx is dv (live) - sar touches only eax. Re-anchored to enc0 each sub-span (below).
mov eax,[enc1]
sub eax,[enc0]
sar eax,4
mov [ZdPx],eax
ENDIF

comment &

mov [dv],edx
mov [du],esi
mov ebx,0
mov esi,[BitMap]
mov ecx,[u0]
mov edx,[v0]
;sar ecx,8
;sar edx,8
mov   bl, ch
mov   bh, dh

 @@InnerLoop:
    add   edx, [dv]                 ; update v
    add   ecx, [du]                 ; update u
    mov   eax, [esi+ebx*4]          ; get pixel from aligned texture map
    mov   bl, ch
    mov   [edi], eax                ; plot pixel
    mov   bh, dh
    add   edi,4
    dec   ebp
    jnz   @@InnerLoop

    DELTA U - ESI
    DELTA V - EDX
	  U - [u0]
	  V - [v0]
    Texture - [Bitmap]
    Counter - EBP
    VPage   - EDI

&

comment &

Registers| 31 - 24 | 23 - 16 | 15 - 8  |  7 - 0  |
--------------------------------------------------
EAX      | frac DU |000000000| int DV  | frac DV |
--------------------------------------------------
EBX      | frac U  |000000000| int V   | frac V  |
--------------------------------------------------
ECX      |         |         |         | int U   |
--------------------------------------------------
EDX      |         |	     |         | int DU  |
--------------------------------------------------
ESI      | Counter				 |
--------------------------------------------------
EDI 	 | Textuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuure |
--------------------------------------------------
EBP      | Holds Pixel                           |
--------------------------------------------------
&
IFDEF FDS_TEX_SWIZZLE
; FATMAP2-style interleaved accumulators for the 8-wide-strip tiled texture.
; Bit layouts (w=whole, f=frac, g=gap; gaps are 0 in the accumulator, 1 in the delta so a
; field's carry ripples through its gap into the next field; the loop ANDs clear them back):
;   U: wwwww gggggggg www g fffffffffffffff   hi5@[27:31]  lo3@[16:18]  frac15@[0:14]
;   V: ggggg wwwwwwww gggg  fffffffffffffff   int8@[19:26]              frac15@[0:14]
; (Uacc+Vacc)>>16 = ((U>>3)<<11)|(V<<3)|(U&7) == the tiled index built at load time.
; Inputs: esi = du (8.8), edx = dv (8.8), [u0]/[v0] = span start (8.8). frac15 = frac8<<7,
; so stepping granularity is identical to the row-major 8-bit-frac path.
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
	or  eax,07F88000h         ; gap+guard bits = 1
	mov [TilDuDx],eax
	mov eax,edx
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
	mov ebx,[v0]
	mov eax,ebx
	and ebx,0FF00h
	shl ebx,11
	and eax,0FFh
	shl eax,7
	or  ebx,eax               ; ebx = interleaved V accumulator (gaps 0)
IFDEF FDS_ZBUF
IFDEF FDS_ZLINEAR
	mov eax,[enc0]           ; linear-Z: RE-ANCHOR to this 16px sub-span's start checkpoint enc
ELSE
	mov eax,[zcAcc]           ; hyperbolic: chained 1/w code
ENDIF
	mov edx,edi               ; edi = span START address here (the counter transform that
	                          ; makes esi negative runs AFTER this block - esi is still du!)
	sub edx,[Where]           ; -> pixel byte offset (px*4)
	shr edx,1                 ; -> px*2
	add edx,[_ZBufPtr]        ; edx = &z16[first pixel of span]
ELSE
	mov eax,[TilDuDx]         ; both deltas live in registers across the span
	mov edx,[TilDvDx]
ENDIF
ELSE
; Create EAX
mov eax,esi
shl eax,24
mov ax,dx
; Create EBX
mov ebx,[u0]
shl ebx,24
mov ecx,[v0]
mov bx,cx
; Create ECX
mov ecx,[u0]
shr ecx,8
; Create EDX
mov edx,esi
shr edx,8
IFDEF FDS_ZBUF
; LIN + z: count to memory, esi (du was consumed above) = &z16[span start]; edi stays
; at the span start and walks forward - the shared advance below is for the no-z loop.
mov [zcounter],ebp
mov esi,edi
sub esi,[Where]
shr esi,1
add esi,[_ZBufPtr]
and ecx,0ffffh
mov ch,bh
jmp @@ZClassic
ENDIF
ENDIF
; Create ESI
mov esi,ebp
shl esi,2
add edi,esi
xor esi,-1
inc esi

; Create EDI

IFDEF FDS_TEX_SWIZZLE
	lea ebp,[ecx+ebx]         ; prime pixel 0's combined index
ELSE
and ecx,0ffffh

mov ch,bh

	db 08Bh,02Ch,08Dh ; |
	Texture1 dd ?      ; | <-- mov ebp,[TEXTURE+ecx*4]
ENDIF

@@InnerLoop:
;	add ebx,eax
;	db 08Bh,02Ch,08Dh ; |
;	Texture dd ?      ; | <-- mov ebp,[TEXTURE+ecx*4]
;	adc cl,dl
;	mov [edi+esi],ebp
;	mov ch,bh
;	add esi,4

IFDEF FDS_TEX_SWIZZLE
IFDEF FDS_ZBUF
	; tiled fetch + 16-bit z (test-first: occluded pixels skip fetch+store entirely).
	; FIRST-WINS: skip on equal codes (jbe) + unbiased write. Faces arrive front-to-back,
	; so on a 16-bit code tie the NEARER face already drew; draw-on-equal let far faces
	; (whole clusters collapse to one code past z~1000) punch through nearer ones =
	; reverse painter's. A z-skip can never leave a hole: the tying face's pixel is there.
	cmp ax,[edx]              ; z test
	jbe @@ZSkip
	mov [edx],ax              ; z write (unbiased)
	shr ebp,16                ; tiled texel index
	add ecx,[TilDuDx]         ; u += du (interleaved, ripple)
	add ebx,[TilDvDx]         ; v += dv
	and ecx,0F8077FFFh        ; clear u gaps
	db 08Bh,02Ch,0ADh         ; mov ebp,[ebp*4 + Texture2]
	Texture2 dd ?
	and ebx,07F87FFFh         ; clear v gaps
	mov [edi+esi],ebp         ; plot
	lea ebp,[ecx+ebx]
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
	add eax,[ZdPx]
	add edx,2
	add esi,4
	jnz @@InnerLoop
@@SpanDone:
	mov [zcAcc],eax           ; carry the depth code into the next span
ELSE
	; tiled fetch, interleaved-accumulator form (FATMAP2): ecx=U acc, ebx=V acc,
	; eax/edx = deltas with gap bits set (carry ripples across fields), ebp = index/texel.
	; 2 adds + 2 ands + shr + lea per pixel, no scratch memory, pairs U/V on P55C.
	shr ebp,16                ; tiled texel index
	add ecx,eax               ; u += du (ripple through gaps)
	add ebx,edx               ; v += dv
	and ecx,0F8077FFFh        ; clear u gaps (keep hi5|lo3|frac)
	db 08Bh,02Ch,0ADh         ; mov ebp,[ebp*4 + Texture2]
	Texture2 dd ?
	and ebx,07F87FFFh         ; clear v gaps (keep int8|frac)
	mov [edi+esi],ebp         ; plot
	lea ebp,[ecx+ebx]         ; next pixel's combined index
	add esi,4
	jnz @@InnerLoop
ENDIF
ELSE
IFDEF FDS_ZBUF
	; row-major + z (dynamic textures): de-pipelined - fetch and plot the same pixel,
	; first-wins z like the tiled loop. Without this the LIN mapper drew depthless inside
	; the front-to-back walk and every farther face punched straight through it.
@@ZClassic:
	mov ebp,[zcAcc]
	cmp bp,[esi]              ; 16-bit z test
	jbe @@ZCSkip              ; first-wins: equal codes keep the earlier (nearer) face
	mov [esi],bp              ; z write (unbiased)
	db 08Bh,02Ch,08Dh         ; |
	TextureZ dd ?             ; | <-- mov ebp,[TEXTURE+ecx*4]
	mov [edi],ebp             ; plot
@@ZCSkip:
	mov ebp,[ZdPx]
	add [zcAcc],ebp           ; z step (memory: chains across sub-spans)
	add ebx,eax               ; v/u-frac step (CF feeds the adc below)
	mov ch,bh
	adc cl,dl
	add esi,2                 ; z pointer
	add edi,4
	dec dword ptr [zcounter]
	jnz @@ZClassic
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
ENDIF

mov eax,[u1]
mov ecx,[v1]
mov [u0],eax
mov [v0],ecx
IFDEF FDS_ZLINEAR
; pipeline the encoded Z exactly like u0/u1: shift enc0=enc1 (checkpoint N is the new sub-span
; start), then compute the new enc1 from Z at checkpoint N+1 -- REUSING the InnerOuterLoop
; divide already in ST(0) (no extra FDIV). eax reloaded by [XWidth] below.
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
; scanline early — the vertical half of the texture crawl.
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
; scanline early — the vertical half of the texture crawl.
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