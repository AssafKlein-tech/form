#: SmallSize 1K
#: SmallExtension 3K
#: LargeSize 10K
#: largepatches 8

* Regression repro for the PF_InitTree rbuf-array overflow.
* Order matters: an MR (+merger) module runs FIRST, so the master caches
* PF.rbufs sized to nummergers+1 (small). A later parallel + non-MR module
* needs numtasks = nummappers (big) -- on the buggy binary that loops past
* the cached array -> MPI_Irecv into garbage -> hang. small_test_red never
* triggers this because its first module is non-MR (caches the big size).
* Same id rules / order as small_test_red, so final F must match SHA
* 1e85be3fb498f229c309eb85984aeb81bb4e0896f8331b910b543289911fe8c2.

format 255;
on parallel;
on mapreduce;
Symbols a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;

Global F = (-a-b+c+d-e+f+g+19*h-101*o)^11;

.Sort
id g =  -17/11*h - 5/349*i + 14/3*d;
id a =  - 20/7*n + 19/51*m + 13/4*b + 195/14*f + 13/2*j;
id c =  -33/4*p + 171/516*l + 9/5*j + 23/4*k;
.Sort
off mapreduce;
id h = b;
id e = d;
id g = f;
id i = b;
id k = d;
id j = f;
id n = b;
id m = d;
id p = f;
id o = b;
.Sort
print;
.end
