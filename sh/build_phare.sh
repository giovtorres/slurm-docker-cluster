#!/usr/bin/env bash

# export PYTHONPATH="${WORK}/build:${PWD}:${PWD}/pyphare"

cd /data

# write script it missing
[ ! -f "build.sh" ] && cat > build.sh << EOL
export MPI_HOME=/opt/mpi
export PATH="/opt/py/py/bin:$PATH"

mkdir -p build
cd build
CMAKE_CXX_FLAGS="-DNDEBUG -g0 -O3 -march=native -mtune=native" # -DPHARE_DIAG_DOUBLES=1
cmake ~/PHARE/ -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="\${CMAKE_CXX_FLAGS}" -Dphare_configurator=ON
make
EOL

chmod +x build.sh
[ ! -d "build" ] && ./build.sh
