
init:
	rm -rf x/
	export ANDROID_SDK_ROOT=~/Library/Android/sdk
	meson setup x --buildtype=debug
	ninja -C x

start:
	cd x && ninja && cd ..;
	./run x

debug:
	cd x && ninja && cd ..;
	./run x -V debug
