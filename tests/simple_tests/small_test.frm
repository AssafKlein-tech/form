format 255;
#create <smallterms_presort.text>;
#create <smallterms_postsort.text>;
#:ThreadLoadBalancing ON

Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Local F = (a+b+c+d+e+f+g)^8;


.Sort

id g =  a + b;
id f =  a + c; 
id e =  b + d;

.end