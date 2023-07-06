all: clean build 

clean :
	rm -f chip8 a.out

build :
	g++ src/chip8emu.cpp -lSDL2 -lSDL2_image -lm -o chip8