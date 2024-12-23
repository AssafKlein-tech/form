format 255;
#:ThreadLoadBalancing ON
on powerfirst;
Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Local F = (a+b+c+d)^8 * (f+e+g+h)^8 * (i+j+k)^8 * l^8;


.Sort
id l =  m + n ;
id d =  j + k ; 
id f =  o + p ;

.Sort  //on power first order
off powerfirst;
.sort //sorting the different order


.end