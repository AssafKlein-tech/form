format 255;
#create <terms_powerfirst16G.text>;
#:ThreadLoadBalancing ON
on powerfirst;
Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Local F = (a+b+c+d)^10 * (f+e+g+h)^10 * (i+j+k)^10 * l^10;


.Sort
id d = j + k; 
.sort
id l = n + m;
.sort
id f = o + p;
.Sort 

#write <terms_powerfirst_16G.text> "    + %+E",F

.Sort  //on power first order
off powerfirst;

.end