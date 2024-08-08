dnl Information on the package.
AC_PREREQ(2.69)
AC_INIT([PSXImager], [2.1], [cb@cebix.net], [psximager])
AC_CONFIG_SRCDIR([src/psxrip.cpp])
AM_INIT_AUTOMAKE([1.12 foreign])

AC_PROG_CXX

PKG_CHECK_MODULES(LIBCDIO, libcdio >= 2.0.0)
PKG_CHECK_MODULES(LIBISO9660, libiso9660 >= 2.0.0)
PKG_CHECK_MODULES(LIBVCDINFO, libvcdinfo >= 2.0.0)

AX_BOOST_BASE([1.50.0])
AX_BOOST_FILESYSTEM
AX_BOOST_REGEX
AX_BOOST_SYSTEM

AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT
