FROM aureliendavid/gpac-ubuntu-deps:latest as dev


WORKDIR /gpac

# get code
COPY . gpac_public

# get manually built dependencies to the main folder
RUN bash -c "[[ -e /gpac-deps/gpac_public/extra_lib ]] && cp -av /gpac-deps/gpac_public/extra_lib/* /gpac/gpac_public/extra_lib/ && rm -rf /gpac-deps/gpac_public/extra_lib/*"



WORKDIR /gpac/gpac_public

# static build
RUN make distclean && ./configure --static-bin && make -j
RUN rm -rf /gpac/binaries && mkdir -p /gpac/binaries && cp -vf bin/gcc/* /gpac/binaries/ || true


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
