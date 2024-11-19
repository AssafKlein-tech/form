#:ThreadLoadBalancing ON

Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;
.global
Global F = (a+b+c+d)^2 * (f+e+g+h)^2 * (i+j+k)^2 * l^2;

.Sort

id e =  h + o ; 
id  l = m + n + o;

*.Sort

.store
save temp.sav;
.clear
*.sort
load temp.sav;
print ;
.end