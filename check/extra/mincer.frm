#ifndef `TEST'
  #message Use -D TEST=XXX
  #terminate
#else
  #include `NAME_' # `TEST'
#endif
.end

*--#[ minceex2 :
#-

* This is a modified version of the example which comes with minceex.tgz.

#include- minceex.h
#include- treatqaqa.prc
Off Statistics;
#define GAUGE "0"
#define POW "2"
#define PROJ "0"
#define CURRENT "F2"
#define MSBAR "1"
#message GAUGE = `GAUGE', POW = `POW', PROJ = `PROJ';
*
*	Now some variables that are calcdia specific
*
AutoDeclare Symbol z,x,k,y;
AutoDeclare index ii,mu,nu,ro,si,ka,la;
I	K1,K2;
*Format nospaces;
.global
#ifdef `CHILD'
#include diagram`CHILD'.h
#else
*#include diagram.h
L d9c=+vqg(1,mu1)*fp(1,p2)*vqg(1,mu2)*fp(1,p3)*vqg(1,MU)*fp(1,p4)*vqg(1,mu3)*\
 fp(1,p5)*vqg(1,mu4)*fp(1,p6)*vqg(1,NU)*fp(1,p1)*vqg(1,mu5)*v3g(mu6,-[P+p1],\
 mu7,-p7,mu8,[P+p2])*Dg(mu1,mu8,[P+p2])*Dg(mu2,mu4,p8)*Dg(mu3,mu7,p7)*Dg(mu5\
 ,mu6,[P+p1])
#define NAME "d9c"
#define TOPO "no"
#define SUBTOPO "12"
#endif
		;
#call rename(`TOPO')
#call special(`TOPO')
#call treatqaqa
.sort
*
*	Now we look for special cases that can be made faster
*
#call redoTOPO
#call integral(`TOPO',0)
#call propagators
#ifdef `GAUGE'
#if ( `GAUGE' >= 0 )
	if ( count(xi,1) > `GAUGE' ) Discard;
#endif
#endif
#ifdef `WITHEXPANSION'
 Bracket xi;
 Print +f +s;
 .sort
 Multiply ep;
 #call expansion(6)
 .sort
 Multiply ep^-1/2;
#endif
.sort
Drop;
Local diff = d9c - (
      BasicNOIntegral*rat( - 384*ep^8 - 1696*ep^7 + 4992*ep^6 - 2136*ep^5 -
      448*ep^4 + 968*ep^3 - 832*ep^2 + 384*ep - 64,8*ep^2 - 14*ep + 3) +
      GschemeConstants(0,0)*BasicT1Integral*rat(27648*ep^15 - 195840*ep^14 -
      343680*ep^13 + 803520*ep^12 + 1246176*ep^11 - 1272672*ep^10 - 1151520*
      ep^9 + 957872*ep^8 + 714192*ep^7 - 697088*ep^6 - 51728*ep^5 + 238016*
      ep^4 - 104032*ep^3 + 18352*ep^2 - 1056*ep,96*ep^7 + 184*ep^6 - 156*ep^5
       - 458*ep^4 - 147*ep^3 + 103*ep^2 + 27*ep - 9) + GschemeConstants(0,0)^2
      *GschemeConstants(1,0)*rat(23040*ep^17 + 532608*ep^16 + 1136064*ep^15 -
      1916768*ep^14 - 4050640*ep^13 + 1126944*ep^12 + 4923760*ep^11 - 1652076*
      ep^10 - 1430780*ep^9 + 1240236*ep^8 - 6616*ep^7 - 1392932*ep^6 + 940332*
      ep^5 + 364204*ep^4 - 578232*ep^3 + 204504*ep^2 - 29568*ep + 1440,96*
      ep^11 + 440*ep^10 + 420*ep^9 - 810*ep^8 - 1731*ep^7 - 609*ep^6 + 675*
      ep^5 + 475*ep^4 - 36*ep^2) + GschemeConstants(0,0)^2*GschemeConstants(2,
      0)*rat( - 35328*ep^15 - 416640*ep^14 - 1206976*ep^13 + 202336*ep^12 +
      3340112*ep^11 + 2979040*ep^10 - 1120000*ep^9 - 1748944*ep^8 - 54512*ep^7
       + 331648*ep^6 - 148976*ep^5 + 154048*ep^4 + 101968*ep^3 - 131184*ep^2
       + 10752*ep + 576,24*ep^9 + 132*ep^8 + 222*ep^7 - 21*ep^6 - 489*ep^5 -
      597*ep^4 - 297*ep^3 - 54*ep^2) + GschemeConstants(0,0)*GschemeConstants(
      1,0)*GschemeConstants(2,0)*rat(912384*ep^19 + 10126080*ep^18 + 32486400*
      ep^17 - 32112384*ep^16 - 129704832*ep^15 + 6938672*ep^14 + 142865984*
      ep^13 + 29654488*ep^12 - 86908036*ep^11 - 32951720*ep^10 + 46159128*ep^9
       + 10332596*ep^8 - 19502292*ep^7 + 2353272*ep^6 + 4274592*ep^5 - 2006868
      *ep^4 - 78592*ep^3 + 299112*ep^2 - 77856*ep + 5472,864*ep^13 + 3888*
      ep^12 + 3216*ep^11 - 8664*ep^10 - 15930*ep^9 - 2133*ep^8 + 10656*ep^7 +
      4986*ep^6 - 2154*ep^5 - 1425*ep^4 + 108*ep^3 + 108*ep^2));
Print +s;
.end
assert succeeded?
assert result("diff") =~ expr("0")
*--#] minceex2 : 
*--#[ minceex4 :
#-

* This is a modified version of the example which comes with minceex.tgz.

#include- minceex.h
#include- treatqaqa.prc
Off Statistics;
#define GAUGE "0"
#define POW "4"
#define PROJ "0"
#define CURRENT "F2"
#define MSBAR "1"
#message GAUGE = `GAUGE', POW = `POW', PROJ = `PROJ';
*
*	Now some variables that are calcdia specific
*
AutoDeclare Symbol z,x,k,y;
AutoDeclare index ii,mu,nu,ro,si,ka,la;
I	K1,K2;
*Format nospaces;
.global
#ifdef `CHILD'
#include diagram`CHILD'.h
#else
*#include diagram.h
L d9c=+vqg(1,mu1)*fp(1,p2)*vqg(1,mu2)*fp(1,p3)*vqg(1,MU)*fp(1,p4)*vqg(1,mu3)*\
 fp(1,p5)*vqg(1,mu4)*fp(1,p6)*vqg(1,NU)*fp(1,p1)*vqg(1,mu5)*v3g(mu6,-[P+p1],\
 mu7,-p7,mu8,[P+p2])*Dg(mu1,mu8,[P+p2])*Dg(mu2,mu4,p8)*Dg(mu3,mu7,p7)*Dg(mu5\
 ,mu6,[P+p1])
#define NAME "d9c"
#define TOPO "no"
#define SUBTOPO "12"
#endif
		;
#call rename(`TOPO')
#call special(`TOPO')
#call treatqaqa
.sort
*
*	Now we look for special cases that can be made faster
*
#call redoTOPO
#call integral(`TOPO',0)
#call propagators
#ifdef `GAUGE'
#if ( `GAUGE' >= 0 )
	if ( count(xi,1) > `GAUGE' ) Discard;
#endif
#endif
#ifdef `WITHEXPANSION'
 Bracket xi;
 Print +f +s;
 .sort
 Multiply ep;
 #call expansion(6)
 .sort
 Multiply ep^-1/2;
#endif
.sort
Drop;
Local diff = d9c - (
      BasicNOIntegral*rat( - 2304*ep^11 - 7232*ep^10 + 52000*ep^9 + 18160*ep^8
       - 114872*ep^7 - 91176*ep^6 + 39656*ep^5 + 2032*ep^4 + 416*ep^3 + 24168*
      ep^2 - 23376*ep + 4608,48*ep^3 - 204*ep^2 + 228*ep - 45) + 
      GschemeConstants(0,0)*BasicT1Integral*rat(27648*ep^21 - 516864*ep^20 - 
      3994112*ep^19 - 4253952*ep^18 + 33084608*ep^17 + 126363536*ep^16 + 
      119266688*ep^15 - 373133320*ep^14 - 1152987376*ep^13 - 491716608*ep^12
       + 1502474704*ep^11 + 977746688*ep^10 - 1553743792*ep^9 - 190578368*ep^8
       + 928197648*ep^7 - 530750040*ep^6 + 127311552*ep^5 - 1788976*ep^4 - 
      5651008*ep^3 + 1262784*ep^2 - 138240*ep,384*ep^11 + 2272*ep^10 + 1456*
      ep^9 - 15584*ep^8 - 34512*ep^7 - 618*ep^6 + 64479*ep^5 + 59969*ep^4 + 
      4013*ep^3 - 12919*ep^2 - 1800*ep + 900) + GschemeConstants(0,0)^2*
      GschemeConstants(1,0)*rat( - 681984*ep^25 - 4028928*ep^24 + 30485760*
      ep^23 + 308835072*ep^22 + 547931904*ep^21 - 2811462304*ep^20 - 
      14396648176*ep^19 - 19187749024*ep^18 + 23314444608*ep^17 + 104940053952
      *ep^16 + 115883017456*ep^15 + 2432489392*ep^14 - 92180742080*ep^13 - 
      38313734648*ep^12 + 21762074288*ep^11 - 31000077880*ep^10 - 42553905504*
      ep^9 + 79077155256*ep^8 + 21490049040*ep^7 - 67631487480*ep^6 + 
      31397120800*ep^5 - 5285019968*ep^4 + 882770432*ep^3 + 231957120*ep^2 - 
      104799744*ep + 4976640,1152*ep^17 + 17952*ep^16 + 106608*ep^15 + 253392*
      ep^14 - 160656*ep^13 - 2311950*ep^12 - 4924581*ep^11 - 2103234*ep^10 + 
      8135736*ep^9 + 14715084*ep^8 + 7900773*ep^7 - 2965308*ep^6 - 5140632*
      ep^5 - 1611936*ep^4 + 205200*ep^3 + 129600*ep^2) + GschemeConstants(0,0)
      ^2*GschemeConstants(2,0)*rat( - 216576*ep^23 - 5999744*ep^22 - 67223936*
      ep^21 - 339143552*ep^20 - 535105088*ep^19 + 2379379240*ep^18 + 
      14115407424*ep^17 + 27832941760*ep^16 + 5470276600*ep^15 - 84856605328*
      ep^14 - 184233479784*ep^13 - 176330297192*ep^12 - 55476773000*ep^11 + 
      40106128128*ep^10 + 37842213784*ep^9 + 9905154984*ep^8 + 19017680048*
      ep^7 + 4642553608*ep^6 - 20224103440*ep^5 + 2239915360*ep^4 + 1885640256
      *ep^3 + 1080892416*ep^2 + 208714752*ep + 9953280,288*ep^15 + 4752*ep^14
       + 30960*ep^13 + 90936*ep^12 + 38034*ep^11 - 558279*ep^10 - 1749240*ep^9
       - 2036232*ep^8 + 458928*ep^7 + 4438827*ep^6 + 5967630*ep^5 + 3989196*
      ep^4 + 1377000*ep^3 + 194400*ep^2) + GschemeConstants(0,0)*
      GschemeConstants(1,0)*GschemeConstants(2,0)*rat(12441600*ep^27 + 
      241436160*ep^26 + 2367744768*ep^25 + 10821396480*ep^24 + 11958792064*
      ep^23 - 91453237152*ep^22 - 422826539856*ep^21 - 612335646096*ep^20 + 
      555540699096*ep^19 + 3284346867128*ep^18 + 4250586842880*ep^17 + 
      351374300560*ep^16 - 4438997900512*ep^15 - 3318133410424*ep^14 + 
      1904859439392*ep^13 + 2636129161552*ep^12 - 1099209124824*ep^11 - 
      1167398292216*ep^10 + 947152336592*ep^9 + 180023292400*ep^8 - 
      443242365600*ep^7 + 71547538392*ep^6 + 77098297952*ep^5 - 23731782976*
      ep^4 - 2606689024*ep^3 + 851670912*ep^2 - 21385728*ep + 34836480,10368*
      ep^19 + 160704*ep^18 + 943200*ep^17 + 2156976*ep^16 - 1893240*ep^15 - 
      21289068*ep^14 - 42156126*ep^13 - 9612999*ep^12 + 86474421*ep^11 + 
      130685994*ep^10 + 39750372*ep^9 - 67459635*ep^8 - 60975099*ep^7 - 
      1731780*ep^6 + 15592104*ep^5 + 4220208*ep^4 - 1004400*ep^3 - 388800*ep^2
      ));
Print +s;
.end
# Too slow
#pend_if valgrind?
assert succeeded?
assert result("diff") =~ expr("0")
*--#] minceex4 : 
*--#[ testmincer1 :
#-
#include mincer2.h
.global
*
*   We try here a diagram with a ladder topology
*
Local F = Q.p1^2*Q.p3^2/p1.p1^3/p2.p2/p3.p3^3/p4.p4/p5.p5/p6.p6/p7.p7/p8.p8
        *Q.Q^2;
*
*   The diagrams should be multiplied by ep^3 first.
*   Probably this should have been done inside mincer but for historical
*   reasons it isn't and if I change it now many programs will have a problem.
*
Multiply ep^3;
#call integral(la)
Print;
.end
assert succeeded?
assert result("F") =~ expr("
 - 6695/5184 - 1/48*ep^-3 + 41/288*ep^-2 - 1541/6912*ep^-1 + 5/4*z5 - 1/
6*z3 + 2342947/248832*ep + 25/8*ep*z6 + 15/2*ep*z5 - 1/4*ep*z4 + 3017/
576*ep*z3 - 35/2*ep*z3^2 - 1608359/46656*ep^2 + 317/16*ep^2*z7 + 75/4*
ep^2*z6 + 18*ep^2*z5 + 3017/384*ep^2*z4 + 27467/1728*ep^2*z3 - 1083/16*
ep^2*z3*z4 - 105*ep^2*z3^2
")
*--#] testmincer1 :
*--#[ testmincer2 :
#-
#include mincer2.h
.global
*
*   We try here a diagram with a ladder topology
*
Local F = Q.p1^2*Q.p3^2/p1.p1^3/p2.p2/p3.p3^3/p4.p4/p5.p5*Q.Q;
*
*   The diagrams should be multiplied by ep^3 first.
*   This gives results to order ep.
*   Probably this should have been done inside mincer but for historical
*   reasons it isn't and if I change it now many programs will have a problem.
*
Multiply ep^3;
#call integral(t1)
Print;
.end
assert succeeded?
assert result("F") =~ expr("
 - 25/64 + 5/16*ep^-2 - 1/2*ep^-1 + 3/8*z3 - 49/16*ep + 9/16*ep*z4 + 33/
4*ep*z3 + 2201/256*ep^2 + 21/8*ep^2*z5 + 99/8*ep^2*z4 - 87/16*ep^2*z3 -
1485/256*ep^3 + 45/8*ep^3*z6 + 231/4*ep^3*z5 - 261/32*ep^3*z4 - 33/2*
ep^3*z3 - 9/8*ep^3*z3^2
")
*--#] testmincer2 :
