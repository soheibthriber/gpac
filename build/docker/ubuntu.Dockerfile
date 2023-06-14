FROM ubuntu:latest

WORKDIR /gpac

# set up the machine
RUN apt-get -yqq update && apt-get install -y --no-install-recommends build-essential pkg-config g++ git scons cmake yasm fakeroot dpkg-dev devscripts debhelper ccache
RUN apt-get install -y --no-install-recommends zlib1g-dev libfreetype6-dev libjpeg62-dev libpng-dev libmad0-dev libfaad-dev libogg-dev libvorbis-dev libtheora-dev liba52-0.7.4-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libavdevice-dev libxv-dev x11proto-video-dev libgl1-mesa-dev x11proto-gl-dev libxvidcore-dev libssl-dev libjack-dev libasound2-dev libpulse-dev libsdl2-dev dvb-apps mesa-utils
RUN apt-get autoremove -y && apt-get clean -y


# get code
COPY . gpac_public


# get manually built dependencies
RUN git clone --depth=1 https://github.com/gpac/deps_unix
WORKDIR /gpac/deps_unix
RUN git submodule update --init --recursive --force --checkout
RUN ./build_all.sh linux


# init artifacts
RUN rm -rf /gpac/binaries && mkdir -p /gpac/binaries


WORKDIR /gpac/gpac_public

# static build
RUN make distclean && ./configure --static-bin && make -j
RUN cp -vf bin/gcc/* /gpac/binaries/ || true


# deb package
RUN rm -f *.deb && echo 7 > debian/compat && make distclean && make deb
RUN mv -v *.deb /gpac/binaries/


#install
RUN dpkg -i /gpac/binaries/*.deb

# work build
# RUN make distclean
# RUN ./configure
# RUN make -j
# RUN make install



CMD ["gpac"]
