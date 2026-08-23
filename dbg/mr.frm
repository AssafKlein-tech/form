on mapreduce;
Symbols a,b,c,d,e,f;
Local F = (a+2*b+3*c+d+e+f)^9;
.sort
off mapreduce;
Format nospaces;
Print +s F;
.end
