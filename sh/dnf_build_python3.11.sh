#!/usr/bin/env bash
set -ex

PY_VER="3.11.7"
INSTALL_TO="/opt/py/python-${PY_VER}"
SRC_DIR="Python-${PY_VER}"
TAR_XZ="${SRC_DIR}.tar.xz"

a=$(nproc --all) b=10
SRDEATH=$(( a > b ? b : a ))
THREADS=${THREADS:-$SRDEATH}

dnf update -y
dnf install -y git openssl-devel bzip2-devel libffi-devel xz xz-devel sqlite-devel readline-devel gcc g++ tk-devel zlib-devel wget

(
    mkdir -p /opt/py && cd /opt/py

    wget "https://www.python.org/ftp/python/${PY_VER}/${TAR_XZ}"

    tar xf "Python-${PY_VER}.tar.xz"

    cd "$SRC_DIR"

    ./configure CFLAGS="-O2 -fPIC -I/usr/include/openssl" \
      CXXFLAGS="-O2 -fPIC -I/usr/include/openssl" \
      --enable-shared LDFLAGS="-L/usr/lib -L/usr/lib64 -Wl,-rpath=${INSTALL_TO}/lib:/usr/lib64" \
      --prefix=${INSTALL_TO} --enable-optimizations --with-ensurepip=install
    make -j$THREADS
    make install
    cd ..
    rm -rf "$SRC_DIR" "${TAR_XZ}"

    dnf clean all
    rm -rf /var/cache/yum /var/cache/dnf

    ln -s $(basename "$INSTALL_TO") py
)

/opt/py/py/bin/python3 -m pip install pip -U
