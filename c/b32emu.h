/* b32emu -- run the BestSpeech/Keynote Gold B32_TTS.DLL (win32 x86, 1995)
 * under the Unicorn CPU emulator and capture synthesized PCM.
 *
 * Portable C99: depends only on libc and unicorn, so the same sources build
 * for Windows, Linux and Android. Nothing here calls a Windows API -- the
 * emulator is the operating system as far as the guest DLL is concerned.
 *
 * Public domain, matching the b32tts_wrapper project this derives from.
 */
#ifndef B32EMU_H
#define B32EMU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The engine always produces this format. */
#define B32_SAMPLE_RATE 11025
#define B32_CHANNELS    1
#define B32_BITS        16

/* bstSetParams selectors (from b32tts_wrapper / @rommix0's bst.h) */
#define BST_RATE_SETTING      257
#define BST_GAIN_SETTING      258
#define BST_BIT_DEPTH_SETTING 4097

typedef struct b32_emu b32_emu;

/* Called with each PCM chunk as the engine emits it. Return 0 to abort
 * synthesis, non-zero to continue. May be NULL, in which case audio is
 * accumulated internally and retrieved with b32_take_audio(). */
typedef int (*b32_audio_cb)(const int16_t *pcm, size_t bytes, void *user);

/* One voice: a display name plus the engine parameters that define it. Stored
 * as separate values rather than a ready-made prefix string because the codes
 * are order-sensitive -- ~f discards a preceding ~h -- so the prefix has to be
 * assembled in one place with any caller overrides folded in. */
typedef struct {
    const char *name;
    int head_size;           /* ~v */
    int excitation;          /* ~e */
    int inflection;          /* ~h */
    int unvoiced;            /* ~u */
    int base_hz;             /* ~f */
    int rate;                /* ~r, the engine's own sign: lower is faster */
} b32_voice;

const b32_voice *b32_voices(int *count);
/* Case-insensitive lookup; returns NULL if unknown. */
const b32_voice *b32_voice_by_name(const char *name);

/* Load and initialize. dll_bytes/dll_len is the raw B32_TTS.DLL image; it is
 * copied, so the caller may free it immediately. Returns NULL on failure and
 * writes a message to errbuf. */
b32_emu *b32_open(const void *dll_bytes, size_t dll_len,
                  char *errbuf, size_t errlen);
void b32_close(b32_emu *e);

/* Engine parameters. rate follows the b32tts_wrapper convention: higher is
 * faster (it is negated internally). gain is -70..20.
 *
 * rate is applied via the engine's ~r text code by b32_speak_voice, because
 * bstSetParams(BST_RATE_SETTING) only affects the first utterance after the
 * engine is created. It therefore has no effect on plain b32_speak(), which
 * sends text verbatim -- put your own ~r code in the text for that. */
void b32_set_rate(b32_emu *e, int rate);
void b32_set_gain(b32_emu *e, int gain);

/* Leave a voice-quality parameter at the selected voice's own value. */
#define B32_VOICE_DEFAULT (-32768)

/* The rest of the engine's voice-quality parameters, from the Keynote GOLD
 * command set. Like rate these travel as text codes, so they apply to every
 * utterance. Pass B32_VOICE_DEFAULT for any the voice should decide.
 *
 *   inflection  ~h  -300..100  pitch range; lower is more monotone. Saturates
 *                              below about -150, and a high baseline frequency
 *                              clamps the top of the range as well.
 *   head_size   ~v  0..6       vocal tract size, in no particular order
 *   excitation  ~e  1..6       1 breathy, 2 whispery, 3 normal
 *   unvoiced    ~u  -70..20    gain of unvoiced sound; low sounds blocked-up
 */
void b32_set_voice_params(b32_emu *e, int inflection, int head_size,
                          int excitation, int unvoiced);

/* Baseline pitch as a percentage of the selected voice's own frequency, so a
 * setting survives a change of voice. Clamped to the engine's 43..600 Hz. */
void b32_set_pitch(b32_emu *e, int percent);

/* Text-parser options (the engine's ~nN commands). Each is off in this DLL
 * revision whatever the Keynote GOLD manual says, and every one of these was
 * confirmed to change the audio; ~n5 (mathematics) and ~n8 (control
 * characters) are documented but inert here, so they are deliberately absent. */
#define B32_PARSE_LETTER_NAMES (1u << 1)  /* ~n1   spell letters out         */
#define B32_PARSE_DIGITS       (1u << 2)  /* ~n2   digits individually       */
#define B32_PARSE_PUNCTUATION  (1u << 3)  /* ~n3   speak commas, periods     */
#define B32_PARSE_WHITESPACE   (1u << 4)  /* ~n4   speak space, tab, return  */
#define B32_PARSE_FULL_NUMBERS (1u << 6)  /* ~n6   no grouping of digits     */
#define B32_PARSE_UPPER_WORDS  (1u << 7)  /* ~n7   capital groups as words   */
#define B32_PARSE_TIMES        (1u << 9)  /* ~n9   8:00 as "eight o'clock"   */
#define B32_PARSE_ABBREV       (1u << 10) /* ~n10  expand Dr., St., Mt. ...  */
void b32_set_parse_flags(b32_emu *e, unsigned flags);

/* Phrase prediction. The parser normally looks ahead for punctuation before
 * committing to a phrase, which is what gives an utterance its intonation
 * contour; 0 makes it speak text as it arrives instead (~~2,1]). */
void b32_set_phrase_prediction(b32_emu *e, int on);

/* Trim silence out of the synthesized audio.
 *
 * interior_ms caps any run of silence inside the utterance -- the engine's
 * sentence pause is about 420 ms and its comma pause about 180 ms, which is a
 * long wait at screen-reader speeds. 0 leaves them alone.
 *
 * tail_ms caps the silence at the very end. The engine appends about 477 ms of
 * it to every utterance, delaying whatever the caller queues next, so this is
 * capped by default rather than only on request. */
void b32_set_pause_caps(b32_emu *e, int interior_ms, int tail_ms);

/* Post-synthesis time stretch, applied to the captured audio via sonic.
 * 1.0 disables it. The engine's own rate parameter saturates around 2.7x
 * normal speed, so this is how you go faster (or slower) than it can manage
 * while keeping pitch intact. */
void b32_set_speed(b32_emu *e, float speed);
float b32_get_speed(const b32_emu *e);

/* Synthesize. text is engine-encoded (control codes already prefixed).
 * Returns 0 on success, non-zero on emulation failure. */
int b32_speak(b32_emu *e, const char *text, b32_audio_cb cb, void *user);

/* Convenience: prefix the voice's control codes, then speak. */
int b32_speak_voice(b32_emu *e, const b32_voice *v, const char *text,
                    b32_audio_cb cb, void *user);

/* Hand over the internally accumulated buffer (when cb was NULL). Ownership
 * transfers to the caller, which must free() it. Resets the internal buffer. */
int16_t *b32_take_audio(b32_emu *e, size_t *bytes);

/* Last error message from this instance ("" if none). */
const char *b32_error(const b32_emu *e);

/* Verbose tracing of shimmed API calls to stderr. */
void b32_set_trace(b32_emu *e, int on);

/* Write a 16-bit mono RIFF/WAVE file. Returns 0 on success. */
int b32_write_wav(const char *path, const int16_t *pcm, size_t bytes,
                  unsigned rate);

#ifdef __cplusplus
}
#endif
#endif /* B32EMU_H */
