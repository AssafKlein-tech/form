format 255;
#create <smallterms_presort.text>;
#:ThreadLoadBalancing ON

Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Local F = (a+b+c+d)^4 * (f+e+g+h)^4 * (i+j+k)^4 * l^5;


.Sort

id l =  m + n ;
id d =  j + k ; 
id f =  o + p ;

 
#write <smallterms_presort.text> "       + %+E",F


.end