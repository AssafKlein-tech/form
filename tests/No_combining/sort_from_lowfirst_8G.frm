format 255;
#:ThreadLoadBalancing ON
on powerfirst;
Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Local F = (a+b+c+d)^9 * (f+e+g+h)^9 * (i+j+k)^9 * l^9;


.Sort
id l =  m + n ;
id d =  j + k ; 
id f =  o + p ;
.Sort 

//#write <terms_powerfirst_8G.text> "    + %+E",F

.Sort  //on power first order
off powerfirst;
.sort //sorting the different order

//#write <terms_lowfirst_8G.text> "    + %+E",F

.end
//#create <terms_powerfirsS8G.text>;