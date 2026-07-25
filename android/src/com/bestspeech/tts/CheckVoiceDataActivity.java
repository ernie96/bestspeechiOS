package com.bestspeech.tts;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.speech.tts.TextToSpeech;

import java.io.File;
import java.util.ArrayList;

/**
 * Answers the system's CHECK_TTS_DATA probe.
 *
 * <p>There is no downloadable voice data: the voices live inside the
 * user-supplied B32_TTS.DLL, so "available" simply means that file is present.
 */
public class CheckVoiceDataActivity extends Activity {

    private static final String DLL_NAME = "B32_TTS.DLL";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);

        boolean present = false;
        File[] roots = { getExternalFilesDir(null), getFilesDir() };
        for (File root : roots) {
            if (root != null && new File(root, DLL_NAME).isFile()) {
                present = true;
                break;
            }
        }

        ArrayList<String> available = new ArrayList<>();
        ArrayList<String> unavailable = new ArrayList<>();
        (present ? available : unavailable).add("eng-USA");

        Intent result = new Intent();
        result.putStringArrayListExtra(
                TextToSpeech.Engine.EXTRA_AVAILABLE_VOICES, available);
        result.putStringArrayListExtra(
                TextToSpeech.Engine.EXTRA_UNAVAILABLE_VOICES, unavailable);
        setResult(present ? TextToSpeech.Engine.CHECK_VOICE_DATA_PASS
                          : TextToSpeech.Engine.CHECK_VOICE_DATA_FAIL,
                  result);
        finish();
    }
}
