#ifndef CBestSpeech_h
#define CBestSpeech_h

#include <stdint.h>
#include <stddef.h>

void* CBestSpeech_init(const void* dll_bytes, size_t dll_len);
void CBestSpeech_destroy(void* engine);

// Set parameters
void CBestSpeech_set_rate(void* engine, int rate);
void CBestSpeech_set_pitch(void* engine, int percent);
void CBestSpeech_set_voice(void* engine, int inflection, int head_size, int excitation, int unvoiced);
void CBestSpeech_set_speed(void* engine, float speed);

// Speak and return dynamically allocated PCM (caller must free with free())
int16_t* CBestSpeech_speak(void* engine, const char* text, size_t* out_bytes);

#endif /* CBestSpeech_h */
