format 255;
Off Parallel;
Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Global F = (a+b-c+d-e+f+g)^7;


.Sort
On Parallel;
id g =  1/2*a + 1/3*b;
id f =  1/6*a + 1/5*c; 
id e =  -1/6*b - 1/10*c;
id d =  c;

.sort
print;

.end