#!/usr/bin/env bash
set -ex

a=$(nproc --all) b=10
SRDEATH=$(( a > b ? b : a ))
THREADS=${THREADS:-$SRDEATH}

# MPI is installed via dnf as it's a dep of hdf5-devel. but we need to build for slurm :(
export MPI_HOME="/opt/mpi" # /data = shared volume (use MPI_HOME for Cmake config)

yum -y update
dnf install -y autoconf make m4 flex bison gawk libtool pmix-devel
git clone https://github.com/open-mpi/ompi --depth 1 --recursive --shallow-submodules
(
    cd ompi
    git submodule update --init --recursive
    ./autogen.pl
    ./configure --prefix="${MPI_HOME}" --with-slurm --with-pmix --with-hwloc
    make -j${THREADS}
    make install
)
rm -rf ompi
