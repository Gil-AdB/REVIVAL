.386P
_DATA   SEGMENT DWORD PUBLIC 'DATA'
Rdx1                    dd 257 dup(?) ; Radix offset table (Bits 0-7)
Rdx2                    dd 257 dup(?) ; Radix offset table (Bits 8-15)
Rdx3                    dd 257 dup(?) ; Radix offset table (Bits 16-23)
Rdx4                    dd 257 dup(?) ; Radix offset table (Bits 24-31)

List1               dd 0           ; Main Face list
List2               dd 0           ; Secondary face list
NumOfFaces          dd 0           ; Amount of Faces to sort
SZO               	equ 32         ; Sort-Z offset inside FACETYPE structure.
TEBP            		dd ?   		     ; Temporary EBP
_DATA ENDS

_TEXT   SEGMENT BYTE PUBLIC 'CODE'
	ASSUME  cs:_TEXT, ds:_DATA


	PUBLIC  Radix_Asm_
Radix_Asm_:
; on entry: ESI - FList  EDX - SList EBX - NumOfFaces
Mov [TEBP],EBP ; Save previous EBP
Mov [List1],ESI
Mov [List2],EDX
Mov [NumOfFaces],EBX

; Clear all Radix offset tables, total 1028 DWords.
Xor EAX,EAX
Mov ECX,1028
Mov EDI,OFFSET Rdx1
REP StoSD

; STEP 1: Sample Face Z values for statistics
Mov ECX,[NumOfFaces]
Mov EDI,OFFSET Rdx1
Mov ESI,[List1]
Xor EDX,EDX
@Loop1:
	; Insert FList[I]->LDZ to EAX
	Mov EAX,[ESI]
	Mov EAX,[EAX+SZO]

	; Distribute EAX across Radix offset tables
	Mov DL,AL
	Inc DWord PTR [EDI+EDX*4+4]
	Mov DL,AH
	Inc DWord PTR [EDI+EDX*4+1032]
	Rol EAX,16
	Mov DL,AL
	Inc DWord PTR [EDI+EDX*4+2060]
	Mov DL,AH
	Inc DWord PTR [EDI+EDX*4+3088]

	Add ESI,4 ; Move to next face pointer
	Dec ECX
JNZ @Loop1

; STEP 2: Convert Radix offset Tables to Incremental statistics
Mov EDI,OFFSET Rdx1
Mov EBX,4
@Loop2:
	Mov ECX,256
	Add EDI,4
	Xor EAX,EAX
	@InnerLoop2:
		Add [EDI],EAX
		Mov EAX,[EDI]
		Add EDI,4
		Dec ECX
	JNZ @InnerLoop2
	Dec EBX
JNZ @Loop2

; Third Loop C code
;   for(I=0;I<NumOfFaces;I++)
;   {
;       Z=Bytes1[(FList[I])->LDZ & 0x000000FF]++;
;       SList[Z] = FList[I];
;   }

; STEP 3: Redistribute List1 to List2 using Lowest Radix (Rdx1)
; Since STEPS 3-6 are nearly Identical, only this one will be commented.
Mov ECX,[NumOfFaces]
Mov EDX,Offset Rdx1
Mov ESI,[List1]
Mov EDI,[List2]
Xor EBX,EBX
@Loop3:
	Mov EBP,[ESI]       ; Get value of stored pointer in List1
	Mov EAX,[EBP+SZO]   ; Now Contains Sort-Z value (from struct FaceType)
	Mov BL,AL           ; Low Radix
	Mov EAX,[EDX+EBX*4] ; Takes value from Radix offset table
	Inc DWord PTR [EDX+EBX*4] ; Update Radix offset table
	Mov [EDI+EAX*4],EBP ; Re-distribute Pointer to List2
	Add ESI,4           ; Next pointer in List1
	Dec ECX
JNZ @Loop3

; STEP 4: Redistribute List2 to List1 using second Radix (Rdx2)
Mov ECX,[NumOfFaces]
Mov EDX,Offset Rdx2
Mov ESI,[List2]
Mov EDI,[List1]
Xor EBX,EBX
@Loop4:
	Mov EBP,[ESI]
	Mov EAX,[EBP+SZO]
	Mov BL,AH
	Mov EAX,[EDX+EBX*4]
	Inc DWord PTR [EDX+EBX*4]
	Mov [EDI+EAX*4],EBP
	Add ESI,4
	Dec ECX
JNZ @Loop4

; STEP 5: Redistribute List1 to List2 using Third Radix (Rdx3)
Mov ECX,[NumOfFaces]
Mov EDX,Offset Rdx3
Mov ESI,[List1]
Mov EDI,[List2]
Xor EBX,EBX
@Loop5:
	Mov EBP,[ESI]
	Mov EAX,[EBP+SZO]
	Rol EAX,16
	Mov BL,AL
	Mov EAX,[EDX+EBX*4]
	Inc DWord PTR [EDX+EBX*4]
	Mov [EDI+EAX*4],EBP
	Add ESI,4
	Dec ECX
JNZ @Loop5

; STEP 6: Redistribute List2 to List1 using Highest Radix (Rdx4)
Mov ECX,[NumOfFaces]
Mov EDX,Offset Rdx4
Mov ESI,[List2]
Mov EDI,[List1]
Xor EBX,EBX
@Loop6:
	Mov EBP,[ESI]
	Mov EAX,[EBP+SZO]
	Rol EAX,16
	Mov BL,AH
	Mov EAX,[EDX+EBX*4]
	Inc DWord PTR [EDX+EBX*4]
	Mov [EDI+EAX*4],EBP
	Add ESI,4
	Dec ECX
JNZ @Loop6
Mov EBP,[TEBP] ; Restore previous EBP
ret
_TEXT   ENDS

END
