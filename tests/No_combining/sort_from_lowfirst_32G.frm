format 255;
#create <terms_powerfirs_t32G.text>;
#:ThreadLoadBalancing ON
on powerfirst;
off threadstats;
Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Local F = (a+b+c+d)^11 * (f+e+g)^11 * (i+j+k)^11 * l^12;


.Sort
id d = j + k; 
id g = g + h;
.sort
id l = n + m;
.sort
id f = o + p;


.Sort  //on power first order
off powerfirst;

.end