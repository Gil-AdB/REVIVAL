.386p

_DATA	SEGMENT DWORD PUBLIC 'DATA'
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
Vertex_R  equ 20
Vertex_G  equ 24
Vertex_B  equ 28
OneTwo    dd  0.0625
One       dd  1.0
Sixteen   dd  16.0
Sixtyfive dd  256.0
D1				dd  0
D2				dd  0
D3				dd  0
Teax      dd  0
Tebx      dd  0
TempFB		db	0
align 1
_DATA	ENDS

_TEXT	SEGMENT BYTE PUBLIC 'CODE'
	ASSUME	cs:_TEXT, ds:_DATA


	PUBLIC PG_Texture_32_
PG_Texture_32_:

mov [TEBP],ebp

;vtx in ebx , bitmap in eax , where in ecx

mov [BitMap],eax
mov [Texture],eax
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
mov eax,_Xres
mul edi
mov edi,eax
add edi,[Where]
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
mov [XWidth],ebp

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
FLD One
FDIV ST(0),ST(6)
;mov cl,[Edi]
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
mov cl,[ecx+ebx*4]

FMUL dword ptr [SixtyFive]
FLD ST(0)
FMUL ST(0),ST(4)
FISTP dword ptr [u1]
FMUL ST(0),ST(2)
FISTP dword ptr [v1]


mov bl,byte ptr [DR]
shl ebx,24
mov bx,word ptr [DG]
mov [D3],ebx

mov ebx,[R]
shl ebx,24
mov bx,word ptr [G]
mov [Tebx],ebx

mov eax,[B]
shl eax,8
mov al,byte ptr [R+1]
;and eax,0ffffffh
mov [Teax],eax

; Prepare D2
mov ecx,[DBB]
shl ecx,8
mov cl,byte ptr [DR+1]
;and ecx,0ffffffh
mov [D2],ecx

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

sub [u0],esi
sub [v0],edx
mov ebx,[Tebx]
mov eax,[Teax]
rol eax,8
mov al,byte ptr [u0]
ror eax,8

mov ecx,esi
mov byte ptr [D2+3],cl

mov ecx,0
mov cl,dl
shl ecx,24
mov cl,1
mov [D1],ecx

mov ecx,0
mov cl,byte ptr [u0+1]
mov ch,byte ptr [v0+1]

and esi,0ff00h
shr esi,8
and edx,0ff00h
or  edx,esi

xor ebp,-1
add ebp,256
shl [v0],24
or  ebp,[v0]

sub edi,4

@@InnerLoop:
	add edi,4                ; 1
	add ebx,[D3]             ; 1-2
	adc eax,[D2]             ; 3-4
	db 8Bh, 34h, 8Dh
	Texture dd 0
;	mov esi,[ecx*4+Texture]  ; 4
	adc cl,dl                ; 5
	add ebp,[D1]             ; 5-6
	adc ch,dh                ; 7
	mov [TempFB],ah          ; 7
	shr esi,1                ; 8
	mov ah,bh                ; 8
	add esi,eax              ; 9
	mov ah,[TempFB]          ; 9
	test ebp,0ffh            ; 10
	mov [edi],esi            ; 10
	jnz @@InnerLoop          ; 11
comment &
@@InnerLoop:
	add ebx,[D3]
	adc eax,[D2]
	adc cl,dl
	add ebp,[D1]
	adc ch,dh                   ; Texture ready
	mov [TempFB],ah
	mov ah,bh                   ; Gouraud ready
	db 8Bh, 34h, 8Dh
	Texture dd 0
;	mov esi,[Texture+ecx*4]
	shr esi,1
	add esi,eax
	mov [edi],esi
	add edi,4
	mov ah,[TempFB]
	test ebp,0ffh
	jnz @@InnerLoop
&


mov [Teax],eax
mov [Tebx],ebx
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
add eax,_XRes
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
@@CRSend:
mov [Right_Section_Height],ebx
  ret
CalcRightSection endp


_TEXT	ENDS

END