# plugindir is set in configure

plugin_LTLIBRARIES = libgstdvbvideosink.la libgstdvbaudiosink.la

# for the next set of variables, rename the prefix if you renamed the .la

# sources used to compile this plug-in
libgstdvbvideosink_la_SOURCES = gstdvbvideosink.c
libgstdvbaudiosink_la_SOURCES = gstdvbaudiosink.c

# flags used to compile this plugin
# add other _CFLAGS and _LIBS as needed
libgstdvbvideosink_la_CFLAGS = $(GST_CFLAGS)
libgstdvbvideosink_la_LIBADD = $(GST_LIBS) -lgstbase-0.10
libgstdvbvideosink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)

libgstdvbaudiosink_la_CFLAGS = $(GST_CFLAGS)
libgstdvbaudiosink_la_LIBADD = $(GST_LIBS) -lgstbase-0.10
libgstdvbaudiosink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)

# headers we need but don't want installed
noinst_HEADERS = gstdvbvideosink.h gstdvbaudiosink.h

