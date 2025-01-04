#include <stdio.h>
#include <SDL.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <io.h>
#include "constants.h"

typedef struct {
	int pixel[PIXEL_COUNT];
} Screen_Context;

typedef struct {
	uint8_t r;
	uint8_t g;
	uint8_t b;
} Screen_Color;

typedef struct {
	// MEMORY
	uint8_t memory[MEMORY_SIZE];
	uint16_t stack[STACK_SIZE];
	
	// REGISTERS
	uint8_t registers[REGISTER_COUNT];
	uint16_t memory_register;
	
	// TIMERS
	uint8_t delay_timer;
	uint8_t sound_timer;

	// SPECIAL REGISTERS
	uint8_t stack_pointer;
	uint16_t program_counter;

	uint16_t controller;
	uint16_t controller_prev;
} Chip_Context;

typedef struct {
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_AudioDeviceID* audio;
	SDL_AudioSpec audio_spec;
	SDL_AudioSpec audio_have;
} sdl_t;

sdl_t sdl;
Chip_Context* emulator;
Screen_Context* screen;

Screen_Color active_color = {
	.r = 255,
	.g = 255,
	.b = 255
};
Screen_Color inactive_color = {
	.r = 0,
	.g = 0,
	.b = 0
};

uint32_t next_draw;
int program_running = FALSE;

void audio_callback(void* userdata, uint8_t* stream, int len) {
	(void)userdata;
	
	int16_t* audio_buffer = (int16_t*)stream;
	static uint32_t running_sample_index = 0;
	const int32_t square_wave_period = SOUND_SAMPLE_RATE / SOUND_FREQ;
	const int32_t half_square_wave_period = square_wave_period / 2;

	for (int i = 0; i < len / 2; i++) {
		audio_buffer[i] = ((running_sample_index++ / half_square_wave_period) % 2) ? 1500 : -1500;
	}
}

int init_window() {
	if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
		fprintf(stderr, "Error initializing SDL.\n");
		return FALSE;
	}

	sdl.window = SDL_CreateWindow(
		WINDOW_TITLE,
		SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED,
		WINDOW_WIDTH,
		WINDOW_HEIGHT,
		0
	);
	if (!sdl.window) {
		fprintf(stderr, "Error creating SDL Window.\n");
		return FALSE;
	}

	sdl.renderer = SDL_CreateRenderer(sdl.window, -1, 0);
	if (!sdl.renderer) {
		fprintf(stderr, "Error creating SDL Renderer.\n");
		return FALSE;
	}

	sdl.audio_spec = (SDL_AudioSpec){
		.freq = SOUND_SAMPLE_RATE,
		.format = AUDIO_S16LSB,
		.channels = 1,
		.samples = 4096,
		.callback = audio_callback
	};

	sdl.audio = SDL_OpenAudioDevice(NULL, 0, &sdl.audio_spec, &sdl.audio_have, 0);
	if (!sdl.audio) {
		fprintf(stderr, "Error creating SDL Audio.\n");
		return FALSE;
	}

	return TRUE;
}

int load_game(char* game_rom_path) {

	FILE* game_rom = fopen(game_rom_path, "rb");

	if (!game_rom) {
		fprintf(stderr, "Error using specified ROM file.\n");
		return FALSE;
	}

	uint8_t buffer[MEMORY_SIZE - MEMORY_PROGRAM_START];

	while (!feof(game_rom)) {
		fread(buffer, sizeof(buffer), 1, game_rom);
	}

	for (int i = 0; i < MEMORY_SIZE - MEMORY_PROGRAM_START; i++) {
		emulator->memory[i + MEMORY_PROGRAM_START] = buffer[i];
	}

	fclose(game_rom);

	return TRUE;
}

int setup(char* game_rom_path) {
	srand(time(NULL));

	const uint8_t CHARACTER_SET[CHARACTER_SET_SIZE] = {
		/* 0 */ 0xF0, 0x90, 0x90, 0x90, 0xF0,
		/* 1 */ 0x20, 0x60, 0x20, 0x20, 0x70,
		/* 2 */ 0xF0, 0x10, 0xF0, 0x80, 0xF0,
		/* 3 */ 0xF0, 0x10, 0xF0, 0x10, 0xF0,
		/* 4 */ 0x90, 0x90, 0xF0, 0x10, 0x10,
		/* 5 */ 0xF0, 0x80, 0xF0, 0x10, 0xF0,
		/* 6 */ 0xF0, 0x80, 0xF0, 0x90, 0xF0,
		/* 7 */ 0xF0, 0x10, 0x20, 0x40, 0x40,
		/* 8 */ 0xF0, 0x90, 0xF0, 0x90, 0xF0,
		/* 9 */ 0xF0, 0x90, 0xF0, 0x10, 0xF0,
		/* A */ 0xF0, 0x90, 0xF0, 0x90, 0x90,
		/* B */ 0xE0, 0x90, 0xE0, 0x90, 0xE0,
		/* C */ 0xF0, 0x80, 0x80, 0x80, 0xF0,
		/* D */ 0xE0, 0x90, 0x90, 0x90, 0xE0,
		/* E */ 0xF0, 0x80, 0xF0, 0x80, 0xF0,
		/* F */ 0xF0, 0x80, 0xF0, 0x80, 0x80
	};

	emulator = (Chip_Context*)malloc(sizeof(Chip_Context));
	screen = (Screen_Context*)malloc(sizeof(Screen_Context));
	
	for (int i = 0; i < PIXEL_COUNT; i++) screen->pixel[i] = FALSE;

	for (int i = 0; i < MEMORY_SIZE; i++) emulator->memory[i] = i < CHARACTER_SET_SIZE ? CHARACTER_SET[i] : 0;
	for (int i = 0; i < STACK_SIZE; i++) emulator->stack[i] = 0;
	for (int i = 0; i < REGISTER_COUNT; i++) emulator->registers[i] = 0;
	emulator->memory_register = 0;
	emulator->delay_timer = 0;
	emulator->sound_timer = 0;
	emulator->stack_pointer = 0;
	emulator->program_counter = MEMORY_PROGRAM_START;
	emulator->controller = 0;
	emulator->controller_prev = 0;

	return load_game(game_rom_path);
}

int execute_instruction() {
	uint16_t inst = (uint16_t)(emulator->memory[emulator->program_counter] << 8) | (uint16_t)emulator->memory[emulator->program_counter + 1];
	uint16_t init_pc = emulator->program_counter;

	uint8_t inst_section[4] = {
		(inst & 0xf000) >> 12,
		(inst & 0x0f00) >> 8,
		(inst & 0x00f0) >> 4,
		(inst & 0x000f)
	};

	int valid_instruction = TRUE;
	int inst_increment = 2;

	int x, y, spr, pressed_key, temp;
	int bcd_values[3];
	switch (inst_section[0]) {
		case 0x0:
			if (inst_section[2] == 0xE) {
				switch (inst_section[3]) {
					// 0x00E0: CLS (clear display)	
					case 0x0:
						for (int i = 0; i < PIXEL_COUNT; i++) screen->pixel[i] = FALSE;
						break;
					// 0x00EE: RET (return to stack)
					case 0xE:
						emulator->stack_pointer = (emulator->stack_pointer - 1) % STACK_SIZE;
						emulator->program_counter = emulator->stack[emulator->stack_pointer];
						break;
					default:
						valid_instruction = FALSE;
						break;
				}
			} else valid_instruction = FALSE;
			break;
		// 0x1nnn: JMP addr (jumps to address at nnn)
		case 0x1:
			emulator->program_counter = (inst & 0xfff);
			inst_increment = 0;
			break;
		// 0x2nnn: CALL addr (push PC to stack and call nnn)
		case 0x2:
			emulator->stack[emulator->stack_pointer] = emulator->program_counter;
			emulator->stack_pointer = (emulator->stack_pointer + 1) % STACK_SIZE;
			emulator->program_counter = (inst & 0xfff);
			inst_increment = 0;
			break;
		// 0x3xkk: SE Vx, byte (skip next instruction if Vx == kk)
		case 0x3:
			if (emulator->registers[inst_section[1]] == (inst & 0xff)) inst_increment += 2;
			break;
		// 0x4xkk: SNE Vx, byte (skip next instruction if Vx != kk)
		case 0x4:
			if (emulator->registers[inst_section[1]] != (inst & 0xff)) inst_increment += 2;
			break;
		case 0x5:
			// 0x5xy0: SE Vx, Vy (skip next instruction if Vx == Vy)
			if (inst_section[3] == 0x0) {
				if (emulator->registers[inst_section[1]] == emulator->registers[inst_section[2]]) inst_increment += 2;
			} else valid_instruction = FALSE;
			break;
		// 0x6xkk: LD Vx, byte (set Vx equal to kk)
		case 0x6:
			emulator->registers[inst_section[1]] = (inst & 0xff);
			break;
		// 0x7xkk: ADD Vx, byte (add kk to Vx)
		case 0x7:
			emulator->registers[inst_section[1]] += (inst & 0xff);
			break;
		case 0x8:
			switch (inst_section[3]) {
				// 0x8xy0: LD Vx, Vy (set Vx to Vy)
				case 0x0:
					emulator->registers[inst_section[1]] = emulator->registers[inst_section[2]];
					break;
				// 0x8xy1: OR Vx, Vy (set Vx to Vx | Vy)
				case 0x1:
					emulator->registers[inst_section[1]] |= emulator->registers[inst_section[2]];
					emulator->registers[0xF] = FALSE;
					break;
				// 0x8xy2: AND Vx, Vy (set Vx to Vx & Vy)
				case 0x2:
					emulator->registers[inst_section[1]] &= emulator->registers[inst_section[2]];
					emulator->registers[0xF] = FALSE;
					break;
				// 0x8xy3: XOR Vx, Vy (set Vx to Vx ^ Vy)
				case 0x3:
					emulator->registers[inst_section[1]] ^= emulator->registers[inst_section[2]];
					emulator->registers[0xF] = FALSE;
					break;
				// 0x8xy4: ADD Vx, Vy (set Vx to Vx + Vy, VF true if carry)
				case 0x4:
					temp = emulator->registers[inst_section[1]] + emulator->registers[inst_section[2]];
					emulator->registers[inst_section[1]] = temp;
					emulator->registers[0xF] = (temp > 255);
					break;
				// 0x8xy5: SUB Vx, Vy (set Vx to Vx - Vy, VF true if no carry)
				case 0x5:
					temp = emulator->registers[inst_section[1]] - emulator->registers[inst_section[2]];
					emulator->registers[inst_section[1]] = temp;
					emulator->registers[0xF] = (temp >= 0);
					break;
				// 0x8xy6: SHR Vx (set Vx to Vx >> 1, VF true if bit shifted out)
				case 0x6:
					temp = emulator->registers[inst_section[1]] & 0x1;
					emulator->registers[inst_section[1]] = emulator->registers[inst_section[2]] >> 1;
					emulator->registers[0xF] = (temp > 0);
					break;
				// 0x8xy7: SUBN Vx, Vy (set Vx to Vy - Vx, VF true if no carry)
				case 0x7:
					temp = emulator->registers[inst_section[2]] - emulator->registers[inst_section[1]];
					emulator->registers[inst_section[1]] = temp;
					emulator->registers[0xF] = (temp >= 0);
					break;
				// 0x8xyE: SHL Vx (set Vx to Vx << 1, VF true if bit shifted out)
				case 0xE:
					temp = emulator->registers[inst_section[1]] & 0x80;
					emulator->registers[inst_section[1]] = emulator->registers[inst_section[2]] << 1;
					emulator->registers[0xF] = (temp > 0);
					break;
				default:
					valid_instruction = FALSE;
					break;
			}
			break;
		case 0x9:
			// 0x9xy0: SNE Vx, Vy (skip next instruction if Vx != Vy)
			if (inst_section[3] == 0x0) {
				if (emulator->registers[inst_section[1]] != emulator->registers[inst_section[2]]) inst_increment += 2;
			} else valid_instruction = FALSE;
			break;
		// 0xAnnn: LD I, addr
		case 0xA:
			emulator->memory_register = (inst & 0xfff);
			break;
		// 0xBnnn: JMP V0, addr (jump to address + V0)
		case 0xB:
			emulator->program_counter = (inst & 0xfff) + emulator->registers[0];
			inst_increment = 0;
			break;
		// 0xCxkk: RND Vx, byte (set Vx to random & byte)
		case 0xC:
			emulator->registers[inst_section[1]] = rand() & (inst & 0xff);
			break;
		// 0xDxyn Vx, Vy, nibble
		case 0xD:
			emulator->registers[0xF] = FALSE;

			x = emulator->registers[inst_section[1]] % SCREEN_WIDTH;
			y = emulator->registers[inst_section[2]] % SCREEN_HEIGHT;
			for (int i = 0; i < inst_section[3]; i++) {
				if (y + i >= SCREEN_HEIGHT) break;
				spr = emulator->memory[emulator->memory_register + i];
				for (int j = 0; j < 8; j++) {
					if (x + j >= SCREEN_WIDTH) break;
					if (screen->pixel[SCREEN_WIDTH * (y + i) + (x + j)] && (((spr >> (7 - j)) & 0x1) == 1)) emulator->registers[0xF] = TRUE;
					screen->pixel[SCREEN_WIDTH * (y + i) + (x + j)] ^= (((spr >> (7 - j)) & 0x1) == 1);
				}
			}
			break;
		case 0xE:
			switch (inst & 0xff) {
				// 0xEx9E: SKP Vx (skip next instruction if key with value Vx is pressed)
				case 0x9E:
					if (((emulator->controller >> (emulator->registers[inst_section[1]])) & 0x1)) inst_increment += 2;
					break;
				// 0xExA1: SKNP Vx (skip next instruction if key with value Vx is not pressed)
				case 0xA1:
					if (((emulator->controller >> (emulator->registers[inst_section[1]])) & 0x1) == 0) inst_increment += 2;
					break;
				default:
					valid_instruction = FALSE;
					break;
			}
			break;
		case 0xF:
			switch (inst & 0xff) {
				// 0xFx07: LD Vx, DT (set Vx to Delay Timer)
				case 0x07:
					emulator->registers[inst_section[1]] = emulator->delay_timer;
					break;
				// 0xFx0A: LD Vx, K (wait for key press, store in Vx)
				case 0x0A:
					if (emulator->controller) {
						for (pressed_key = 0; pressed_key < KEYBOARD_COUNT; pressed_key++) {
							if (((emulator->controller_prev >> pressed_key) & 0x1) && !((emulator->controller >> pressed_key) & 0x1)) break;
						}
						emulator->registers[inst_section[1]] = pressed_key;
					} else {
						inst_increment = 0;
					}
					break;
				// 0xFx15: LD DT, Vx (set Delay Timer to Vx)
				case 0x15:
					emulator->delay_timer = emulator->registers[inst_section[1]];
					break;
				// 0xF18: LD ST, Vx (set Sound Timer to Vx)
				case 0x18:
					emulator->sound_timer = emulator->registers[inst_section[1]];
					break;
				// 0xFx1E: ADD I, Vx (set I to I + Vx)
				case 0x1E:
					emulator->memory_register += emulator->registers[inst_section[1]];
					break;
				// 0xFx29: LD F, Vx (set I to number sprite location)
				case 0x29:
					emulator->memory_register = (emulator->registers[inst_section[1]] % 0x10) * 5;
					break;
				// 0xFx33: LD B, Vx (store BCD values of Vx into I, I+1, I+2)
				case 0x33:
					bcd_values[0] = emulator->registers[inst_section[1]] / 100;
					bcd_values[1] = (emulator->registers[inst_section[1]] / 10) % 10;
					bcd_values[2] = emulator->registers[inst_section[1]] % 10;

					for (int i = 0; i < 3; i++) emulator->memory[emulator->memory_register + i] = bcd_values[i];
					break;
				// 0xFx55: LD [I], Vx (store registers V0 to Vx into I through I + x)
				case 0x55:
					for (int i = 0; i <= inst_section[1]; i++) emulator->memory[emulator->memory_register + i] = emulator->registers[i];
					emulator->memory_register += inst_section[1] + 1;
					break;
				// 0xFx65: LD Vx, [I] (store memory I through I + x into V0 to Vx)
				case 0x65:
					for (int i = 0; i <= inst_section[1]; i++) emulator->registers[i] = emulator->memory[emulator->memory_register + i];
					emulator->memory_register += inst_section[1] + 1;
					break;
				default:
					valid_instruction = FALSE;
					break;
			}
			break;
		default:
			valid_instruction = FALSE;
			break;
	}

	if (!valid_instruction) {
		fprintf(stderr, "Unsupported instruction \"0x%04x\" (%01x %01x %01x %01x) found at 0x%08x.\n", inst, inst_section[0], inst_section[1], inst_section[2], inst_section[3], init_pc);
	}

	emulator->program_counter += inst_increment;

	return valid_instruction;
}

void run_timers() {
	if (emulator->delay_timer > 0) emulator->delay_timer--;
	if (emulator->sound_timer > 0) {
		SDL_PauseAudioDevice(sdl.audio, FALSE);
		emulator->sound_timer--;
	} else {
		SDL_PauseAudioDevice(sdl.audio, TRUE);
	}
}

uint32_t draw_time() {
	uint32_t now = SDL_GetTicks();
	if (next_draw <= now) return 0;
	else return next_draw - now;
}

void render() {
	SDL_SetRenderDrawColor(
		sdl.renderer,
		inactive_color.r,
		inactive_color.g,
		inactive_color.b,
		255
	);
	SDL_RenderClear(sdl.renderer);

	SDL_SetRenderDrawColor(
		sdl.renderer,
		active_color.r,
		active_color.g,
		active_color.b,
		255
	);
	SDL_Rect current_pixel;
	for (int i = 0; i < PIXEL_COUNT; i++) {
		if (screen->pixel[i]) {
			current_pixel = (SDL_Rect){
				.x = (i % SCREEN_WIDTH) * PIXEL_WIDTH,
				.y = (i / SCREEN_WIDTH) * PIXEL_HEIGHT,
				.w = PIXEL_WIDTH,
				.h = PIXEL_HEIGHT
			};
			SDL_RenderFillRect(sdl.renderer, &current_pixel);
		}
	}

	SDL_RenderPresent(sdl.renderer);
}

void destroy_window() {
	free(screen);
	free(emulator);

	SDL_DestroyRenderer(sdl.renderer);
	SDL_DestroyWindow(sdl.window);
	SDL_Quit();
}

void get_input() {
	SDL_Event event;
	SDL_PollEvent(&event);

	switch (event.type) {
		case SDL_QUIT:
			program_running = FALSE;
			break;
		case SDL_KEYDOWN:
			emulator->controller_prev = emulator->controller;
			switch (event.key.keysym.sym) {
				case SDLK_ESCAPE:
					program_running = FALSE;
					break;
				case SDLK_1: emulator->controller |= 0x0002; break;
				case SDLK_2: emulator->controller |= 0x0004; break;
				case SDLK_3: emulator->controller |= 0x0008; break;
				case SDLK_4: emulator->controller |= 0x1000; break;
				case SDLK_q: emulator->controller |= 0x0010; break;
				case SDLK_w: emulator->controller |= 0x0020; break;
				case SDLK_e: emulator->controller |= 0x0040; break;
				case SDLK_r: emulator->controller |= 0x2000; break;
				case SDLK_a: emulator->controller |= 0x0080; break;
				case SDLK_s: emulator->controller |= 0x0100; break;
				case SDLK_d: emulator->controller |= 0x0200; break;
				case SDLK_f: emulator->controller |= 0x4000; break;
				case SDLK_z: emulator->controller |= 0x0400; break;
				case SDLK_x: emulator->controller |= 0x0001; break;
				case SDLK_c: emulator->controller |= 0x0800; break;
				case SDLK_v: emulator->controller |= 0x8000; break;
			}
			break;
		case SDL_KEYUP:
			emulator->controller_prev = emulator->controller;
			switch (event.key.keysym.sym) {
				case SDLK_1: emulator->controller &= ~0x0002; break;
				case SDLK_2: emulator->controller &= ~0x0004; break;
				case SDLK_3: emulator->controller &= ~0x0008; break;
				case SDLK_4: emulator->controller &= ~0x1000; break;
				case SDLK_q: emulator->controller &= ~0x0010; break;
				case SDLK_w: emulator->controller &= ~0x0020; break;
				case SDLK_e: emulator->controller &= ~0x0040; break;
				case SDLK_r: emulator->controller &= ~0x2000; break;
				case SDLK_a: emulator->controller &= ~0x0080; break;
				case SDLK_s: emulator->controller &= ~0x0100; break;
				case SDLK_d: emulator->controller &= ~0x0200; break;
				case SDLK_f: emulator->controller &= ~0x4000; break;
				case SDLK_z: emulator->controller &= ~0x0400; break;
				case SDLK_x: emulator->controller &= ~0x0001; break;
				case SDLK_c: emulator->controller &= ~0x0800; break;
				case SDLK_v: emulator->controller &= ~0x8000; break;
			}
			break;
	}
}

int main(int argc, char* args[]) {
	program_running = TRUE;

	if (argc == 2) program_running &= setup(args[1]);
	else {
		char rom_path[512];
		printf("Enter ROM file: ");
		if (fgets(rom_path, sizeof(rom_path), stdin)) {
			rom_path[strcspn(rom_path, "\n")] = 0;
			program_running &= setup(rom_path);
		} else {
			program_running = FALSE;
		}
	}

	program_running &= init_window();

	if (!program_running) {
		printf("Something went wrong, please review any errors. Press Enter to terminal the program...\n");
		fgetc(stdin);
		return -1;
	}

	double t, display_last, display_delta;
	int clock_count = 0;
	display_last = SDL_GetTicks();
	while (program_running) {
		t = SDL_GetTicks();
		display_delta = t - display_last;
		get_input();

		if (display_delta > 1000 / FPS) {
			SDL_Delay(display_delta - (1000 / FPS));
			for(int i = 0;i < 10;i++) execute_instruction();
			render();
			display_last = t;
			run_timers();
		}
	}

	destroy_window();

	return 0;
}