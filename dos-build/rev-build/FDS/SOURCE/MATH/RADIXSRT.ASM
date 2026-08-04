.386p
_DATA	SEGMENT DWORD PUBLIC 'DATA'
align 4
Bytes1  	      	dd 256 dup(0)
align 4
Bytes2  	      	dd 256 dup(0)
align 4
Bytes3  	      	dd 256 dup(0)
align 4
Bytes4  	      	dd 256 dup(0)
FList		      	dd 0
SList		      	dd 0
NumOfFaces   		dd 0
LDZ		      	equ 0
align 1
_DATA	ENDS

_TEXT	SEGMENT BYTE PUBLIC 'CODE'
	ASSUME	cs:_TEXT, ds:_DATA


	PUBLIC	Radix_Asm_
Radix_Asm_:
; on entry: esi - FList  edx - SList ebx - NumOfFaces
mov [FList],esi
mov [SList],edx
mov [NumOfFaces],ebx

mov eax,0
mov ecx,1024
mov edi,offset Bytes1
rep stosd

mov esi,[FList]
mov ecx,[NumOfFaces]

mov edx,0

@@FirstLoop:
mov eax,[esi]
add esi,4
mov ebx,[eax]
mov dl,bl
inc dword ptr [Bytes1+edx*4+4]
mov dl,bh
inc dword ptr [Bytes2+edx*4+4]
ror ebx,16
mov dl,bl
inc dword ptr [Bytes3+edx*4+4]
mov dl,bh
inc dword ptr [Bytes4+edx*4+4]
dec ecx
jnz @@FirstLoop

mov esi,-1020
@@SecondLoop:
mov eax,[Bytes1+esi+1024]
mov ebx,[Bytes2+esi+1024]
mov ecx,[Bytes1+esi+1020]
mov edx,[Bytes2+esi+1020]
add eax,ecx
add ebx,edx
mov [Bytes1+esi+1024],eax
mov [Bytes2+esi+1024],ebx
mov eax,[Bytes3+esi+1024]
mov ebx,[Bytes4+esi+1024]
mov ecx,[Bytes3+esi+1020]
mov edx,[Bytes4+esi+1020]
add eax,ecx
add ebx,edx
mov [Bytes3+esi+1024],eax
mov [Bytes4+esi+1024],ebx
add esi,4
jnz @@SecondLoop

mov edx,0
mov ecx,[NumOfFaces]
mov esi,[FList]
mov edi,[SList]
mov eax,[esi]
mov ebx,[eax]
mov dl,bl
dec ecx
@@ThirdLoop:
mov ebp,[Bytes1+edx*4]
inc ebp
add esi,4
mov [Bytes1+edx*4],ebp
mov [edi+ebp*4-4],eax
mov eax,[esi]
mov ebx,[eax]
mov dl,bl
dec ecx
jnz @@ThirdLoop


mov ecx,[NumOfFaces]
mov edi,[FList]
mov esi,[SList]
mov eax,[esi]
mov ebx,[eax]
mov dl,bh
dec ecx
@@FourthLoop:
mov ebp,[Bytes2+edx*4]
inc ebp
add esi,4
mov [Bytes2+edx*4],ebp
mov [edi+ebp*4-4],eax
mov eax,[esi]
mov ebx,[eax]
mov dl,bh
dec ecx
jnz @@FourthLoop


mov ecx,[NumOfFaces]
mov esi,[FList]
mov edi,[SList]
mov eax,[esi]
mov ebx,[eax]
ror ebx,16
mov dl,bl
dec ecx
@@FifthLoop:
mov ebp,[Bytes3+edx*4]
inc ebp
add esi,4
mov [Bytes3+edx*4],ebp
mov [edi+ebp*4-4],eax
mov eax,[esi]
mov ebx,[eax]
ror ebx,16
mov dl,bl
dec ecx
jnz @@FifthLoop


mov ecx,[NumOfFaces]
mov edi,[FList]
mov esi,[SList]
mov eax,[esi]
mov ebx,[eax]
ror ebx,24
mov dl,bl
dec ecx
@@SixthLoop:
mov ebp,[Bytes4+edx*4]
inc ebp
mov ebx,[eax]
add esi,4
mov [Bytes4+edx*4],ebp
mov [edi+ebp*4-4],eax
mov eax,[esi]
ror ebx,24
mov dl,bl
dec ecx
jnz @@SixthLoop

ret
_TEXT	ENDS

END
