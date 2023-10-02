#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_timer.h>
#include <map>

#define WIDTH   32
#define HEIGHT  64
#define SCALE   20
#define FPS     60

typedef unsigned char BYTE;
typedef unsigned short WORD;


/* HARDWARE SPECIFICATIONS */

BYTE m_GameMemory[0xfff] ; // 0xfff bytes of memory
BYTE m_Registers[16] ; // 16 registers, 1 byte each
WORD m_AddressI ; // 16-bit address register I
WORD m_ProgramCounter ; // 16-bit program counter
std::vector<WORD> m_stack ; // the 16-bit stack
BYTE m_ScreenData[64][32] ; // 64x32 is the screen size
bool m_KeysPressed[16];     // specifies if the key is pressed
std::atomic<Uint8> m_DelayTimer(0) ; // 8-bit delay timer
std::atomic<Uint8> m_SoundTimer(0) ; // 8-bit sound timer

std::atomic<bool> ProgramRunning(true);

BYTE m_keyMappings[16] = {
    'x' , '1' , '2' , '3' ,
    'q' , 'w' , 'e' , 'a' ,
    's' , 'd' , 'z' , 'c' ,
    '4' , 'r' , 'f' , 'v'
};

std::map<char,int> key_to_index = {
    {'x',0}, {'1',1}, {'2',2}, {'3',3},
    {'q',4}, {'w',5}, {'e',6}, {'a',7},
    {'s',8}, {'d',9}, {'z',10}, {'c',11},
    {'4',12}, {'r',13}, {'f',14}, {'v',15}
};


// SDL stuff
SDL_Window* window;
SDL_Surface* screenSurface;
SDL_Event e;


/*
In chip-8 the game is loaded from 
memory location 0x200.
0-1ff is reserved for interpreter.
*/


/////////////////// OPCODES //////////////////////

// call machine code routine at NNN.
void Opcode0NNN(WORD opcode){
    ;
}

// Clears the screen.
void Opcode00E0(WORD opcode){
    memset(m_ScreenData, 0, sizeof(m_ScreenData));
}

// Returns from a subroutine.
void Opcode00EE(WORD opcode){
    m_ProgramCounter = m_stack[m_stack.size()-1];
    m_stack.pop_back();
}

// Jumps to address NNN.
void Opcode1NNN(WORD opcode){
    m_ProgramCounter = opcode & 0xFFF;
}

// Calls subroutine at NNN.
void Opcode2NNN(WORD opcode){
    m_stack.push_back(m_ProgramCounter);
    m_ProgramCounter = opcode & 0xFFF;
}

// Skips the next instruction if VX equals NN (usually the next instruction is a jump to skip a code block).
void Opcode3XNN(WORD opcode){
    int x = opcode & 0xF00;
    x >>= 8;
    int nn = opcode & 0x0FF;
    if (m_Registers[x] == nn) m_ProgramCounter+=2;
}

// Skips the next instruction if VX does not equal NN (usually the next instruction is a jump to skip a code block).
void Opcode4XNN(WORD opcode){
    int x = m_Registers[(opcode & 0xF00)>>8];
    int nn = opcode & 0x0FF;
    if (x != nn) m_ProgramCounter+=2;
}

// Skips the next instruction if VX equals VY (usually the next instruction is a jump to skip a code block).
void Opcode5XY0(WORD opcode){
    int x = m_Registers[(opcode & 0xF00)>>8];
    int y = m_Registers[(opcode & 0X0F0)>>4];
    if (x == y) m_ProgramCounter+=2;
}

// Sets VX to NN.
void Opcode6XNN(WORD opcode){
    int regx = opcode & 0xF00;
    regx >>= 8;
    WORD nn = opcode & 0x0FF;
    m_Registers[regx] = nn;
}

// Adds NN to VX (carry flag is not changed).   
void Opcode7XNN(WORD opcode){
    int regx = opcode & 0xF00;
    regx >>= 8;
    WORD nn = opcode & 0x0FF;
    m_Registers[regx] += nn;
}

// Sets VX to the value of VY.
void Opcode8XY0(WORD opcode){
    m_Registers[(opcode & 0xF00)>>8] = m_Registers[(opcode & 0X0F0)>>4];
}

// Sets VX to VX or VY. (bitwise OR operation)
void Opcode8XY1(WORD opcode){
    m_Registers[(opcode & 0xF00)>>8] |= m_Registers[(opcode & 0X0F0)>>4];
}

// Sets VX to VX and VY. (bitwise AND operation)
void Opcode8XY2(WORD opcode){
    m_Registers[(opcode & 0xF00)>>8] &= m_Registers[(opcode & 0X0F0)>>4];
}

// Sets VX to VX xor VY.
void Opcode8XY3(WORD opcode){
    m_Registers[(opcode & 0xF00)>>8] ^= m_Registers[(opcode & 0X0F0)>>4];
}

// Adds VY to VX. VF is set to 1 when there's a carry, and to 0 when there isn't.
void Opcode8XY4(WORD opcode){
    int x = opcode & 0xF00; x >>= 8;
    int y = opcode & 0x0F0; y >>= 4;
    int sum = m_Registers[x] + m_Registers[y];
    if (sum > 255) m_Registers[0xF] = 1;
    else m_Registers[0xF] = 0;
    m_Registers[x] = sum & 0xFF;
}

// VY is subtracted from VX. VF is set to 0 when there's a borrow, and 1 when there isn't.
void Opcode8XY5(WORD opcode){
    m_Registers[0xF] = 1;
    int regx = opcode & 0x0F00 ; // mask off reg x
    regx = regx >> 8 ; // shift x across
    int regy = opcode & 0x00F0 ; // mask off reg y
    regy = regy >> 4 ; // shift y across
    BYTE xval = m_Registers[regx] ;
    BYTE yval = m_Registers[regy] ;
    if (yval > xval) // if this is true will result in a value < 0
        m_Registers[0xF] = 0 ;
    m_Registers[regx] = xval-yval ;
}

// Stores the least significant bit of VX in VF and then shifts VX to the right by 1.
void Opcode8XY6(WORD opcode){
    m_Registers[0xF] = m_Registers[(opcode & 0xF00)>>8]&1;
    m_Registers[(opcode & 0xF00)>>8] >>= 1;
}


// Sets VX to VY minus VX. VF is set to 0 when there's a borrow, and 1 when there isn't.
void Opcode8XY7(WORD opcode){
    m_Registers[0xF] = 1;
    int regx = opcode & 0x0F00 ; // mask off reg x
    regx = regx >> 8 ; // shift x across
    int regy = opcode & 0x00F0 ; // mask off reg y
    regy = regy >> 4 ; // shift y across
    BYTE xval = m_Registers[regx] ;
    BYTE yval = m_Registers[regy] ;
    if (xval > yval) // if this is true will result in a value < 0
        m_Registers[0xF] = 0 ;
    m_Registers[regx] = yval-xval ;
}

// Stores the most significant bit of VX in VF and then shifts VX to the left by 1.
void Opcode8XYE(WORD opcode){
    m_Registers[0xF] = m_Registers[(opcode & 0xF00)>>8]>>7;
    m_Registers[(opcode & 0xF00)>>8] <<= 1;
}

// Skips the next instruction if VX doesn't equal VY. (Usually the next instruction is a jump to skip a code block)
void Opcode9XY0(WORD opcode){
    if (m_Registers[(opcode & 0xF00)>>8] != m_Registers[(opcode & 0X0F0)>>4]) m_ProgramCounter+=2;
}

// Sets I to the address NNN.
void OpcodeANNN(WORD opcode){
    m_AddressI = opcode & 0xFFF;
}

// Jumps to the address NNN plus V0.
void OpcodeBNNN(WORD opcode){
    m_ProgramCounter = m_Registers[0] + (opcode & 0xFFF);
}

// Sets VX to the result of a bitwise and operation on a random number (Typically: 0 to 255) and NN.
void OpcodeCXNN(WORD opcode){
    m_Registers[(opcode & 0xF00) >> 8] = ((rand())) & (opcode & 0x0FF);
}

// Draws a sprite at coordinate (VX, VY) that has a width of 8 pixels and a height of N pixels.
void OpcodeDXYN(WORD opcode){
    int regx = opcode & 0x0F00;
    regx >>= 8;
    int regy = opcode & 0x00F0 ;
    regy >>= 4 ;

    int height = opcode & 0x000F;
    int coordx = m_Registers[regx];
    int coordy = m_Registers[regy];

    m_Registers[0xF] = 0;

    for (int yline = 0 ; yline < height ; yline++){
        BYTE data = m_GameMemory[m_AddressI+yline];
        int xpixelinv = 7;
        int xpixel = 0;
        for (xpixel = 0 ; xpixel<8 ; xpixel++, xpixelinv--){
            int mask = 1<<xpixelinv;
            if (data & mask){
                int x = coordx + xpixel ; 
                int y = coordy + yline ;
                x %= HEIGHT;
                y %= WIDTH;
                if ( m_ScreenData[x][y] == 1){
                    m_Registers[0xF] = 1;
                }
                m_ScreenData[x][y] ^= 1;
            }
        }
    }
}

void OpcodeEX9E(WORD opcode){
    int regx = opcode & 0xF00;
    regx >>= 8;
    if (m_KeysPressed[m_Registers[regx]] == 1) m_ProgramCounter += 2;
}

void OpcodeEXA1(WORD opcode){
    int regx = opcode & 0xF00;
    regx >>= 8;
    if (m_KeysPressed[m_Registers[regx]] == 0) m_ProgramCounter += 2;
}

void OpcodeFX07(WORD opcode){
    int regx = opcode & 0xF00;
    regx >>= 8;
    m_Registers[regx] = m_DelayTimer;
}

void OpcodeFX0A(WORD opcode){
    int regx = opcode & 0xF00;
    regx >>= 8;
    int keypressed = 0;
    for (int i = 0 ; i < 16 ; i++){
        if (m_KeysPressed[i] == 1){
            m_Registers[regx] = m_keyMappings[i];
            keypressed = 1;
        }
    }
    SDL_Delay(1000/FPS);
    if (keypressed == 0) m_ProgramCounter -= 2;
}

void OpcodeFX15(WORD opcode){
    m_DelayTimer = m_Registers[(opcode & 0xF00)>>8];
}

void OpcodeFX18(WORD opcode){
    m_SoundTimer = m_Registers[(opcode & 0xF00)>>8];
}


// Adds VX to I.
void OpcodeFX1E(WORD opcode){
    int regx = opcode & 0xF00;
    regx >>= 8;
    m_AddressI += m_Registers[regx];
}

// Sets I to the location of the sprite for the character in VX. Characters 0-F (in hexadecimal) are represented by a 4x5 font.
void OpcodeFX29(WORD opcode){
    int regx = opcode & 0xF00;
    regx >>= 8;
    m_AddressI = m_GameMemory[m_Registers[regx]];
}

// Stores the binary-coded decimal representation of VX, with the most significant of three digits at the address in I,
void OpcodeFX33(WORD opcode){
    int regx = opcode & 0x0F00 ;
    regx >>= 8 ;

    int value = m_Registers[regx] ;

    int hundreds = value / 100 ;
    int tens = (value / 10) % 10 ;
    int units = value % 10 ;

    m_GameMemory[m_AddressI] = hundreds ;
    m_GameMemory[m_AddressI+1] = tens ;
    m_GameMemory[m_AddressI+2] = units ;
}

// Stores V0 to VX (including VX) in memory starting at address I. The offset from I is increased by 1 for each value written,
void OpcodeFX55(WORD opcode){
    int regx = opcode & 0xF00;
    regx >>= 8;
    int tmp_addressI = m_AddressI;
    for (int i = 0 ; i<=regx ; i++){
        m_GameMemory[tmp_addressI++] = m_Registers[i];
    }
    // m_AddressI = tmp_addressI;
}

// Fills V0 to VX (including VX) with values from memory starting at address I. The offset from I is increased by 1 for each value written,
void OpcodeFX65(WORD opcode){
    int regx = opcode & 0xF00;
    regx >>= 8;
    int tmp_addressI = m_AddressI;
    for (int i = 0 ; i<=regx ; i++){
        m_Registers[i] = m_GameMemory[tmp_addressI++];
    }
    // m_AddressI = tmp_addressI;
}

////////////////////////// OPCODES END //////////////////////////

// resets the CPU
void CPUReset(char *filename){
    ProgramRunning = true;

    m_AddressI = 0;
    m_ProgramCounter = 0x200;
    memset(m_Registers, 0, sizeof(m_Registers)); // reset the registers to 0.

    // fontset loading
    BYTE fontset[80] = {
        0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
        0x20, 0x60, 0x20, 0x20, 0x70, // 1
        0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
        0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
        0x90, 0x90, 0xF0, 0x10, 0x10, // 4
        0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
        0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
        0xF0, 0x10, 0x20, 0x40, 0x40, // 7
        0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
        0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
        0xF0, 0x90, 0xF0, 0x90, 0x90, // A
        0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
        0xF0, 0x80, 0x80, 0x80, 0xF0, // C
        0xE0, 0x90, 0x90, 0x90, 0xE0, // D
        0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
        0xF0, 0x80, 0xF0, 0x80, 0x80  // F
    };

    for (int i = 0 ; i<80 ; i++){
        m_GameMemory[i] = fontset[i];
    }

    // load in the game
    FILE *in;
    in = fopen(filename, "rb");
    fread(&m_GameMemory[0x200], 0xfff, 1, in);
    fclose(in);

    // reset the keys
    for (int i = 0 ; i<16 ; i++){
        m_KeysPressed[i] = false;
    }
}

// returns the next opcode that is in memory
WORD GetNextOpcode(){
    WORD opcode = 0;
    // instruction is stored in big-endian form
    opcode = m_GameMemory[m_ProgramCounter];
    opcode <<= 8;
    opcode |= m_GameMemory[m_ProgramCounter+1];
    m_ProgramCounter += 2;
    return opcode;
}

// Displays the screen in terminal
void display_screen(){
    for (int y = 0 ; y<32 ; y++){
        for (int x = 0 ; x<64 ; x++){
            if (m_ScreenData[x][y] == 1)
                std::cout << " 0 ";
            else
                std::cout << "   ";
        }
        std::cout << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000/60));
    system("clear");
}

void loadSurfaceFromMatrix(){
	int bytes_per_pixel = screenSurface->format->BytesPerPixel;

	for (int y = 0 ; y<WIDTH * SCALE; y++){
		for (int x = 0 ; x<HEIGHT * SCALE ; x++){
			Uint8* pixel = (Uint8 *)screenSurface->pixels + y*screenSurface->pitch + x*bytes_per_pixel;
			*(Uint32 *)pixel = 0xFFFFFF * (bool)m_ScreenData[x/SCALE][y/SCALE];
		}	
	}
}

void decreseTimer(){
    printf("Timer thread started\n");
    while (ProgramRunning){
        std::this_thread::sleep_for(std::chrono::milliseconds(1000/60));
        if (m_DelayTimer > 0)
            m_DelayTimer--;
    }
    printf("Timer thread exited\n");
}

void keyUpdate(){
    printf("Key thread started\n");
    while (ProgramRunning){
        while (SDL_PollEvent(&e)){
            if (e.type == SDL_QUIT){
                SDL_DestroyWindow( window );
                SDL_Quit();
                ProgramRunning = false;
            }
            else if (e.type == SDL_KEYDOWN){
                int key = e.key.keysym.sym;
                m_KeysPressed[key_to_index[key]] = true;
            }
            else if (e.type == SDL_KEYUP){
                int key = e.key.keysym.sym;
                m_KeysPressed[key_to_index[key]] = false;
            }
        }
        SDL_Delay(1000/FPS);
    }
    printf("Key thread exited\n");
}


void RunEmulator(char* filename){	
    printf("Starting emulator\n");

    CPUReset(filename);
    int lsb = 0;

    // start timer thread 
    std::thread timer_thread(decreseTimer);

    // key updating thread
    std::thread key_update(keyUpdate);

    while (ProgramRunning){
        WORD opcode = GetNextOpcode();
        int msb = (opcode & 0xF000);
        msb >>= 12;
        switch (msb){
            case 0 :
                if ((opcode & 0x00F) == 0)
                    Opcode00E0(opcode);
                else if ((opcode & 0xF00) == 0)
                    Opcode00EE(opcode);
                else 
                    Opcode0NNN(opcode);
                break;
                
            case 1 : 
                Opcode1NNN(opcode);
                break;
            
            case 2 : 
                Opcode2NNN(opcode);
                break;
            
            case 3 : 
                Opcode3XNN(opcode);
                break;
            
            case 4 : 
                Opcode4XNN(opcode);
                break;
            
            case 5 : 
                Opcode5XY0(opcode);
                break;

            case 6 : 
                Opcode6XNN(opcode);
                break;
            
            case 7 : 
                Opcode7XNN(opcode);
                break;

            case 8 :
                lsb = opcode & 0x00F;
                switch (lsb){
                    case 0:
                        Opcode8XY0(opcode);
                        break;
                    case 1:
                        Opcode8XY1(opcode);
                        break;
                    case 2:
                        Opcode8XY2(opcode);
                        break;
                    case 3:
                        Opcode8XY3(opcode);
                        break;
                    case 4:
                        Opcode8XY4(opcode);
                        break;
                    case 5:
                        Opcode8XY5(opcode);
                        break;
                    case 6:
                        Opcode8XY6(opcode);
                        break;
                    case 7:
                        Opcode8XY7(opcode);
                        break;
                    case 14:
                        Opcode8XYE(opcode);
                        break;
                    
                }
                break;

            case 9 : 
                Opcode9XY0(opcode);
                break;

            case 0xA : 
                OpcodeANNN(opcode);
                break;
            
            case 0xB : 
                OpcodeBNNN(opcode);
                break;

            case 0xC : 
                OpcodeCXNN(opcode);
                break;

            case 0xD :
                OpcodeDXYN(opcode);
                loadSurfaceFromMatrix();
                SDL_UpdateWindowSurface(window);
                SDL_Delay(1000/FPS);
                break;
            
            case 0xE : 
                if ((opcode & 0x00F) == 1)
                    OpcodeEXA1(opcode);
                else 
                    OpcodeEX9E(opcode);
                break;
            
            case 0xF : 
                lsb = opcode & 0x0FF;
                switch (lsb){
                    case 0x07:
                        OpcodeFX07(opcode);
                        break; 
                    case 0x0A:
                        OpcodeFX0A(opcode);
                        break;  
                    case 0x15:
                        OpcodeFX15(opcode);
                        break;
                    case 0x18:
                        OpcodeFX18(opcode);
                        break;
                    case 0x1E:
                        OpcodeFX1E(opcode);
                        break;  
                    case 0x29:
                        OpcodeFX29(opcode);
                        break;  
                    case 0x33:
                        OpcodeFX33(opcode);
                        break;
                    case 0x55:
                        OpcodeFX55(opcode);
                        break;  
                    case 0x65:
                        OpcodeFX65(opcode);
                        break;  
                }
                break;
                
        }
        // display_screen();
    }
    ProgramRunning = false;
    timer_thread.join();
    key_update.join();
    printf("Emulator exited\n");
} 


int main(int argc , char** argv){
    if (argc == 1){
        std::cout << "Please provide a chip-8 program file to load in." << std::endl;
        std::cout << "Usage: ./chip8 <game_file>" << std::endl;
        return 0;
    }

    if (SDL_Init(SDL_INIT_EVERYTHING)!=0){
		printf( "SDL could not initialize! SDL_Error: %s\n", SDL_GetError() );
		return 0;
	}
    window = SDL_CreateWindow("CHIP-8",
										  SDL_WINDOWPOS_CENTERED,
										  SDL_WINDOWPOS_CENTERED,
                                          HEIGHT * SCALE,
										  WIDTH * SCALE,
										  SDL_WINDOW_SHOWN);
    if (window == NULL){
		printf( "Window could not be created! SDL_Error: %s\n", SDL_GetError() );
		return 0 ;
	}
    screenSurface = SDL_GetWindowSurface( window );
	SDL_FillRect( screenSurface, NULL, SDL_MapRGB( screenSurface->format, 0xFF, 0xFF, 0xFF));

    RunEmulator(argv[1]);

    printf("Program exited successfully\n");

    return 0;
}