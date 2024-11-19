#:ThreadLoadBalancing ON
#create <print_by_term.text>;
#create <small_terms_print.text>;
#:ON Threads;
#:ThreadBucketSize 100
 
Symbols a,b,c,d,e,f,g,h,j,z;

Local F = (a+b+c+d+e+f)^32;

id d = a + e;

.Sort


id c = f + b;
id a = f + e;

#Printtimes

.Sort
#Printtimes

 
.end

