package com.hllartyparty.roguesquadron64recomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.text.method.ScrollingMovementMethod;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

public final class LauncherActivity extends Activity {
    private static final int PICK_ROM = 1001;
    private static final int ROM_SIZE = 16 * 1024 * 1024;
    private static final int MAX_LOG_CHARS = 512 * 1024;
    private static final String EXPECTED_SHA1 = "ed42eed1ee2db646ff7ef94ba8c5421d164a4f0d";
    private TextView status;
    private Button normal;
    private Button widescreen;

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
        status.setPadding(0, 24, 0, 24);

        normal = new Button(this);
        normal.setText("Normal Boot");
        normal.setOnClickListener(view -> launchGame("normal"));

        widescreen = new Button(this);
        widescreen.setText("16:9 Boot (Experimental)");
        widescreen.setOnClickListener(view -> launchGame("horplus"));

        Button logs = new Button(this);
        logs.setText("Show Log");
        logs.setOnClickListener(view -> showLatestLog());

        Button pick = new Button(this);
        pick.setText("Choose Different ROM");
        pick.setOnClickListener(view -> pickRom());

        box.addView(title);
        box.addView(status);
        box.addView(normal);
        box.addView(widescreen);
        box.addView(logs);
        box.addView(pick);
        setContentView(box);
        refreshRomStatus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (status != null) refreshRomStatus();
    }

    private void refreshRomStatus() {
        File installed = installedRom();
        boolean ready = installed.isFile() && installed.length() == ROM_SIZE;
        normal.setEnabled(ready);
        widescreen.setEnabled(ready);
        status.setText(ready
            ? "ROM ready. Review the options below and launch the game."
            : "Select your legally obtained Rogue Squadron (USA v1.0) ROM.");
    }

    private File installedRom() {
        return new File(new File(getFilesDir(), "program"), "rogue_squadron.z64");
    }

    private File latestLog() {
        return new File(getFilesDir(), "roguesq-runtime.log");
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
            runOnUiThread(() -> {
                refreshRomStatus();
                status.setText("ROM accepted. Ready to play.");
            });
        } catch (Exception error) {
            runOnUiThread(() -> status.setText("ROM rejected: " + error.getMessage()));
        }
    }

    private String readLatestLog() {
        File log = latestLog();
        String prefix = "";
        if (!log.isFile() || log.length() == 0) {
            log = new File(getFilesDir(), "roguesq-runtime.previous.log");
            prefix = "[Latest boot produced no native output. Showing previous boot log.]\n\n";
        }
        if (!log.isFile() || log.length() == 0) {
            return "No boot log is available yet. Launch the game once, then return here.";
        }
        try (RandomAccessFile input = new RandomAccessFile(log, "r")) {
            long start = Math.max(0, input.length() - MAX_LOG_CHARS);
            input.seek(start);
            byte[] bytes = new byte[(int)(input.length() - start)];
            input.readFully(bytes);
            String text = new String(bytes, StandardCharsets.UTF_8);
            if (start > 0) prefix += "[Earlier log output omitted]\n\n";
            return prefix + text;
        } catch (IOException error) {
            return "Unable to read latest boot log: " + error.getMessage();
        }
    }

    private void showLatestLog() {
        String logText = readLatestLog();
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        int pad = 24;
        content.setPadding(pad, pad, pad, pad);

        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        Button copy = new Button(this);
        copy.setText("Copy All");
        copy.setOnClickListener(view -> copyLog(logText));
        Button share = new Button(this);
        share.setText("Share Log");
        share.setOnClickListener(view -> shareLog());
        actions.addView(copy, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        actions.addView(share, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));

        TextView text = new TextView(this);
        text.setText(logText);
        text.setTextIsSelectable(true);
        text.setHorizontallyScrolling(true);
        text.setMovementMethod(new ScrollingMovementMethod());
        ScrollView scroll = new ScrollView(this);
        scroll.addView(text);

        content.addView(actions);
        content.addView(scroll, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        new AlertDialog.Builder(this)
            .setTitle("Latest Boot Log")
            .setView(content)
            .setPositiveButton("Close", null)
            .show();
    }

    private void copyLog(String text) {
        ClipboardManager clipboard = (ClipboardManager)getSystemService(Context.CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(ClipData.newPlainText("Rogue Squadron boot log", text));
        Toast.makeText(this, "Log copied", Toast.LENGTH_SHORT).show();
    }

    private void shareLog() {
        Uri logUri = Uri.parse("content://" + getPackageName() + ".logs/latest");
        Intent share = new Intent(Intent.ACTION_SEND);
        share.setType("text/plain");
        share.putExtra(Intent.EXTRA_SUBJECT, "Rogue Squadron 64 latest boot log");
        share.putExtra(Intent.EXTRA_STREAM, logUri);
        share.setClipData(ClipData.newRawUri("Rogue Squadron boot log", logUri));
        share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivity(Intent.createChooser(share, "Share latest boot log"));
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

    private void launchGame(String displayMode) {
        if (!normal.isEnabled()) return;
        Intent game = new Intent(this, GameActivity.class);
        game.putExtra("display_mode", displayMode);
        startActivity(game);
    }
}
