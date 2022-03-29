#!/bin/bash -x

## to create the buildimage
## git clone https://github.com/fanquake/core-review.git
## cd core-review/guix/
## DOCKER_BUILDKIT=1 docker build --pull --no-cache -t alpine-guix - < Dockerfile

export ELEMENTS_SRC="$HOME/github/elements/"
set -e
mkdir -p /tmp/elementsguix/
running=$(docker container list | grep elementsbuild || :)
if [ -z "$running" ];then
    docker container stop elementsbuild || :
    docker container rm -f elementsbuild || :
    docker run -dt --name elementsbuild --privileged -v /tmp/elementsguix/:/elements/ alpine-guix
fi
#if you build a hash instead of a tag, remember to use only the first 12 chars
#tag=3aa4e68d5036
tag=elements-22.0-rc1

tagbuild=${tag#elements-}
builddir="guix-build-${tagbuild#v}"


sudo mkdir -p /tmp/elementsguix/
sudo rsync -aq --delete "${ELEMENTS_SRC}" /tmp/elementsguix/
sudo chown -R root:root /tmp/elementsguix/

cat >/tmp/elementsbuild.sh <<__EOF__
#!/bin/bash

set -ex
#guix pull
cd /elements
#git fetch -v --all
git checkout $tag
export SDK_PATH=/SDKs
export SOURCES_PATH=/sources
export BASE_CACHE=/base_cache
#export HOSTS="x86_64-linux-gnu"
#export HOSTS="x86_64-apple-darwin"
#export HOSTS="x86_64-apple-darwin18"
./contrib/guix/guix-clean

if [ ! -d /SDKs/Xcode-12.1-12A7403-extracted-SDK-with-libcxx-headers ];then
    mkdir -p /SDKs/
    pushd /SDKs/
        tar -xf /sources/Xcode-12.1-12A7403-extracted-SDK-with-libcxx-headers.tar.gz
    popd
fi

./contrib/guix/guix-build
find ${builddir}/output/ -type f -print0 | env LC_ALL=C sort -z | xargs -r0 sha256sum
__EOF__
chmod 700 /tmp/elementsbuild.sh
docker cp /tmp/elementsbuild.sh elementsbuild:/root
docker cp sources/. elementsbuild:/sources/
docker exec -it elementsbuild /root/elementsbuild.sh
mkdir -p out/
docker cp elementsbuild:/elements/${builddir}/output/ out/
