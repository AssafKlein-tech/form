#: SmallSize 800M
#: TermsInSmall 20M
format 255;
Off Parallel;
on memdebugflag;
Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Global F = (-a-b+c+d-e+f+g+19*h-101*o)^15;


.Sort
#Printtimes
On Parallel;
off compress;
id g =  -17/11*h - 5/349*i + 14/3*d;
id a =  - 20/7*n + 19/51*m + 13/4*b + 195/14*f + 13/2*j; 
id c =  -33/4*p + 171/516*l + 9/5*j + 23/4*k;

.end