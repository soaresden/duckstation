package com.github.stenzek.duckstation;

import android.os.AsyncTask;
import android.view.View;
import android.widget.ImageView;
import android.widget.TextView;

import androidx.core.content.ContextCompat;

import java.net.URLDecoder;

public class GameListEntry {
    public enum EntryType {
        Disc,
        PSExe,
        Playlist,
        PSF
    }

    public enum CompatibilityRating {
        Unknown,
        DoesntBoot,
        CrashesInIntro,
        CrashesInGame,
        GraphicalAudioIssues,
        NoIssues,
    }

    private String mPath;
    private String mCode;
    private String mTitle;
    private String mFileTitle;
    private long mSize;
    private String mModifiedTime;
    private DiscRegion mRegion;
    private EntryType mType;
    private CompatibilityRating mCompatibilityRating;
    private String mCoverPath;

    public static String getFileNameForPath(String path) {
        String newPath = path;
        int lastSlash;
        if (path.startsWith("content://") || path.startsWith("file://")) {
            try {
                newPath = URLDecoder.decode(path, "UTF-8");
            } catch (Exception e) {
            }

            final int lastColon = newPath.lastIndexOf(':');
            lastSlash = newPath.lastIndexOf('/');
            if (lastColon >= 0 && lastColon > lastSlash)
                lastSlash = lastColon;
        } else {
            lastSlash = path.lastIndexOf('/');
        }

        if (lastSlash >= 0)
            newPath = newPath.substring(lastSlash + 1);

        return newPath;
    }

    private String getFileTitle(String path) {
        final String newPath = getFileNameForPath(path);
        final int extensionPos = newPath.lastIndexOf('.');
        if (extensionPos >= 0)
            return newPath.substring(0, extensionPos);
        else
            return newPath;
    }

    public GameListEntry(String path, String code, String title, long size, String modifiedTime, String region,
                         String type, String compatibilityRating, String coverPath) {
        mPath = path;
        mCode = code;
        mTitle = title;
        mFileTitle = getFileNameForPath(path);
        mSize = size;
        mModifiedTime = modifiedTime;
        mCoverPath = coverPath;

        try {
            mRegion = DiscRegion.valueOf(region);
        } catch (IllegalArgumentException e) {
            mRegion = DiscRegion.NTSC_U;
        }

        try {
            mType = EntryType.valueOf(type);
        } catch (IllegalArgumentException e) {
            mType = EntryType.Disc;
        }

        try {
            mCompatibilityRating = CompatibilityRating.valueOf(compatibilityRating);
        } catch (IllegalArgumentException e) {
            mCompatibilityRating = CompatibilityRating.Unknown;
        }
    }

    public String getPath() {
        return mPath;
    }

    public String getCode() {
        return mCode;
    }

    public String getTitle() {
        return mTitle;
    }

    public String getFileTitle() {
        return mFileTitle;
    }

    public long getSize() { return mSize; }

    public String getModifiedTime() {
        return mModifiedTime;
    }

    public DiscRegion getRegion() {
        return mRegion;
    }

    public EntryType getType() {
        return mType;
    }

    public CompatibilityRating getCompatibilityRating() {
        return mCompatibilityRating;
    }

    public String getCoverPath() { return mCoverPath; }

    public void setCoverPath(String coverPath) { mCoverPath = coverPath; }
}
