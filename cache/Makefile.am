noinst_LIBRARIES = libprotocol50.a

libprotocol50_a_SOURCES = \
	protocol50.cpp \
	protocol50.h \
	p50_actions.cpp \
	p50_actions.h

AM_CPPFLAGS = -I$(top_srcdir)

EXTRA_DIST = \
	README.md \
	KEY_LAYOUT_V1_CENSUS.md \
	key_layout_census.py \
	formal/Protocol50.tla \
	formal/Protocol50.cfg \
	formal/.gitignore \
	formal/check_trace.py \
	formal/README.md

check-local:
	python3 $(srcdir)/key_layout_census.py \
		--root $(top_srcdir) --check $(srcdir)/KEY_LAYOUT_V1_CENSUS.md
