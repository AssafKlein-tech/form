format 255;
Off Parallel;
Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Global F = (a+b-c+d-e+f+g)^7;


.Sort
On Parallel;
id g =  17*a + 5*b;
id f =  19*a + 20*c; 
id e =  -33*b - 100*c;
id d =  c;

.sort
print;

.end