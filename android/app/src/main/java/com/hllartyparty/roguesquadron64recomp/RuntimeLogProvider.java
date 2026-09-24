package com.hllartyparty.roguesquadron64recomp;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileNotFoundException;

/** Read-only provider for sharing the complete latest native runtime log. */
public final class RuntimeLogProvider extends ContentProvider {
    private static final String CURRENT = "roguesq-runtime.log";
    private static final String PREVIOUS = "roguesq-runtime.previous.log";

    @Override
    public boolean onCreate() {
        return true;
    }

    private File selectedLog(Uri uri) throws FileNotFoundException {
        if (getContext() == null || !"/latest".equals(uri.getPath())) {
            throw new FileNotFoundException("Unknown runtime-log URI");
        }
        File current = new File(getContext().getFilesDir(), CURRENT);
        if (current.isFile() && current.length() > 0) return current;
        File previous = new File(getContext().getFilesDir(), PREVIOUS);
        if (previous.isFile() && previous.length() > 0) return previous;
        throw new FileNotFoundException("No runtime log is available");
    }

    @Override
    public String getType(Uri uri) {
        return "text/plain";
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("Runtime log is read-only");
        return ParcelFileDescriptor.open(selectedLog(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        try {
            File log = selectedLog(uri);
            String[] requested = projection != null ? projection
                : new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE };
            MatrixCursor cursor = new MatrixCursor(requested, 1);
            MatrixCursor.RowBuilder row = cursor.newRow();
            for (String column : requested) {
                if (OpenableColumns.DISPLAY_NAME.equals(column)) {
                    row.add("RogueSquadron64-latest-log.txt");
                } else if (OpenableColumns.SIZE.equals(column)) {
                    row.add(log.length());
                } else {
                    row.add(null);
                }
            }
            return cursor;
        } catch (FileNotFoundException error) {
            return new MatrixCursor(projection != null ? projection : new String[0], 0);
        }
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("Runtime log is read-only");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Runtime log is read-only");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Runtime log is read-only");
    }
}
