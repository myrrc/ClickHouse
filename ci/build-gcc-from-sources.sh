#!/usr/bin/env bash
set -e

source default-config

sudo apt-get install -y curl

if [[ "${GCC_SOURCES_VERSION}" == "latest" ]]; then
    GCC_SOURCES_VERSION=$(curl -sSL https://ftpmirror.gnu.org/gcc/ | grep -oE 'gcc-[0-9]+(\.[0-9]+)+' | sort -Vr | head -n1)
fi

GCC_VERSION_SHORT=$(echo "$GCC_SOURCES_VERSION" | grep -oE '[0-9]' | head -n1)

echo "Will download ${GCC_SOURCES_VERSION} (short version: $GCC_VERSION_SHORT)."

THREADS=$(grep -c ^processor /proc/cpuinfo)

mkdir "${WORKSPACE}/gcc"
pushd "${WORKSPACE}/gcc"

wget https://ftpmirror.gnu.org/gcc/${GCC_SOURCES_VERSION}/${GCC_SOURCES_VERSION}.tar.xz
tar xf ${GCC_SOURCES_VERSION}.tar.xz
pushd ${GCC_SOURCES_VERSION}
./contrib/download_prerequisites
popd
mkdir gcc-build
pushd gcc-build
../${GCC_SOURCES_VERSION}/configure --enable-languages=c,c++ --disable-multilib
make -j $THREADS
sudo make install

popd
popd

sudo ln -sf /usr/local/bin/gcc /usr/local/bin/gcc-${GCC_GCC_SOURCES_VERSION_SHORT}
sudo ln -sf /usr/local/bin/g++ /usr/local/bin/g++-${GCC_GCC_SOURCES_VERSION_SHORT}
sudo ln -sf /usr/local/bin/gcc /usr/local/bin/cc
sudo ln -sf /usr/local/bin/g++ /usr/local/bin/c++

echo '/usr/local/lib64' | sudo tee /etc/ld.so.conf.d/10_local-lib64.conf
sudo ldconfig

hash gcc g++
gcc --version

export CC=gcc
export CXX=g++
