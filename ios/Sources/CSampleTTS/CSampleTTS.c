#include "CSampleTTS.h"
#include "../../../c/b32emu.c"
#include "../../../c/sonic/sonic.c"
#include <stdlib.h>

void* CSampleTTS_init(const void* dll_bytes, size_t dll_len) {
    char errbuf[256];
    b32_emu* e = b32_open(dll_bytes, dll_len, errbuf, sizeof(errbuf));
    return e;
}

void CSampleTTS_destroy(void* engine) {
    if (engine) b32_close((b32_emu*)engine);
}

void CSampleTTS_set_rate(void* engine, int rate) {
    if (engine) b32_set_rate((b32_emu*)engine, rate);
}

void CSampleTTS_set_pitch(void* engine, int percent) {
    if (engine) b32_set_pitch((b32_emu*)engine, percent);
}

void CSampleTTS_set_voice(void* engine, int inflection, int head_size, int excitation, int unvoiced) {
    if (engine) b32_set_voice_params((b32_emu*)engine, inflection, head_size, excitation, unvoiced);
}

void CSampleTTS_set_speed(void* engine, float speed) {
    if (engine) b32_set_speed((b32_emu*)engine, speed);
}

int16_t* CSampleTTS_speak(void* engine, const char* text, size_t* out_bytes) {
    if (!engine) return NULL;
    int res = b32_speak((b32_emu*)engine, text, NULL, NULL);
    if (res != 0) return NULL;
    return b32_take_audio((b32_emu*)engine, out_bytes);
}
