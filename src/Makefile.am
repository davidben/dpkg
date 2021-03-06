## Process this file with automake to produce Makefile.in

localedir = $(datadir)/locale
pkgconfdir = $(sysconfdir)/@PACKAGE@

AM_CPPFLAGS = \
	-DLOCALEDIR=\"$(localedir)\" \
	-DADMINDIR=\"$(admindir)\" \
	-idirafter $(top_srcdir)/lib/compat \
	-I$(top_builddir) \
	-I$(top_srcdir)/lib


bin_PROGRAMS = dpkg dpkg-query dpkg-trigger

dpkg_SOURCES = \
	archives.c archives.h \
	cleanup.c \
	configure.c \
	depcon.c \
	enquiry.c \
	errors.c \
	filesdb.c filesdb.h \
	divertdb.c \
	statdb.c \
	help.c \
	main.c main.h \
	packages.c \
	pkg-array.c pkg-array.h \
	pkg-show.c \
	processarc.c \
	remove.c \
	select.c \
	trigproc.c \
	update.c

dpkg_LDADD = \
	../lib/dpkg/libdpkg.a \
	../lib/compat/libcompat.a \
	$(LIBINTL) \
	$(ZLIB_LIBS) \
	$(BZ2_LIBS) \
	$(SELINUX_LIBS)

dpkg_query_SOURCES = \
	filesdb.c filesdb.h \
	divertdb.c \
	pkg-array.c pkg-array.h \
	pkg-show.c \
	query.c

dpkg_query_LDADD = \
	../lib/dpkg/libdpkg.a \
	../lib/compat/libcompat.a \
	$(LIBINTL)

dpkg_trigger_SOURCES = \
	trigcmd.c

dpkg_trigger_LDADD = \
	../lib/dpkg/libdpkg.a \
	../lib/compat/libcompat.a \
	$(LIBINTL)

install-data-local:
	$(mkdir_p) $(DESTDIR)$(pkgconfdir)/dpkg.cfg.d
	$(mkdir_p) $(DESTDIR)$(admindir)/alternatives
	$(mkdir_p) $(DESTDIR)$(admindir)/info
	$(mkdir_p) $(DESTDIR)$(admindir)/updates
