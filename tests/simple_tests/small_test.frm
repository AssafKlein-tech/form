format 255;
Off Parallel;
Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Global F = (a+b+c+d+e+f+g)^8;


.Sort
On Parallel;
id g =  a + b;
id f =  a + c; 
id e =  b + c;
id d =  c;

.end