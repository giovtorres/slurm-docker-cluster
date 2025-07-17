##*****************************************************************************
#  AUTHOR:
#    Thomas Cadeau<thomas.cadeau@ext.bull.net>
#
#  SYNOPSIS:
#    X_AC_FREEIPMI
#
#  DESCRIPTION:
#    Determine if the FREEIPMI libraries exists
##*****************************************************************************

AC_DEFUN([X_AC_FREEIPMI],
[
  _x_ac_freeipmi_dirs="/usr /usr/local"
  _x_ac_freeipmi_libs="lib64 lib"


  AC_ARG_WITH(
    [freeipmi],
    AS_HELP_STRING(--with-freeipmi=PATH,Specify path to freeipmi installation),
    [AS_IF([test "x$with_freipmi" != xno && test "x$with_freeipmi" != xyes],
           [_x_ac_freeipmi_dirs="$with_freeipmi"])])

  if [test "x$with_freeipmi" = xno]; then
    AC_MSG_NOTICE([support for freeipmi disabled])
  else
    AC_CACHE_CHECK(
      [for freeipmi installation],
      [x_ac_cv_freeipmi_dir],
      [
        for d in $_x_ac_freeipmi_dirs; do
          test -d "$d" || continue
          test -d "$d/include" || continue
          test -f "$d/include/ipmi_monitoring.h" || continue
          for bit in $_x_ac_freeipmi_libs; do
            test -d "$d/$bit" || continue
            _x_ac_freeipmi_cppflags_save="$CPPFLAGS"
            CPPFLAGS="-I$d/include $CPPFLAGS"
            _x_ac_freeipmi_libs_save="$LIBS"
            LIBS="-L$d/$bit -lipmimonitoring -lfreeipmi $LIBS"
            AC_LINK_IFELSE(
              [AC_LANG_PROGRAM(
                [[
                  #include <freeipmi/freeipmi.h>
                  #include <ipmi_monitoring.h>
                  #include <ipmi_monitoring_bitmasks.h>
                ]],
                [[
                  ipmi_ctx_t ipmi_ctx = ipmi_ctx_create();
                  int err;
                  unsigned int flag = 0;
                  return ipmi_monitoring_init (flag, &err);
                ]],
              )],
              [AS_VAR_SET(x_ac_cv_freeipmi_dir, $d)],
              [])
            CPPFLAGS="$_x_ac_freeipmi_cppflags_save"
            LIBS="$_x_ac_freeipmi_libs_save"
            test -n "$x_ac_cv_freeipmi_dir" && break
          done
          test -n "$x_ac_cv_freeipmi_dir" && break
        done
      ])

    if test -z "$x_ac_cv_freeipmi_dir"; then
      if test -z "$with_freeipmi"; then
        AC_MSG_WARN([unable to locate freeipmi installation (libipmonitoring/libfreeipmi])
      else
        AC_MSG_ERROR([unable to locate freeipmi installation (libipmonitoring/libfreeipmi])
      fi
    else
      FREEIPMI_CPPFLAGS="-I$x_ac_cv_freeipmi_dir/include"
      if test "$ac_with_rpath" = "yes"; then
        FREEIPMI_LDFLAGS="-Wl,-rpath -Wl,$x_ac_cv_freeipmi_dir/$bit -L$x_ac_cv_freeipmi_dir/$bit"
      else
        FREEIPMI_LDFLAGS="-L$x_ac_cv_freeipmi_dir/$bit"
      fi
      FREEIPMI_LIBS="-lipmimonitoring -lfreeipmi"
      AC_DEFINE(HAVE_FREEIPMI, 1, [Define to 1 if freeipmi library found])
    fi

    AC_SUBST(FREEIPMI_LIBS)
    AC_SUBST(FREEIPMI_CPPFLAGS)
    AC_SUBST(FREEIPMI_LDFLAGS)
  fi

  AM_CONDITIONAL(BUILD_IPMI, test -n "$x_ac_cv_freeipmi_dir")
])
