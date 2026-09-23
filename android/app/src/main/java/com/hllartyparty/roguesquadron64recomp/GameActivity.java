package com.hllartyparty.roguesquadron64recomp;

import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;
import java.io.File;
import org.libsdl.app.SDLActivity;

public final class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL2","RogueSquadron64Recomp"};
    }

    @Override
    protected String[] getArguments() {
        File program = new File(getFilesDir(), "program");
        return new String[]{
            "--android-data-dir=" + getFilesDir().getAbsolutePath(),
            "--android-program-dir=" + program.getAbsolutePath()
        };
    }

    @Override
    protected void onCreate(Bundle state) {
        new File(getFilesDir(), "program").mkdirs();
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_FULLSCREEN |
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }
}
