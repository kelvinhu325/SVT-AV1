# 
# Copyright(c) 2019 Intel Corporation
# SPDX - License - Identifier: BSD - 2 - Clause - Patent
# 

#!/bin/bash

function clean {
	rm -R -f debug
	rm -R -f release
    rm -R -f ../../Bin/Debug
    rm -R -f ../../Bin/Release
}

function debug {
	mkdir -p debug
	mkdir -p ../../Bin/Debug
	cd debug
	PATH=$PATH:/usr/local/bin/
	cmake ../../.. 					\
		-DCMAKE_BUILD_TYPE=Debug			\
		-DCMAKE_C_COMPILER=$CMAKE_COMPILER		\
		-DCMAKE_ASM_NASM_COMPILER=$CMAKE_ASSEMBLER	\
		
	# Compile the Library	
	make SvtAv1EncApp
	cd ..
}

function release {
	mkdir -p release
	mkdir -p ../../Bin/Release
	cd release
	PATH=$PATH:/usr/local/bin/
	cmake ../../.. 					\
		-DCMAKE_BUILD_TYPE=Release			\
		-DCMAKE_C_COMPILER=$CMAKE_COMPILER		\
		-DCMAKE_ASM_NASM_COMPILER=$CMAKE_ASSEMBLER	\
	
	# Compile the Library	
	make SvtAv1EncApp
        cd  ..	
}

# Defines
CMAKE_ASSEMBLER=yasm
GCC_COMPILER=gcc
ICC_COMPILER=/opt/intel/composerxe/bin/icc

if [ ! -e $ICC_COMPILER ]; then
	CMAKE_COMPILER=$GCC_COMPILER
elif [ "$1" == "gcc" ]; then
	CMAKE_COMPILER=$GCC_COMPILER
elif [ "$2" == "gcc" ]; then
	CMAKE_COMPILER=$GCC_COMPILER
else
	CMAKE_COMPILER=$ICC_COMPILER
fi

if [ $# -eq 0 ]; then
	debug
	release
elif [ "$1" = "clean" ]; then
	clean
elif [ "$1" = "debug" ]; then
	debug
elif [ "$1" = "release" ]; then
	release
elif [ "$1" = "cpp" ]; then
	debug
	release
elif [ "$1" = "all" ]; then
	debug
	release
elif [ "$1" = "gcc" ]; then
	debug
	release
else
	echo "build.sh <clean|all|debug|release|help>"
fi

exit
