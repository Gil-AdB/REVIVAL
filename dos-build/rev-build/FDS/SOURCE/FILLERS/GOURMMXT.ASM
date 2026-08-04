.586p
.mmx
_DATA   SEGMENT PARA PUBLIC 'DATA'
align 16
MMXTemp			dq  0
Left_Section            dd 0
Right_Section           dd 0
Left_Section_Height     dd 0
Right_Section_Height    dd 0
Left_R              dd 0.0 ; float
Delta_left_R            dd 0.0 ; float
Left_G              dd 0.0 ; float
Delta_left_G            dd 0.0 ; float
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
R                       dd 0
G                       dd 0
B                       dd 0
dr              dd 0
tb      db 4 dup(0)
dg                      dd 0
dbb                     dd 0
XWidth          dd 0
Height          dd 0
Longest         dd 0
; FDS_NGON: chains hold up to 9 vertices. Left_Array and RIght_Array MUST stay adjacent
; and equally sized - the n-gon prologue copies both in one run.
Left_Array              dd 10 dup(0)
RIght_Array             dd 10 dup(0)
Left_X          dd 0
Delta_Left_X        dd 0
Right_X         dd 0
Delta_Right_X       dd 0
Where                   dd 0
Top                     dd 0
Middle                  dd 0
Bottom                  dd 0
ScanLine        dd 0
TEBP            dd 0
Vertex_X  equ 0
Vertex_Y  equ 4
Vertex_R  equ 16
Vertex_G  equ 12
Vertex_B  equ 8
One       dd  1.0
Sixteen   dd  1.0
Sixtyfive dd  256.0
; ---- FDS_SUBTEX: vertex X,Y arrive as 16.16 instead of snapped integers ----
; This filler is pass 2 of the transparent textured-Gouraud pair (FILLERS.CPP The_BiGTTrue):
; pass 1 lays the texture down, this one MODULATES it (pmullw in the inner loop). So the two
; passes must cover EXACTLY the same pixels — any column the texture pass paints and this one
; misses keeps its raw, unmodulated texel and shows up as a full-brightness seam. That is what
; the bright diagonal across the city water was: pass 1 on ceil/16.16, pass 2 on floor/integer.
PreY      dd  0     ; (ceil(Y1)<<16) - Y1 : vertex -> first scanline, in [0,65536)
CeilTop   dd  0     ; ceil(Y_top) — shared by both edges' first section and by FLSNZ
PreYTop   dd  0
F65536    dd  65536.0
Inv65536  dd  0.0000152587890625
align 1
_DATA   ENDS

_TEXT   SEGMENT BYTE PUBLIC 'CODE'
	ASSUME  cs:_TEXT, ds:_DATA


IFDEF FDS_NGON
; ---------------------------------------------------------------------------------------
; N-gon entry. Chains are built in C++ (NGon_BuildChains, FILLERS.CPP), which documents the
; struct read here:  0 Left[10] | 40 Right[10] | 80 NLeft | 84 NRight | 88 Top.
; No texture, so unlike BITRUE/TEXTGOUR there is no SMC base to re-patch here.
	PUBLIC  RGBTGouraudMMX_N_
RGBTGouraudMMX_N_:
mov [TEBP],ebp
;chains in ebx , where in ecx
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

	PUBLIC  RGBTGouraudMMX_
RGBTGouraudMMX_:
mov [TEBP],ebp

;vtx in ebx , where in ecx

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
; and FLSNZ needs the same ceil for the scanline address. (The MIDDLE vertex cannot be
; shared: the edge bends there, so its section needs its own prestep.)
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
mov esi,[Right_X]
IFDEF FDS_SUBTEX
; fatmap2 S9: spans run ceil(left_x) .. ceil(right_x)-1, HALF-OPEN — the SAME rule the
; texture pass uses, which is the whole point (see the header note). No sub-texel X prestep
; on R,G,B: a sub-pixel shift of a smooth light ramp is invisible, and it would cost three
; FPU ops per span.
add eax,0FFFFh
add esi,0FFFFh
ENDIF
sar eax,16
mov edi,[ScanLine]
sar esi,16
sub esi,eax
jle UpdateValues
FLD dword ptr [Sixteen]
mov [XWidth],esi
FILD dword ptr [XWidth]
FDIV                ;ST(6) - 1.0/Width
FLD dword ptr [SixtyFive]
FMUL
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
FFREE ST(6)
FISTP dword ptr [dbb]
FISTP dword ptr [dg]
FISTP dword ptr [dr]
lea edi,[edi+eax*4]
;FLD One
;FDIV ST(0),ST(6)
;mov cl,[Edi]
FLD dword ptr [SixtyFive]
FLD ST(0)
FMUL ST(0),ST(4)
FISTP dword ptr [R]
FLD ST(0)
FMUL ST(0),ST(3)
FISTP dword ptr [G]
FMUL ST(0),ST(1)
FISTP dword ptr [B]

;Left R   ST(5)
;Left G   ST(4)
;Left B   ST(3)
;Delta R  ST(2)
;Delta G  ST(1)
;Delta B  ST(0)
comment &
FXCH ST(3)  ;ST(0) = B    ST(3) = Delta B
FADD ST(0),ST(3)
FXCH ST(1)  ;ST(0) = Delta G    ST(1) = B
FXCH ST(4)  ;ST(0) = G    ST(4) = Delta G
FADD ST(0),ST(4)
FXCH ST(2)  ;ST(0) = Delta R    ST(2) = G
FXCH ST(5)  ;ST(0) = R    ST(5) = Delta R
FADD ST(0),ST(5)

;Delta R  ST(5)
;Delta G  ST(4)
;Delta B  ST(3)
;Left R   ST(2)
;Left G   ST(1)
;Left B   ST(0)
&

InnerOuterLoop:

;Delta Z  ST(5)
;Delta U  ST(4)
;Delta V  ST(3)
;Left U   ST(2)
;Left V   ST(1)
;Left Z   ST(0)

comment &
Registers| 31 - 24 | 23 - 16 | 15 - 8  |  7 - 0  |
--------------------------------------------------
EAX      | frac R  |000000000| int G   | frac G  |
--------------------------------------------------
EBX      | frac DR |000000000| int DG  | frac DG |
--------------------------------------------------
ECX      |000000000| int B   | frac B  | int R   |
--------------------------------------------------
EDX      |000000000|000000000|000000000|000000000|
--------------------------------------------------
EDI      | C0000uuuuuuuuuuuuuuuuuuuuuuuuuuuunter |
--------------------------------------------------
ESI      |000000000| int DB  | frac DB | int DR  |
--------------------------------------------------
EBP      | Screeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeen |
--------------------------------------------------
&
; The preperations are made for 8.8 fixed point!!! (B is 8.7 - lines longer than 128 may be a little scrambled)

;mov eax,[dr]
;mov ebx,[dg]
;mov ecx,[dbb]
;sub [r],eax
;sub [g],ebx
;sub [b],ecx

lea ebp,[edi+esi*4]
mov edi,esi
shl edi,2
xor edi,-1
inc edi


emms

mov eax,[r]
mov word ptr [MMXTemp],ax
mov eax,[g]
mov word ptr [MMXTemp+2],ax
mov eax,[b]
mov word ptr [MMXTemp+4],ax
movq mm0,[MMXTemp]

mov eax,[dr]
mov word ptr [MMXTemp],ax
mov eax,[dg]
mov word ptr [MMXTemp+2],ax
mov eax,[dbb]
mov word ptr [MMXTemp+4],ax
movq mm1,[MMXTemp]

@@InnerLoop:    ;Diffuse MMX R/G/B Gouraud Inner Loop
movq mm3,mm0
punpcklbw mm2,[edi+ebp]
psrlw mm3,8
psrlw mm2,8
pmullw mm2,mm3
psrlw mm2,7
packuswb mm2,mm2
movd [edi+ebp],mm2
paddw mm0,mm1
add edi,4
jnz @@InnerLoop

;FFREE ST(0)
;FFREE ST(1)
;FFREE ST(2)
emms
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
; anywhere. Step past it.
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
; Section covers scanlines ceil(Y1)..ceil(Y2)-1, slopes come from the TRUE fractional
; height, and X plus the light ramp are prestepped from the vertex to the first scanline.
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
; A section thinner than a scanline makes dX<<16/dY overflow idiv, but it covers exactly
; one scanline and UpdateValues moves on before the slope is ever applied - so only the
; prestep matters there, and that one IS bounded (PreY < dY). One divide either way.
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
idiv dword ptr [Height]          ; per-scanline dX, 16.16
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
FLD dword ptr [esi+Vertex_R]
FSUB dword ptr [ecx+Vertex_R]
FMUL ST(0),ST(2)
FST dword ptr [Delta_Left_R]
FMUL ST(0),ST(1)
FADD dword ptr [ecx+Vertex_R]
FSTP dword ptr [Left_R]
FLD dword ptr [esi+Vertex_G]
FSUB dword ptr [ecx+Vertex_G]
FMUL ST(0),ST(2)
FST dword ptr [Delta_Left_G]
FMUL ST(0),ST(1)
FADD dword ptr [ecx+Vertex_G]
FSTP dword ptr [Left_G]
FLD dword ptr [esi+Vertex_B]
FSUB dword ptr [ecx+Vertex_B]
FMUL ST(0),ST(2)
FST dword ptr [Delta_Left_B]
FMUL ST(0),ST(1)
FADD dword ptr [ecx+Vertex_B]
FSTP dword ptr [Left_B]
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
FLD dword ptr [esi+Vertex_R]
mov [Delta_Left_X],eax
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
mov ebx,[esi+Vertex_Y]
sub ebx,[ecx+Vertex_Y]
mov [Height],ebx
jle @@CRSzero
mov eax,[esi+Vertex_Y]
add eax,0FFFFh
sar eax,16                       ; ceil(Y2)
cmp ecx,[Top]
jne @@NoTopR
mov edx,[CeilTop]
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
mov ebx,eax
jle @@CRSzero
sal edx,16
sub edx,[ecx+Vertex_Y]
mov [PreY],edx
@@GotPreR:
FILD dword ptr [Height]
FLD dword ptr [F65536]
FDIVR
FILD dword ptr [PreY]
FMUL dword ptr [Inv65536]
cmp ebx,1
jne @@MultiR
xor eax,eax
mov [Delta_Right_X],eax
mov eax,[esi+Vertex_X]
sub eax,[ecx+Vertex_X]
imul dword ptr [PreY]
idiv dword ptr [Height]
add eax,[ecx+Vertex_X]
mov [Right_X],eax
jmp @@XDoneR
@@MultiR:
mov eax,[esi+Vertex_X]
sub eax,[ecx+Vertex_X]
mov edx,eax
sar edx,31
shld edx,eax,16
sal eax,16
idiv dword ptr [Height]
cmp edx,0
jge @@Flr2
dec eax
@@Flr2:
mov [Delta_Right_X],eax
imul dword ptr [PreY]
shrd eax,edx,16
add eax,[ecx+Vertex_X]
mov [Right_X],eax
@@XDoneR:
FLD dword ptr [esi+Vertex_R]
FSUB dword ptr [ecx+Vertex_R]
FMUL ST(0),ST(2)
FST dword ptr [Delta_Right_R]
FMUL ST(0),ST(1)
FADD dword ptr [ecx+Vertex_R]
FSTP dword ptr [Right_R]
FLD dword ptr [esi+Vertex_G]
FSUB dword ptr [ecx+Vertex_G]
FMUL ST(0),ST(2)
FST dword ptr [Delta_Right_G]
FMUL ST(0),ST(1)
FADD dword ptr [ecx+Vertex_G]
FSTP dword ptr [Right_G]
FLD dword ptr [esi+Vertex_B]
FSUB dword ptr [ecx+Vertex_B]
FMUL ST(0),ST(2)
FST dword ptr [Delta_Right_B]
FMUL ST(0),ST(1)
FADD dword ptr [ecx+Vertex_B]
FSTP dword ptr [Right_B]
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
FLD dword ptr [esi+Vertex_R]
mov [Delta_Right_X],eax
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


_TEXT   ENDS

END
