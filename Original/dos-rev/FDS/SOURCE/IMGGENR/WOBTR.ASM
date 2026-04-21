.386p
_DATA	SEGMENT DWORD PUBLIC 'DATA'
align 4
ScanLine								dd  0
Delta_Left_U            dd  0
Delta_Left_V            dd  0
Delta_Right_U           dd  0
Delta_Right_V           dd  0
Left_U									dd  0
Left_V                  dd  0
Right_U                 dd  0
Right_V                 dd  0
du                      dd  0
dv                      dd  0
lu                      dd  0
lv                      dd  0
PointsList              dd  0
Texture		        			dd  0
x                       dd  0
y								        dd  0
Tebp       							dd  0
t 						      		dd  0
counter									dd  0
align 1
ScreenWidth							equ 320
ScreenHeight						equ 240
PointsWidth							equ ScreenWidth/8
EXTRN _WobPointsHeight  : DWORD ;equ ScreenHeight/8
u												equ 0
v												equ 4
ElementSize             equ 16
_DATA	ENDS

_TEXT	SEGMENT BYTE PUBLIC 'CODE'
	ASSUME	cs:_TEXT, ds:_DATA


	PUBLIC	Grid_Texture_MapASM_
Grid_Texture_MapASM_:
; On Entry : ecx - texture ; esi = pointer to points array which contains : {signed long u,v}; edi - Screen Pointer
mov [Tebp],ebp
mov [ScanLine],edi
mov [Texture],ecx
mov [PointsList],esi
mov eax,[_WobPointsHeight]
mov [x],PointsWidth
mov [y],eax

@@DrawLoop:
mov eax,[esi+u]
mov ebx,[esi+v]
mov [Left_U],eax
mov [Left_V],ebx
mov ecx,[esi+ElementSize+ElementSize*PointsWidth+u] ; yes, yes , i know it's looks a LITTLE too much , but then , it's concludes in one add in the end ;)
mov edx,[esi+ElementSize+ElementSize*PointsWidth+v]
sub ecx,eax
sub edx,ebx
sar ecx,3
mov eax,[esi+ElementSize+u]
sar edx,3
mov [Delta_Left_U],ecx
mov [Delta_Left_V],edx
mov ebx,[esi+ElementSize+v]
mov [Right_U],eax
mov [Right_V],ebx
mov ecx,[esi+ElementSize*2+ElementSize*PointsWidth+u]
mov edx,[esi+ElementSize*2+ElementSize*PointsWidth+v]
sub ecx,eax
sub edx,ebx
sar ecx,3
add esi,ElementSize
sar edx,3
mov [PointsList],esi
mov [Delta_Right_U],ecx
mov [Delta_Right_V],edx
mov edi,[ScanLine]
mov [counter],8
@@Inner8:
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
mov cl,byte ptr [dv] ; chk this
mov ch,byte ptr [dv+1] ; chk this
mov ebp,eax
mov esi,[Texture]


    mov   eax, [esi+edx*4]
    mov   dh,0
    add   ebx, ecx
    mov   [edi],eax
    adc   edx, ebp
    mov   dh, bh

    mov   eax, [esi+edx*4]
    mov   dh,0
    add   ebx, ecx
    mov   [edi+4],eax
    adc   edx, ebp
    mov   dh, bh

    mov   eax, [esi+edx*4]
    mov   dh,0
    add   ebx, ecx
    mov   [edi+8],eax
    adc   edx, ebp
    mov   dh, bh

    mov   eax, [esi+edx*4]
    mov   dh,0
    add   ebx, ecx
    mov   [edi+12], eax
    adc   edx, ebp
    mov   dh, bh

    mov   eax, [esi+edx*4]
    mov   dh,0
    add   ebx, ecx
    mov   [edi+16], eax
    adc   edx, ebp
    mov   dh, bh

    mov   eax, [esi+edx*4]
    mov   dh,0
    add   ebx, ecx
    mov   [edi+20], eax
    adc   edx, ebp
    mov   dh, bh


    mov   eax, [esi+edx*4]
    mov   dh,0
    add   ebx, ecx
    mov   [edi+24], eax
    adc   edx, ebp
    mov   dh, bh

    mov   eax, [esi+edx*4]
    mov   [edi+28], eax


add edi,1280
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
mov esi,[counter]
dec esi
mov [counter],esi
jnz @@Inner8
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
mov ebp,[Tebp]
ret
_TEXT	ENDS

END
