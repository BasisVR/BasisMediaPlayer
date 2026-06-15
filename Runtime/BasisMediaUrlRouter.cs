using System;

/// <summary>
/// Optional URL-resolution seam between the player and a higher-level resolver
/// (e.g. the yt-dlp integration package). The player core has no reference to any
/// integration, so this is how a URL field can steer page URLs through a resolver
/// without the player depending on it.
///
/// An integration installs <see cref="Resolver"/> at load (e.g. via
/// <c>RuntimeInitializeOnLoadMethod</c>). Callers with a raw URL hand it to
/// <see cref="TryResolveAndLoad"/>: the resolver either takes ownership of the load
/// (resolving a page URL to its stream(s) and loading them, possibly async) and
/// returns true, or returns false to let the caller load the URL directly. When no
/// integration is installed <see cref="Resolver"/> is null and every URL loads
/// directly — identical to having no integration at all. This routes only; it never
/// blocks a URL (host trust is enforced separately by BasisMediaPlayerSecurity).
/// </summary>
public static class BasisMediaUrlRouter
{
    /// <summary>
    /// Installed by an optional integration. Returns true if it took ownership of the
    /// load for the given URL, false to let the caller load it directly. Null when no
    /// integration is present.
    /// </summary>
    public static Func<BasisMediaPlayer, string, bool> Resolver;

    // Delimiters that end the path portion of a URL (query / fragment), hoisted so
    // IsDirectlyPlayable doesn't allocate a char[] per call.
    private static readonly char[] PathEnd = { '?', '#' };

    /// <summary>
    /// Routes <paramref name="url"/> through the installed resolver, if any. Returns
    /// true when the resolver took ownership (the caller should not also load); false
    /// when there is no resolver or it declined (the caller loads directly).
    /// </summary>
    public static bool TryResolveAndLoad(BasisMediaPlayer player, string url)
        => Resolver != null && Resolver(player, url);

    /// <summary>
    /// True if the player can open <paramref name="url"/> directly, without a resolver:
    /// any non-HTTP scheme (transport like rtsp/rtmp/rist, or a local file), or an
    /// http(s) URL whose path ends in a media-container extension
    /// (.mp4/.m4s/.ts/.m2ts/.mts/.m3u8). An http(s) URL with no media extension is a page URL
    /// (e.g. a YouTube/Twitch watch page) and needs a resolver. This is the single
    /// source of truth for the live-vs-resolve steering; resolvers and callers both
    /// consult it. It classifies only — it never blocks (host trust is separate).
    /// </summary>
    public static bool IsDirectlyPlayable(string url)
    {
        if (string.IsNullOrEmpty(url)) return false;

        bool isHttp = url.StartsWith("http://", StringComparison.OrdinalIgnoreCase)
                   || url.StartsWith("https://", StringComparison.OrdinalIgnoreCase);
        if (!isHttp) return true; // transport scheme or local file — opened directly

        // Strip query/fragment so "…/stream.m3u8?token=…" still matches by extension.
        string path = url;
        int cut = path.IndexOfAny(PathEnd);
        if (cut >= 0) path = path.Substring(0, cut);

        // No .mpd — there is no DASH demuxer in the native engine, so a raw MPD must go
        // through a resolver (yt-dlp) rather than being treated as directly playable.
        return path.EndsWith(".mp4", StringComparison.OrdinalIgnoreCase)
            || path.EndsWith(".m4s", StringComparison.OrdinalIgnoreCase)
            || path.EndsWith(".ts", StringComparison.OrdinalIgnoreCase)
            || path.EndsWith(".m2ts", StringComparison.OrdinalIgnoreCase)   // Blu-ray-flavour MPEG-TS
            || path.EndsWith(".mts", StringComparison.OrdinalIgnoreCase)    // AVCHD-flavour MPEG-TS
            || path.EndsWith(".m3u8", StringComparison.OrdinalIgnoreCase);
    }
}
