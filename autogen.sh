#!/bin/sh
# Run this to generate all the initial makefiles, etc.

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

PKG_NAME="eos-installer"

(test -f $srcdir/configure.ac \
  && test -f $srcdir/eos-installer.doap) || {
    echo -n "**Error**: Directory "\`$srcdir\'" does not look like the"
    echo " top-level eos-installer directory"
    exit 1
}

which gnome-autogen.sh || {
    echo "You need to install gnome-common"
    exit 1
}

REQUIRED_AUTOMAKE_VERSION=1.7
. gnome-autogen.sh
