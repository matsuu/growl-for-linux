displaydir = $(pkglibdir)/display
display_LTLIBRARIES = libballoon.la
libballoon_la_SOURCES = balloon.c balloon.xpm display_balloon.xpm
libballoon_la_CFLAGS = $(GTK2_CFLAGS) $(LIBCURL_CFLAGS) $(SQLITE3_CFLAGS)
libballoon_la_LIBADD = $(GTK2_LIBS) $(LIBCURL_LIBS) $(SQLITE3_LIBS)

EXTRA_DIST = Makefile.w32
