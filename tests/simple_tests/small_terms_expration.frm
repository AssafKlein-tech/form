format 255;
#create <small_terms_print_by_term.text>;
#create <small_terms_print.text>;

Symbols a, b, c, d, e, f, g, h;

Local F = (a + b + c + d + e + f + g) ^ 32;

id d = a + g;
id h = f - g;

print <small_terms_print_by_term.text> +s "%w %t";

.Sort

#write <small_terms_print.text> "       + %+E",F

.end

