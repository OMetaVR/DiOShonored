# Linux build of the d3d9 spike. DXVK=<dir with lib/ and include/dxvk> defaults to the in-repo install.
DXVK ?= $(abspath ../../../third_party/dxvk/install-linux)
CXX ?= clang++
CXXFLAGS ?= -O2 -g -Wall -Wno-unknown-pragmas -isystem $(DXVK)/include/dxvk $(shell pkg-config --cflags sdl3)
d3d9_test: main_sdl.cpp d3d9_scene.cpp d3d9_scene.h shaders_gen.h
	$(CXX) $(CXXFLAGS) -o $@ main_sdl.cpp d3d9_scene.cpp $(shell pkg-config --libs sdl3) -ldl
run: d3d9_test
	LD_LIBRARY_PATH=$(DXVK)/lib ./d3d9_test
clean:
	rm -f d3d9_test
