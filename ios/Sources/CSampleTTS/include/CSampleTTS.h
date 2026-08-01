#ifndef CSampleTTS_h
#define CSampleTTS_h

#include <stdint.h>
#include <stddef.h>

void* CSampleTTS_init(const void* dll_bytes, size_t dll_len);
void CSampleTTS_destroy(void* engine);

// Set parameters
void CSampleTTS_set_rate(void* engine, int rate);
void CSampleTTS_set_pitch(void* engine, int percent);
void CSampleTTS_set_voice(void* engine, int inflection, int head_size, int excitation, int unvoiced);
void CSampleTTS_set_speed(void* engine, float speed);

// Speak and return dynamically allocated PCM (caller must free with free())
int16_t* CSampleTTS_speak(void* engine, const char* text, size_t* out_bytes);

#endif /* CSampleTTS_h */
