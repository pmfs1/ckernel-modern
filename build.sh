#!/bin/sh
sudo apt-get update -y && sudo apt-get install software-properties-common -y --install-recommends libc6-dev-i386 gcc-multilib g++-multilib genisoimage && make -f Makefile clean && make -f Makefile krlean && make -f Makefile krlean vmdk && make -f Makefile krlean iso

# chmod +x build.sh && ./build.sh