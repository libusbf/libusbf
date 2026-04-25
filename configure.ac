AC_INIT([libusbf], [0.1], [robert.baldyga@unvertical.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])

AC_PROG_CC
AM_PROG_AR
AC_CONFIG_MACRO_DIR([m4])
AC_DEFINE([_GNU_SOURCE], [], [Use GNU extensions])

LT_INIT

# Checks for typedefs, structures, and compiler characteristics.
AC_C_INLINE
AC_TYPE_SIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_FUNC_MALLOC

# libaio for asynchronous endpoint I/O
AC_CHECK_LIB([aio], [io_setup], [:],
	[AC_MSG_ERROR([libaio is required (install libaio-dev)])])
AC_CHECK_HEADER([libaio.h], [],
	[AC_MSG_ERROR([libaio.h is required (install libaio-dev)])])

AC_CONFIG_FILES([Makefile src/Makefile examples/Makefile])

AC_OUTPUT
