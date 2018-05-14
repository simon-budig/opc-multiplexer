bin_PROGRAMS = opc-mpx \
	artsend

opc_mpx_SOURCES =   \
	artnetnode.c \
	artnetnode.h \
	gmqtt-source.c  \
	gmqtt-client.c  \
	opc-mpx.c   \
	opc-types.h \
	opcbroker.c \
	opcbroker.h \
	opcclient.c \
	opcclient.h \
	pxsource.c \
	pxsource.h

artsend_SOURCES = \
	artsend.c

AM_CPPFLAGS =             \
	-D_GNU_SOURCE     \
	$(GLIB_CFLAGS)    \
	$(GOBJECT_CFLAGS) \
	$(CFLAGS)

opc_mpx_LDADD =         \
	$(GLIB_LIBS)    \
	$(GOBJECT_LIBS)

artsend_LDADD =         \
	$(GLIB_LIBS)    \
	$(GOBJECT_LIBS)
