

start:
	cd x && ninja && cd ..;
	./run x

debug:
	cd x && ninja && cd ..;
	./run x -V debug
