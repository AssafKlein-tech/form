format 255;
#create <terms_powerfirst_1G.text>;
#create <terms_lowfirst_1G.text>;
#:ThreadLoadBalancing ON
on powerfirst;
Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Local F = (a+b+c+d)^7 * (f+e+g+h)^8 * (i+j+k)^7 * l^6;


.Sort
id l =  m + n ;
id d =  j + k ; 
id f =  o + p ;
.Sort 

#write <terms_powerfirst_1G.text> "    + %+E",F

.Sort  //on power first order
off powerfirst;
.sort //sorting the different order

//#write <terms_lowfirst_1G.text> "    + %+E",F

.end