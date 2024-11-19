#:ThreadLoadBalancing ON
#create <small_terms_print_by_term.text>;
#create <small_terms_print.text>;

Symbols a,b,c,d,e,f,g,h,i,z;

Local F = (a+b+c+d+e+f)^35;

.Sort

id f =  g + h;
id a =  i + z;
print <small_terms_print_by_term.text> +s " %t";
.sort
#write <small_terms_print.text> "       + %+E",F
 
.end