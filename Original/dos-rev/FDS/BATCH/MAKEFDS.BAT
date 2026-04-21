@Echo Off
echo Linking main file...
Call Link /C FDS.CPP
WLink @FDS.Lnk
rem WLink /d2 /hw file *.Obj name fds
