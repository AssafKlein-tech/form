format 255;

#:ThreadLoadBalancing ON
on powerfirst;
Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Local F = (a+b+c+d)^5 * (f+e+g+h)^5 * (i+j+k)^5 * l^5;


.Sort
id l =  m + n ;
id d =  j + k ; 
id f =  o + p ;
#write <terms_powerfirst_small.text> "    + %+E",F
.sort
.end