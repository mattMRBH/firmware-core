.PHONY: all configure build test clean run

all: run

configure:
	cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build: configure
	cmake --build build

test: build
	ctest --test-dir build --output-on-failure

clean:
	rm -rf build

run: build
	ctest --test-dir build --output-on-failure -V
