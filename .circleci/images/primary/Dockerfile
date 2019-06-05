FROM circleci/buildpack-deps:bionic

RUN sudo apt-get -y -qq update
RUN sudo apt-get -y install software-properties-common
RUN sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
RUN sudo apt-get -y install cmake
RUN sudo apt-get -y install qemu-system-ppc qemu-user-static gcc-multilib-powerpc-linux-gnu gcc-powerpc-linux-gnu
RUN sudo apt-get -y install qemu-system-arm gcc-arm-linux-gnueabi libc6-dev-armel-cross gcc-aarch64-linux-gnu libc6-dev-arm64-cross
RUN sudo apt-get -y install libc6-dev-i386 clang clang-tools gcc-5 gcc-5-multilib gcc-6 valgrind
