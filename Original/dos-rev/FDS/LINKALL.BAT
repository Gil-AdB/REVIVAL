@Echo off
echo Flood Demo system - Compiling everything and linking together...
echo.
if not exist DPMI.Obj      Call MakeDPMI
if not exist ISR.Obj       Call MakeISR
if not exist VESA.Obj      Call MakeVESA

if not exist MDSPLAY.Obj   Call MakeMDS

if not exist Math.Obj      Call MakeMath

if not exist V3D_Read.Obj  Call MakeV3D
if not exist 3DS_Read.Obj  Call Make3DS
if not exist FLD_Read.Obj  Call MakeFLD

if not exist ImgCode.Obj   Call MakeImg
if not exist ImgProc.Obj   Call MakeImgP
if not exist ImgGenr.Obj   Call MakeImgG

if not exist Cameras.Obj   Call MakeCams
if not exist Frustrum.Obj  Call MakeClip

if not exist Render.Obj    Call MakeRend

if not exist Fillers.Obj   Call MakeFill
if not exist Bilinear.Obj  Call MakeAsm

if not exist PclSys.Obj    Call MakePcl
if not exist Preproc.Obj   Call MakeMisc

Call MakeFDS
