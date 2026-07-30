package com.bestspeech.tts;

/**
 * Thin binding over the native BestSpeech engine running under Unicorn.
 *
 * <p>The native side holds a single emulated CPU per handle and is not
 * thread-safe; callers must serialize. {@link android.speech.tts.TextToSpeechService}
 * already serializes synthesis, so that costs nothing in practice.
 */
final class B32Native {

    static {
        System.loadLibrary("b32tts");
    }

    private B32Native() {}

    /** Receives PCM as the engine produces it, rather than after the utterance. */
    interface Sink {
        /**
         * @param pcm   16-bit little-endian mono samples at {@link #SAMPLE_RATE}
         * @param bytes valid prefix length of {@code pcm}
         * @return false to abandon the rest of the utterance
         */
        boolean onAudio(byte[] pcm, int bytes);
    }

    static final int SAMPLE_RATE = 11025;

    /**
     * @param dll raw bytes of a user-supplied B32_TTS.DLL
     * @return native handle, never 0 (a failure throws instead)
     */
    static native long open(byte[] dll);

    static native void close(long handle);

    /** Higher is faster. Roughly -50..50 is useful. */
    static native void setRate(long handle, int rate);

    /** Engine gain in dB, -70..20. */
    static native void setGain(long handle, int gain);

    /**
     * Post-synthesis time stretch (1.0 = off), pitch preserved. The engine's own
     * rate caps near 2.75x, so this is what reaches higher.
     */
    static native void setSpeed(long handle, float speed);

    /** Baseline pitch as a percentage of the selected voice's own frequency. */
    static native void setPitch(long handle, int percent);

    /** Leave a voice-quality parameter at the selected voice's own value. */
    static final int VOICE_DEFAULT = -32768;

    /**
     * The engine's remaining voice-quality parameters, any of which may be
     * {@link #VOICE_DEFAULT}.
     *
     * @param inflection pitch range, -300..100; lower is more monotone
     * @param headSize   vocal tract size, 0..6, in no particular order
     * @param excitation 1 breathy, 2 whispery, 3 normal, up to 6
     * @param unvoiced   gain of unvoiced sound, -70..20 dB
     */
    static native void setVoiceParams(long handle, int inflection, int headSize,
                                      int excitation, int unvoiced);

    // Text-parser options; mirror the B32_PARSE_* bits in b32emu.h.
    static final int PARSE_LETTER_NAMES = 1 << 1;   // spell letters out
    static final int PARSE_DIGITS       = 1 << 2;   // digits individually
    static final int PARSE_PUNCTUATION  = 1 << 3;   // speak commas, periods
    static final int PARSE_WHITESPACE   = 1 << 4;   // speak space, tab, return
    static final int PARSE_FULL_NUMBERS = 1 << 6;   // no grouping of digits
    static final int PARSE_UPPER_WORDS  = 1 << 7;   // capital groups as words
    static final int PARSE_TIMES        = 1 << 9;   // 8:00 as "eight o'clock"
    static final int PARSE_ABBREV       = 1 << 10;  // expand Dr., St., Mt.

    /**
     * @param parseFlags       {@code PARSE_*} bits; anything not set is turned off
     * @param phrasePrediction look ahead for punctuation before committing to a
     *                         phrase, which is what shapes the intonation
     */
    static native void setTextParams(long handle, int parseFlags,
                                     boolean phrasePrediction);

    /**
     * Caps silence runs in the output, in milliseconds; 0 for either disables
     * that cap. The engine pauses about 420 ms at a sentence and appends about
     * 477 ms to the end of every utterance.
     */
    static native void setPauseCaps(long handle, int interiorMs, int tailMs);

    static native int voiceCount();

    static native String voiceName(int index);

    /** The voice's default baseline pitch in Hz. */
    static native int voiceBaseHz(int index);

    /**
     * @param voice a name from {@link #voiceName}, or null to send {@code text}
     *              with no voice prefix applied
     * @return 0 on success
     */
    static native int speak(long handle, String text, String voice, Sink sink);
}
