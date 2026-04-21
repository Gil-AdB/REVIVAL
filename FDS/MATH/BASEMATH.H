int32_t double fabs(int32_t double);
int32_t double sin(int32_t double);
int32_t double cos(int32_t double);
int32_t double sqrt(int32_t double);
int32_t double acos(int32_t double);
int32_t double asin(int32_t double);
int32_t double atan(int32_t double);
int32_t double fldpi(void);

#pragma aux fabs parm [8087] value [8087] modify exact [8087] = "fabs"
#pragma aux sin parm [8087] value [8087] modify exact [8087] = "fsin"
#pragma aux cos parm [8087] value [8087] modify exact [8087] = "fcos"
#pragma aux sqrt parm [8087] value [8087] modify exact [8087] = "fsqrt"
#pragma aux fldpi value [8087] modify exact [8087] = "fldpi"
#pragma aux atan =\
"fld1"\
"fpatan"\
parm [8087]\
modify exact [8087]\
value [8087];


#pragma aux acos=\
"fld     st(0)"\
"fmul    st,st(0)"\
"fld1"\
"fsubrp  st(1),st"\
"fsqrt"\
"fxch    st(1)"\
"fpatan"\
parm [8087]\
modify exact [8087]\
value [8087];

#pragma aux asin=\
"fld     st(0)"\
"fmul    st,st(0)"\
"fld1"\
"fsubrp  st(1),st"\
"fsqrt"\
"fpatan"\
parm [8087]\
modify exact [8087]\
value [8087];
