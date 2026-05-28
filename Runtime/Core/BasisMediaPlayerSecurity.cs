using System;
using System.IO;
using System.Net;
using UnityEngine;

public static class BasisMediaPlayerSecurity
{
    public const int MaxQueueLengthCap = 256;
    public const int MaxPayloadBytesCap = 16 * 1024 * 1024;
    public const float ClipLengthSecondsCap = 30f;
    public const int MaxQueuedAudioFramesCap = 512;

    public static bool IsUrlAllowed(string url, out string reason)
    {
        reason = null;
        if (string.IsNullOrWhiteSpace(url))
        {
            reason = "URL is empty.";
            return false;
        }
        if (!Uri.TryCreate(url, UriKind.Absolute, out Uri uri))
        {
            reason = "URL must be absolute.";
            return false;
        }
        string scheme = uri.Scheme.ToLowerInvariant();
        if (scheme == "file")
        {
            reason = "file:// URLs are blocked.";
            return false;
        }
        // Live-streaming schemes handled by the OS-codec engine (basis_media_native):
        //   rtsp/rtspt  RTSP over UDP/TCP (rtspt = interleaved over TCP, low latency)
        //   rtmp/rtmps  RTMP / RTMP-over-TLS
        //   http/https  fragmented MP4 (.mp4) and MPEG-TS (.ts) over HTTP(S)
        if (scheme != "http" && scheme != "https" &&
            scheme != "rtsp" && scheme != "rtspt" &&
            scheme != "rtmp" && scheme != "rtmps")
        {
            reason = $"Scheme '{scheme}' is not allowed.";
            return false;
        }

        string host = uri.Host;
        if (string.IsNullOrEmpty(host))
        {
            reason = "URL is missing a host.";
            return false;
        }

        if (IsBlockedHost(host, out string hostReason))
        {
            reason = hostReason;
            return false;
        }

        return true;
    }

    public static bool IsBlockedHost(string host, out string reason)
    {
        reason = null;
        if (string.IsNullOrEmpty(host)) { reason = "missing host"; return true; }

        string lower = host.ToLowerInvariant();
        bool allowLoopback = Application.isEditor;
        if (!allowLoopback && (lower == "localhost" || lower.EndsWith(".localhost")))
        {
            reason = "loopback host is blocked in builds.";
            return true;
        }

        if (IPAddress.TryParse(host.Trim('[', ']'), out IPAddress ip))
        {
            if (!allowLoopback && IPAddress.IsLoopback(ip))
            {
                reason = "loopback address is blocked in builds.";
                return true;
            }
            if (IsLinkLocal(ip))
            {
                reason = "link-local address (including cloud metadata) is blocked.";
                return true;
            }
            if (IsPrivate(ip))
            {
                reason = "RFC1918 private address is blocked.";
                return true;
            }
            if (IsUniqueLocalIPv6(ip))
            {
                reason = "IPv6 unique-local address is blocked.";
                return true;
            }
        }
        return false;
    }

    private static bool IsLinkLocal(IPAddress ip)
    {
        if (ip.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork)
        {
            byte[] b = ip.GetAddressBytes();
            return b[0] == 169 && b[1] == 254;
        }
        return ip.IsIPv6LinkLocal;
    }

    private static bool IsPrivate(IPAddress ip)
    {
        if (ip.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork) return false;
        byte[] b = ip.GetAddressBytes();
        if (b[0] == 10) return true;
        if (b[0] == 192 && b[1] == 168) return true;
        if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) return true;
        return false;
    }

    private static bool IsUniqueLocalIPv6(IPAddress ip)
    {
        if (ip.AddressFamily != System.Net.Sockets.AddressFamily.InterNetworkV6) return false;
        byte[] b = ip.GetAddressBytes();
        return (b[0] & 0xFE) == 0xFC;
    }

    public static bool TrySandboxLogPath(string requested, out string sandboxed, out string reason)
    {
        reason = null;
        if (string.IsNullOrEmpty(requested))
        {
            sandboxed = string.Empty;
            return true;
        }
        string root = Path.GetFullPath(Application.persistentDataPath);
        string full;
        try
        {
            full = Path.GetFullPath(Path.IsPathRooted(requested) ? requested : Path.Combine(root, requested));
        }
        catch (Exception ex)
        {
            sandboxed = null;
            reason = "Path normalization failed: " + ex.Message;
            return false;
        }
        string rootWithSep = root.EndsWith(Path.DirectorySeparatorChar.ToString()) ? root : root + Path.DirectorySeparatorChar;
        if (!full.StartsWith(rootWithSep, StringComparison.OrdinalIgnoreCase) && full != root)
        {
            sandboxed = null;
            reason = "Log path must live under Application.persistentDataPath.";
            return false;
        }
        sandboxed = full;
        return true;
    }
}
