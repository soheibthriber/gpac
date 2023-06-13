FROM ubuntu:latest

WORKDIR /gpac


RUN apt -yqq update
RUN apt install -y build-essential pkg-config g++ git scons cmake yasm
RUN apt install -y fakeroot dpkg-dev devscripts debhelper ccache
RUN apt install -y zlib1g-dev libfreetype6-dev libjpeg62-dev libpng-dev libmad0-dev libfaad-dev libogg-dev libvorbis-dev libtheora-dev liba52-0.7.4-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libavdevice-dev libxv-dev x11proto-video-dev libgl1-mesa-dev x11proto-gl-dev libxvidcore-dev libssl-dev libjack-dev libasound2-dev libpulse-dev libsdl2-dev dvb-apps mesa-utils
RUN apt autoremove -y && apt clean -y


RUN git clone https://github.com/gpac/gpac.git gpac_public
RUN git clone https://github.com/gpac/deps_unix

WORKDIR /gpac/deps_unix
RUN git submodule update --init --recursive --force --checkout
RUN ./build_all.sh linux


RUN rm -rf /gpac/binaries
RUN mkdir -p /gpac/binaries

WORKDIR /gpac/gpac_public

# static build
RUN make distclean
RUN ./configure --static-bin
RUN make -j
RUN cp -vf bin/gcc/* /gpac/binaries/ || true


# deb package
RUN rm -f *.deb
RUN make distclean
RUN echo 7 > debian/compat
RUN make deb
RUN mv -v *.deb /gpac/binaries/


#install
RUN dpkg -i /gpac/binaries/*.deb

# work build
# RUN make distclean
# RUN ./configure
# RUN make -j
# RUN make install



CMD ["gpac", "-version"]
