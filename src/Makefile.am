bin_PROGRAMS = psxbuild psxinject psxrip

CPPFLAGS = $(LIBCDIO_CFLAGS) $(LIBISO9660_CFLAGS) $(LIBVCDINFO_CFLAGS)
LDADD =  $(LIBCDIO_LIBS) $(LIBISO9660_LIBS) $(LIBVCDINFO_LIBS) \
         $(BOOST_SYSTEM_LIB)

psxbuild_SOURCES = psxbuild.cpp
psxinject_SOURCES = psxinject.cpp
psxrip_SOURCES = psxrip.cpp
