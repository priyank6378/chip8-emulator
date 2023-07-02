all: clean build 

clean :
	rm -f chip8

build :
	g++ chip8emu.cpp -o chip8