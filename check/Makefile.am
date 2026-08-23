TEST_BINS =
if BUILD_FORM
TEST_BINS += $(top_builddir)/sources/form$(EXEEXT)
endif
if BUILD_TFORM
TEST_BINS += $(top_builddir)/sources/tform$(EXEEXT)
endif
if BUILD_PARFORM
TEST_BINS += $(top_builddir)/sources/parform$(EXEEXT)
endif

TEST_OPTS =

TESTS =
if CONFIG_RUBY
TESTS_ENVIRONMENT = \
	RUBY="$(RUBY)" \
	TEST_BINS="$(TEST_BINS)" \
	TEST_OPTS="$(TEST_OPTS)" \
	"$(SHELL)"
TESTS += check-help.sh
else
TESTS_ENVIRONMENT = \
	TEST_BINS="$(TEST_BINS)" \
	"$(SHELL)"
endif
TESTS += benchmark-fu.sh

EXTRA_DIST = \
	.diagrams_cache/feyngraph_qcd_3_none.dat \
	.diagrams_cache/feyngraph_qcd_3_onepi_opicomponents1.dat \
	.diagrams_cache/qgraf_qcd_glu_glu_3_none.dat \
	.diagrams_cache/qgraf_qcd_glu_glu_3_onepifloop.dat \
	.diagrams_cache/qgraf_qcd_quaQUA_gluglu_2_onepifloop.dat \
	.diagrams_cache/qgraf_qcd_qua_qua_3_none.dat \
	.diagrams_cache/qgraf_qcd_qua_qua_3_onepifloop.dat \
	.diagrams_cache/qgraf_qcd_qua_quaglu_3_onepifloop.dat \
	benchmark-fu.sh \
	check-help.sh \
	check.rb \
	coverage.frm \
	diagrams.frm \
	diagrams/diagrams.h \
	examples.frm \
	extra/checkpoint.frm \
	extra/color.frm \
	extra/forcer.frm \
	extra/mincer.frm \
	extra/multithreaded.frm \
	extra/mzv-dm.frm \
	extra/series.frm \
	extra/z-fmft.frm \
	features.frm \
	fixes.frm \
	formunit/fu.frm \
	polynomial.frm \
	user.frm
