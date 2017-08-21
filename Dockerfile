FROM gardner/emsdk:latest

WORKDIR /opendht
COPY . /opendht

RUN apt-get update && \
    apt-get install cmake libreadline-dev libncurses5-dev libreadline-dev nettle-dev libgnutls28-dev \
            libargon2-0-dev libncurses-dev libmsgpack-dev cython3 python3-dev python3-setuptools -y && \
    mkdir -p /opendht/build && cd /opendht

RUN /opendht/embuild.sh

    # apt-get -y --purge remove curl git-core cmake && \
    # apt-get -y autoremove && apt-get clean && \
    # rm -rf /var/lib/apt/lists/*

VOLUME /opendht/build
