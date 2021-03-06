FROM ubuntu:14.04
MAINTAINER Disa Mhembere

RUN apt-get update
RUN apt-get -y upgrade
RUN apt-get -o Dpkg::Options::="--force-confold" --force-yes -y upgrade

RUN apt-get -y install \
        build-essential \
        git \
        libboost-all-dev \
        libmpich2-dev \
        libnuma-dbg libnuma-dev libnuma1 \
        python-all-dev python-pip \
        vim \
        libaio-dev \
        libatlas-base-dev \
        libgoogle-perftools-dev

WORKDIR /home/ubuntu/
# TODO: make with multiple procs
RUN git clone --recursive https://github.com/disa-mhembere/knor.git
WORKDIR knor
RUN make -j8

# To ingest example
RUN pip install --upgrade pip
RUN pip install pyyaml

# Enter bash shell
ENTRYPOINT ["bash"]
