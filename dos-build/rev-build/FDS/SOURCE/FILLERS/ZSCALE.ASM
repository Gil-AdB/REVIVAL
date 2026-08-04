.386p
_DATA   SEGMENT DWORD PUBLIC 'DATA'
align 4
du          dd  0
StartX          dd  0
StartY          dd  0
EndX            dd  0
EndY            dd  0
WidthX          dd  0
Height          dd  0
ddx         dd  0
ddy         dd  0
Texture         dd  0
Screen          dd  0
x           dd  0
y           dd  0
tx          dd  0
ty          dd  0
ScanLine        dd  0
Counter         dd  0
TEBP            dd  0
align 1
_DATA   ENDS

_TEXT   SEGMENT BYTE PUBLIC 'CODE'
	ASSUME  cs:_TEXT, ds:_DATA


	PUBLIC  ZScale_thatfucksBTB_
ZScale_thatfucksBTB_:
mov [TEBP],ebp
;eax = x ebx = y ecx = size x edx = size y esi = Texture edi = Screen Buffer
cmp ecx,0
jle ScalerEnd
cmp edx,0
jle ScalerEnd
mov [Texture],esi
mov [Screen],edi
mov esi,eax
mov edi,ebx
sub esi,ecx
sub edi,edx
mov [StartX],esi
mov [StartY],edi
add eax,ecx
add ebx,edx
mov [EndX],eax
mov [EndY],ebx
sub eax,esi
sub ebx,edi
mov [WidthX],eax
mov [Height],ebx
mov ecx,eax
mov ebp,ebx
mov eax,10000000h
mov edx,0
div ecx
mov [ddx],eax
mov eax,10000000h
mov edx,0
div ebp
mov [ddy],eax
mov eax,[StartX]
mov ebx,[StartY]
cmp eax,0
jge @@StartXOverZero
mov ebp,[WidthX]
mov eax,0
mov ecx,[StartX]
mov [x],0
add ebp,ecx
sub eax,ecx
mov [WidthX],ebp
mov ecx,[ddx]
mul ecx
mov [tx],eax
jmp @@CheckY
@@StartXOverZero:
mov [x],eax
mov [tx],0
@@CheckY:
cmp ebx,0
jge @@StartYOverZero
mov ebp,[Height]
mov eax,0
mov ecx,[StartY]
mov [y],0
add ebp,ecx
sub eax,ecx
mov [Height],ebp
mov ecx,[ddy]
mul ecx
mov [ty],eax
jmp @@CheckEndX
@@StartYOverZero:
mov [y],ebx
mov [ty],0
@@CheckEndX:
mov eax,[EndX]
mov ebx,[EndY]
cmp eax,320
jl @@CheckEndY
mov eax,320
sub eax,[x]
mov [WidthX],eax
cmp eax,0
jle ScalerEnd
@@CheckEndY:

cmp ebx,200
jl @@Setup
mov eax,200
sub eax,[y]
mov [Height],eax
@@Setup:
cmp [WidthX],0
jle ScalerEnd
cmp [Height],0
jle ScalerEnd

mov eax,[y]
mov ebx,[x]
shl ebx,2
shl eax,8
add ebx,eax
shl eax,2
add ebx,eax
add ebx,[Screen]
mov [ScanLine],ebx
@@Outer:
mov eax,[ty]
mov ebx,[ddx]
mov edi,[ScanLine]
ror ebx,20
and eax,0ff00000h
mov cl,bl
shr eax,12
mov bl,0
mov edx,[tx]
mov [du],ebx
ror edx,20
mov esi,[WidthX]
;add eax,[Texture]
shl esi,2
mov ebp,[Texture]
mov al,dl
lea edi,[edi+esi]
mov dl,cl
xor esi,-1

inc esi
@@inner:
  mov ecx,[ebp+eax*4]
  or ecx,ecx
  jz @@FuckBTB
  mov [edi+esi],ecx
  @@FuckBTB:
  add edx,[du]
  adc al,dl
  add esi,4
jnz @@inner

mov eax,[ScanLine]
mov ebx,[ty]
add eax,1280
mov ecx,[ddy]
mov edx,[height]
add ebx,ecx
dec edx
mov [ty],ebx
mov [ScanLine],eax
mov [height],edx
jnz @@Outer

ScalerEnd:
mov ebp,[TEBP]
ret
_TEXT   ENDS

END
