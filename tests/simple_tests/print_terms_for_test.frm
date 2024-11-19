format 255;
#create <terms_mid_presort.text>;
#create <terms_mid_postsort.text>;
#:ThreadLoadBalancing ON
on powerfirst;
Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Local F = (a+b+c+d)^7 * (f+e+g+h)^8 * (i+j+k)^7 * l^6;


.Sort
id l =  m + n ;
id d =  j + k ; 
id f =  o + p ;
.Sort 

#write <terms_mid_presort.text> "    + %+E",F

.Sort  //on power first order
on lowfirst;
.sort //sorting the different order

#write <terms_mid_postsort.text> "    + %+E",F

.end