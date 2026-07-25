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
