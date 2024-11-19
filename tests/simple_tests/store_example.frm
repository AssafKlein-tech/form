#:ThreadLoadBalancing ON
#create <print_by_term.text>;
#create <small_terms_print.text>;
#:ON Threads;
#:ThreadBucketSize 100
 
Symbols a,b,c,d,e,f,g,h,j,z;
.global
Global F = (a+b+c+d+e+f)^16;

id d = a + e;

.store
save temp.sav;
.sort

id c = a + b;
id a = c + e;

#Printtimes

.Sort
#Printtimes
 
.end

