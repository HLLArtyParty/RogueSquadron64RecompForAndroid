package com.hllartyparty.roguesquadron64recomp;

import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import org.libsdl.app.SDLActivity;

public final class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL2","RogueSquadron64Recomp"};
    }

    @Override
    protected String[] getArguments() {
        File program = new File(getFilesDir(), "program");
        String displayMode = getIntent().getStringExtra("display_mode");
        if (!"horplus".equals(displayMode)) displayMode = "normal";
        return new String[]{
            "--android-data-dir=" + getFilesDir().getAbsolutePath(),
            "--android-program-dir=" + program.getAbsolutePath(),
            "--display-mode=" + displayMode
        };
    }

    @Override
    protected void onCreate(Bundle state) {
        new File(getFilesDir(), "program").mkdirs();
        rotateRuntimeLog();
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        hideSystemUI();
    }

    private void rotateRuntimeLog() {
        File latest = new File(getFilesDir(), "roguesq-runtime.log");
        if (!latest.isFile() || latest.length() == 0) return;
        File previous = new File(getFilesDir(), "roguesq-runtime.previous.log");
        try (FileInputStream input = new FileInputStream(latest);
             FileOutputStream output = new FileOutputStream(previous, false)) {
            byte[] buffer = new byte[65536];
            for (int count; (count = input.read(buffer)) > 0;) {
                output.write(buffer, 0, count);
            }
        } catch (IOException error) {
            android.util.Log.w("RogueSquadron64", "Unable to preserve previous boot log", error);
            return;
        }
        try (FileOutputStream ignored = new FileOutputStream(latest, false)) {
            // The native logger opens this empty file in append mode for the new boot.
        } catch (IOException error) {
            android.util.Log.w("RogueSquadron64", "Unable to reset latest boot log", error);
        }
    }

    private void hideSystemUI() {
        View decorView = getWindow().getDecorView();
        int flags = View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
        decorView.setSystemUiVisibility(flags);
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUI();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUI();
    }
}
