AC_INIT([libusbf], [0.1], [r.baldyga@hackerion.com])
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

AC_CONFIG_FILES([Makefile src/Makefile examples/Makefile])

AC_OUTPUT
