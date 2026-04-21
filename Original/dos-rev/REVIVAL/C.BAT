wmake
rem wcl386 rev.cpp fds.lib lib\rev.lib -d2 -hw -mf -fp5
rem @WCL386 rev.cpp fds.lib lib\rev.lib /d2 /L=dos4g /I=D:\WATCOM\H /cc++ /mf /fp5
rem WPP386 rev.cpp -d2 -hw -mf -fp5
call link /c rev.cpp
wLink @REV.LNK
