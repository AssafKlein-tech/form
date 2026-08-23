#: MaxTermSize 500
#: ScratchSize 1K
Off compress;
Symbol x,y,z,i;
CFunction f;
Local test = sum_(i,1,100,f(i*(x+y))*(x+y)^20) - 52824783675150;
Bracket f;
.sort
Keep Brackets;
Identify f(x?) = x;
.sort
Identify x = 1;
Identify y = 2;
Print +s;
.end
