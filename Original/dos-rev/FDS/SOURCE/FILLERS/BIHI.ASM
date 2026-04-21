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
Left_Array              dd 3 dup(0)
RIght_Array             dd 3 dup(0)
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
align 1
_DATA	ENDS

_TEXT	SEGMENT BYTE PUBLIC 'CODE'
	ASSUME	cs:_TEXT, ds:_DATA


	PUBLIC	P_Texture_16_
P_Texture_16_:
mov [TEBP],ebp

;vtx in ebx , bitmap in eax , where in ecx

mov [BitMap],eax
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
jz TextureEnd
jge LongestOverZero


mov [Top],ecx
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

mov edi,[ebp+vertex_y]
mov edx,edi
sal edi,7
sal edx,9
add edi,[Where]
add edi,edx
mov [ScanLine],edi

OuterLoop:
;Inner Loop Start Here
mov eax,[Left_X]
mov ebp,[Right_X]
sar eax,16
mov edi,[ScanLine]
sar ebp,16
sub ebp,eax
jle UpdateValues
FLD dword ptr [Sixteen]
mov [XWidth],ebp
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
lea edi,[edi+eax*2]
FLD One
FDIV ST(0),ST(6)
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
FDIV ST(0),ST(1)
mov cl,[ecx+ebx*2]

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
mov ebx,0
mov esi,[BitMap]
mov [Xwidth],eax
mov ecx,[u0]
mov edx,[v0]
;sar ecx,8
;sar edx,8
mov   bl, ch
mov   bh, dh



 @@InnerLoop:
    add   edx, [dv]                 ; update v
    add   ecx, [du]                 ; update u
    mov   ax, [esi+ebx*2]                 ; get pixel from aligned texture map
    mov   bl, ch
    mov   [edi], ax                 ; plot pixel
    mov   bh, dh
    add   edi,2
    dec   ebp
    jnz   @@InnerLoop

mov eax,[u1]
mov ecx,[v1]
mov [u0],eax
mov [v0],ecx

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
add eax,640
dec ebx
mov [ScanLine],eax
mov [Left_Section_Height],ebx
jg @@Update_Left_UVXZ
mov ebx,[Left_Section]
dec ebx
jle TextureEnd
mov [Left_Section],ebx
call CalcLeftSection
cmp ebx,0
jle TextureEnd
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
mov ebx,[Right_Section]
dec ebx
jle TextureEnd
mov [Right_Section],ebx
call CalcRightSection
cmp ebx,0
jle TextureEnd
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
@@CLSend:
mov [Left_Section_Height],ebx
  ret
CalcLeftSection endp



CalcRightSection proc near

mov eax,[Right_Section]
mov ecx,[Right_Array+eax*4]
sub eax,1
mov esi,[Right_Array+eax*4]
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
@@CRSend:
mov [Right_Section_Height],ebx
  ret
CalcRightSection endp


_TEXT	ENDS

END