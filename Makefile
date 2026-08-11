all:
	mkdir -p build
	cd build && cmake -DCMAKE_BUILD_TYPE=DEBUG .. && make
clean:
	rm -rf build
