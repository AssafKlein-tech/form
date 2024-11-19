format 255;
#create <smallterms_presort.text>;
#create <smallterms_postsort.text>;
#:ThreadLoadBalancing ON

Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Local F = (a+b+c+d)^4 * (f+e+g+h)^4 * (i+j+k)^4 * l^5;


.Sort

id l =  m + n ;
id d =  j + k ; 
id f =  o + p ;

 
print <smallterms_presort.text> +s " %t";

.Sort

#write <smallterms_postsort.text> "       + %+E",F

.end