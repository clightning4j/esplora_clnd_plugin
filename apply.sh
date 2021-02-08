#!/bin/sh
#
# call this script from lightning repo clone
# or supply path to the lightning repo clone
# as the first argument

a="/$0"; a=${a%/*}; a=${a:-.}; a=${a#/}/; EDIR=$(cd $a; pwd)
test "$1" = "" || { test -d $1; cd $1; }

cp $EDIR/esplora.c plugins
patch -p1 < $EDIR/Makefile.patch
#sed -i 's/LDLIBS = /LDLIBS = -lcurl -lssl -lcrypto /g' Makefile
