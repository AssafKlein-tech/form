#:ThreadLoadBalancing ON
#create <small_terms_print_by_term.text>;
#create <small_terms_print.text>;
#:ON Threads;
#:ThreadBucketSize 100
 
Symbols a,b,c,d,e,f;

Local F = (a+b+c+d+e+f)^32;

id d = a + e;
.Sort

id c = f + b;
id a = f + e;

*print <small_terms_print_by_term.text> +s "%w %t";
 
.Sort
 
*#write <small_terms_print.text> "       + %+E",F
 
.end
}
