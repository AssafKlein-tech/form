format 255;
#:ThreadLoadBalancing ON
on powerfirst;
Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Local F = (a+b+c+j+k)^10 * (f+e+g+h)^10 * (i+j+k)^10 * (n+m)^10;


.Sort
id f = o + p;
.sort
off powerfirst;

.end