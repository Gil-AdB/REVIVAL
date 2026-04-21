.586p
_DATA   SEGMENT DWORD PUBLIC 'DATA'
align 4
EXTRN _MMXState : dword
align 1
_DATA   ENDS

_TEXT   SEGMENT USE32 BYTE PUBLIC 'CODE'
	ASSUME  cs:_TEXT, ds:_DATA


	PUBLIC  new_int6_
new_int6_:
push eax
mov [_MMXState],0
mov word ptr [emmsblat],9090h
mov al,20h
out 20h,al
pop eax
iretd

	PUBLIC  emmshit_
emmshit_:
emmsblat db 0fh,77h
ret


_TEXT   ENDS

END
