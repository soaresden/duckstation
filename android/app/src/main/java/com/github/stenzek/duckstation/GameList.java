package com.github.stenzek.duckstation;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.AsyncTask;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;

import java.io.File;
import java.util.ArrayList;
import androidx.preference.PreferenceManager;

import java.util.Arrays;
import java.util.Comparator;
import java.util.Set;

public class GameList {
    public interface OnRefreshListener {
        void onGameListRefresh();
    }

    private GameListEntry[] mEntries;
    private ArrayList<OnRefreshListener> mRefreshListeners = new ArrayList<>();

    public GameList() {
        mEntries = new GameListEntry[0];
    }

    public void addRefreshListener(OnRefreshListener listener) {
        mRefreshListeners.add(listener);
    }
    public void removeRefreshListener(OnRefreshListener listener) {
        mRefreshListeners.remove(listener);
    }
    public void fireRefreshListeners() {
        for (OnRefreshListener listener : mRefreshListeners)
            listener.onGameListRefresh();
    }

    private class GameListEntryComparator implements Comparator<GameListEntry> {
        @Override
        public int compare(GameListEntry left, GameListEntry right) {
            return left.getTitle().compareTo(right.getTitle());
        }
    }

    private boolean scanNativeDirectory(AndroidHostInterface hi, String path, boolean recursive) {
        final File directory = new File(path);
        if (!directory.isDirectory())
            return false;

        final File[] files = directory.listFiles();
        if (files == null)
            return false;

        for (final File file : files) {
            final String filePath = file.getAbsolutePath();
            if (!hi.isScannableGameListFilename(filePath))
                continue;

            final long lastModified = file.lastModified();
            hi.scanGameListFile(filePath, lastModified);
        }

        return true;
    }

    private static final String[] scanProjection = new String[]{
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED};

    private void scanDirectory(AndroidHostInterface hi, ContentResolver resolver, Uri treeUri, boolean recursive) {
        try {
            final String treeDocId = DocumentsContract.getTreeDocumentId(treeUri);
            final Uri queryUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, treeDocId);
            final Cursor cursor = resolver.query(queryUri, scanProjection, null, null, null);
            final int count = cursor.getCount();

            while (cursor.moveToNext()) {
                try {
                    final String mimeType = cursor.getString(2);
                    final String documentId = cursor.getString(0);
                    final Uri uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId);
                    if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mimeType)) {
                        if (recursive)
                            scanDirectory(hi, resolver, Uri.parse(documentId), true);

                        continue;
                    }

                    final String uriString = uri.toString();
                    if (!hi.isScannableGameListFilename(uriString)) {
                        Log.d("GameList", "Skipping scanning " + uriString);
                        continue;
                    }

                    final long lastModified = cursor.getLong(3);
                    hi.scanGameListFile(uriString, lastModified);
                }
                catch (Exception e) {
                    e.printStackTrace();
                }
            }
            cursor.close();
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    private void scanDirectory(AndroidHostInterface hi, ContentResolver resolver, String path, boolean recursive) {
        if (path.charAt(0) == '/') {
            // we have a native path
            scanNativeDirectory(hi, path, recursive);
            return;
        }

        scanDirectory(hi, resolver, Uri.parse(path), recursive);
    }

    private void refresh(boolean invalidateCache, boolean invalidateDatabase, Activity parentActivity, AndroidProgressCallback progressCallback) {
        final AndroidHostInterface hi = AndroidHostInterface.getInstance();
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(parentActivity);
        final ContentResolver resolver = parentActivity.getContentResolver();
        hi.beginGameListRefresh(invalidateCache, invalidateDatabase);

        final Set<String> recursiveDirs = PreferenceHelpers.getStringSet(prefs, "GameList/RecursivePaths");
        final Set<String> dirs = PreferenceHelpers.getStringSet(prefs, "GameList/Paths");
        final int totalDirs = (recursiveDirs != null ? recursiveDirs.size() : 0) + (dirs != null ? dirs.size() : 0);
        progressCallback.setProgressRange(totalDirs);

        int dirCount = 0;

        if (recursiveDirs != null) {
            for (String path : recursiveDirs) {
                scanDirectory(hi, resolver, path, true);
                progressCallback.setProgressValue(++dirCount);
            }
        }
        if (dirs != null) {
            for (String path : dirs) {
                scanDirectory(hi, resolver, path, false);
                progressCallback.setProgressValue(++dirCount);
            }
        }

        hi.endGameListRefresh();
    }


    public void refresh(boolean invalidateCache, boolean invalidateDatabase, Activity parentActivity) {
        // Search and get entries from native code
        AndroidProgressCallback progressCallback = new AndroidProgressCallback(parentActivity);
        AsyncTask.execute(() -> {
            refresh(invalidateCache, invalidateDatabase, parentActivity, progressCallback);
            GameListEntry[] newEntries = AndroidHostInterface.getInstance().getGameListEntries();
            Arrays.sort(newEntries, new GameListEntryComparator());

            parentActivity.runOnUiThread(() -> {
                try {
                    progressCallback.dismiss();
                } catch (Exception e) {
                    Log.e("GameList", "Exception dismissing refresh progress");
                    e.printStackTrace();
                }
                mEntries = newEntries;
                fireRefreshListeners();
            });
        });
    }

    public int getEntryCount() {
        return mEntries.length;
    }

    public GameListEntry getEntry(int index) {
        return mEntries[index];
    }

    public GameListEntry getEntryForPath(String path) {
        for (final GameListEntry entry : mEntries) {
            if (entry.getPath().equals(path))
                return entry;
        }

        return null;
    }
}
