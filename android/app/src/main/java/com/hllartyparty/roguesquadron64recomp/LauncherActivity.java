package com.hllartyparty.roguesquadron64recomp;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;

public final class LauncherActivity extends Activity {
    private static final int PICK_ROM = 1001;
    private static final int ROM_SIZE = 16 * 1024 * 1024;
    private static final String EXPECTED_SHA1 = "ed42eed1ee2db646ff7ef94ba8c5421d164a4f0d";
    private TextView status;

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(48, 48, 48, 48);

        TextView title = new TextView(this);
        title.setText("Rogue Squadron 64 Recompiled");
        title.setTextSize(26);
        status = new TextView(this);
        status.setText("Select your legally obtained Rogue Squadron (USA v1.0) ROM.");
        status.setPadding(0, 24, 0, 24);
        Button pick = new Button(this);
        pick.setText("Select ROM");
        pick.setOnClickListener(view -> pickRom());

        box.addView(title);
        box.addView(status);
        box.addView(pick);
        setContentView(box);

        File installed = installedRom();
        if (installed.isFile() && installed.length() == ROM_SIZE) {
            launchGame();
        }
    }

    private File installedRom() {
        return new File(new File(getFilesDir(), "program"), "rogue_squadron.z64");
    }

    private void pickRom() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, PICK_ROM);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_ROM || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        status.setText("Checking ROM…");
        new Thread(() -> importRom(uri)).start();
    }

    private void importRom(Uri uri) {
        try {
            byte[] raw;
            try (InputStream input = getContentResolver().openInputStream(uri);
                 ByteArrayOutputStream output = new ByteArrayOutputStream()) {
                if (input == null) throw new IOException("Unable to open selected file");
                byte[] buffer = new byte[65536];
                for (int count; (count = input.read(buffer)) > 0;) output.write(buffer, 0, count);
                raw = output.toByteArray();
            }
            byte[] z64 = normalize(raw);
            if (z64.length != ROM_SIZE) throw new IOException("Expected a 16 MiB USA v1.0 ROM");
            String actualSha1 = sha1(z64);
            if (!EXPECTED_SHA1.equals(actualSha1)) {
                throw new IOException("Wrong ROM revision (SHA-1 " + actualSha1 + ")");
            }
            File destination = installedRom();
            File parent = destination.getParentFile();
            if (parent != null) parent.mkdirs();
            try (FileOutputStream output = new FileOutputStream(destination)) {
                output.write(z64);
            }
            runOnUiThread(this::launchGame);
        } catch (Exception error) {
            runOnUiThread(() -> status.setText("ROM rejected: " + error.getMessage()));
        }
    }

    static byte[] normalize(byte[] data) throws IOException {
        if (data.length < 4) throw new IOException("File is too small");
        int magic = ((data[0] & 255) << 24) | ((data[1] & 255) << 16) |
                    ((data[2] & 255) << 8) | (data[3] & 255);
        if (magic == 0x80371240) return data;
        byte[] normalized = new byte[data.length];
        if (magic == 0x37804012) {
            for (int i = 0; i + 1 < data.length; i += 2) {
                normalized[i] = data[i + 1];
                normalized[i + 1] = data[i];
            }
            return normalized;
        }
        if (magic == 0x40123780) {
            for (int i = 0; i + 3 < data.length; i += 4) {
                normalized[i] = data[i + 3];
                normalized[i + 1] = data[i + 2];
                normalized[i + 2] = data[i + 1];
                normalized[i + 3] = data[i];
            }
            return normalized;
        }
        throw new IOException("Unknown N64 ROM byte order");
    }

    static String sha1(byte[] data) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-1").digest(data);
        StringBuilder text = new StringBuilder(digest.length * 2);
        for (byte value : digest) text.append(String.format("%02x", value));
        return text.toString();
    }

    private void launchGame() {
        startActivity(new Intent(this, GameActivity.class));
        finish();
    }
}
