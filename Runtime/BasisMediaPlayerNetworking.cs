using System;
using System.Text;
using System.Threading.Tasks;
using Basis;
using Basis.Network.Core;
using Basis.Scripts.Networking;
using Basis.Scripts.Networking.NetworkedAvatar;
using UnityEngine;

[DisallowMultipleComponent]
[RequireComponent(typeof(BasisMediaPlayer))]
public sealed class BasisMediaPlayerNetworking : BasisNetworkBehaviour
{
    public enum SyncedPlaybackState : byte
    {
        Stopped = 0,
        Playing = 1,
        Paused = 2,
    }

    private enum MessageId : byte
    {
        FullState = 1,
        Play = 2,
        Pause = 3,
        Stop = 4,
        Seek = 5,
        RequestState = 6,
        Settings = 7,
    }

    [Flags]
    private enum SettingsFlags : byte
    {
        None = 0,
        AdminOnly = 1 << 0,
        AllowAnyoneToTakeControl = 1 << 1,
    }

    // Matches the panel's Perm_Control constant; admins (perm "*") also satisfy it.
    public const string PermControl = "basis.mediaplayer.control";
    public const string PermAdmin = "*";

    [Header("Permissions")]
    [Tooltip("If true, only clients with the basis.mediaplayer.control or * permission may take ownership and control playback. Overrides AllowAnyoneToTakeControl.")]
    public bool AdminOnly = false;

    [Tooltip("If true, any client may take ownership and control playback. If false, only the current owner can call SetUrl/Play/Stop/Pause/Resume/Seek. Ignored when AdminOnly is true.")]
    public bool AllowAnyoneToTakeControl = true;

    [Header("Sync")]
    [Tooltip("Remote clients seek to catch up when their position drifts more than this many seconds from the owner's last-broadcast position. Set to 0 to disable drift correction.")]
    [Min(0f)] public float DriftSeekThresholdSeconds = 2f;

    [Tooltip("Verbose log lines for join/leave sync, drift corrections, rejected control attempts.")]
    public bool VerboseLogging = false;

    private static readonly Encoding UrlEncoding = new UTF8Encoding(false, false);
    // FullState payload after the 1-byte MessageId: [state:1][positionTicks:8][settingsFlags:1][driftSec:4][urlLen:2] then url bytes.
    // positionTicks is 0 when the source is live (no seekable timeline); receivers treat 0 as "no position".
    private const int SettingsBlockSize = 1 + 4;
    private const int FullStateHeaderSize = 1 + 1 + 8 + SettingsBlockSize + 2;
    private const int SettingsPayloadSize = 1 + SettingsBlockSize;
    private const int SeekPayloadSize = 1 + 8;
    private const int FullStateSettingsOffset = 1 + 1 + 8;
    private const int FullStateUrlLenOffset = FullStateSettingsOffset + SettingsBlockSize;

    // Cached single-byte command payloads; SendCustomNetworkEvent does not retain references.
    private static readonly byte[] PlayBytes = { (byte)MessageId.Play };
    private static readonly byte[] PauseBytes = { (byte)MessageId.Pause };
    private static readonly byte[] StopBytes = { (byte)MessageId.Stop };
    private static readonly byte[] RequestStateBytes = { (byte)MessageId.RequestState };

    private BasisMediaPlayer mediaPlayer;
    private string currentSyncedUrl = string.Empty;
    private bool sendOnNetworkReady;
    private bool applyingRemoteCommand;
    private bool eventsHooked;

    // Main-thread scratch — Unity callbacks are serial so these don't need locking.
    private readonly ushort[] singleRecipient = new ushort[1];
    private readonly byte[] seekScratch = new byte[SeekPayloadSize];
    private readonly byte[] settingsScratch = new byte[SettingsPayloadSize];
    private byte[] fullStateScratch = Array.Empty<byte>();
    private byte[] cachedUrlBytes = Array.Empty<byte>();
    private string cachedUrlBytesSource;

    public BasisMediaPlayer MediaPlayer => mediaPlayer;

    public bool CanLocallyControl
    {
        get
        {
            if (!HasNetworkID)
            {
                return true;
            }

            if (IsOwnedLocallyOnClient)
            {
                return true;
            }

            if (IsLocalAdmin())
            {
                return true;
            }

            if (AdminOnly)
            {
                return false;
            }

            return AllowAnyoneToTakeControl;
        }
    }

    public static bool IsLocalAdmin()
    {
        var perms = BasisNetworkManagement.LocalPermissions;
        return perms != null && (perms.Contains(PermAdmin) || perms.Contains(PermControl));
    }

    public void Awake()
    {
        TryGetComponent(out mediaPlayer);
    }

    private void OnEnable()
    {
        if (mediaPlayer == null)
        {
            TryGetComponent(out mediaPlayer);
        }

        HookPlayerEvents();
    }

    private void OnDisable()
    {
        UnhookPlayerEvents();
    }

    public override void OnNetworkReady()
    {
        if (sendOnNetworkReady)
        {
            sendOnNetworkReady = false;
            BroadcastFullState();
        }
    }

    public override void OnPlayerJoined(BasisNetworkPlayer player)
    {
        if (player == null)
        {
            return;
        }

        if (!IsOwnedLocallyOnClient)
        {
            return;
        }

        var local = BasisNetworkPlayer.LocalPlayer;
        if (local != null && player.playerId == local.playerId)
        {
            return;
        }

        singleRecipient[0] = player.playerId;
        SendFullStateTo(singleRecipient);
        if (VerboseLogging)
        {
            BasisDebug.Log($"{nameof(BasisMediaPlayerNetworking)} sent late-join state to player {player.playerId}.", BasisDebug.LogTag.Video);
        }
    }

    public override void OnOwnershipTransfer(BasisNetworkPlayer newOwner)
    {
        if (IsOwnedLocallyOnClient)
        {
            BroadcastFullState();
            return;
        }

        if (!HasNetworkID)
        {
            return;
        }

        ushort owner = CurrentOwnerId;
        if (owner == 0)
        {
            return;
        }

        var local = BasisNetworkPlayer.LocalPlayer;
        if (local != null && owner == local.playerId)
        {
            return;
        }

        singleRecipient[0] = owner;
        SendCustomNetworkEvent(RequestStateBytes, DeliveryMethod.ReliableOrdered, singleRecipient);
        if (VerboseLogging)
        {
            BasisDebug.Log($"{nameof(BasisMediaPlayerNetworking)} requested state from new owner {owner}.", BasisDebug.LogTag.Video);
        }
    }

    public async Task SetUrl(string url)
    {
        if (string.IsNullOrEmpty(url))
        {
            return;
        }

        if (!await AcquireControlAsync())
        {
            return;
        }

        currentSyncedUrl = url;
        mediaPlayer.LoadUrl(url);
        // OnReady will fire BroadcastFullState once the source resolves; no manual send here.
    }

    public async Task Play()
    {
        if (!await AcquireControlAsync())
        {
            return;
        }

        mediaPlayer.Play();
    }

    public async Task Stop()
    {
        if (!await AcquireControlAsync())
        {
            return;
        }

        mediaPlayer.Stop();
        // BasisMediaPlayer.Stop fires no event, so we broadcast directly.
        SendOwnerSimple(MessageId.Stop);
    }

    public async Task Pause()
    {
        if (!await AcquireControlAsync())
        {
            return;
        }

        mediaPlayer.Pause();
    }

    public async Task Resume()
    {
        if (!await AcquireControlAsync())
        {
            return;
        }

        mediaPlayer.Resume();
    }

    public async Task Seek(TimeSpan position)
    {
        if (!await AcquireControlAsync())
        {
            return;
        }

        mediaPlayer.Seek(position);
    }

    public async Task SetAdminOnly(bool value)
    {
        if (AdminOnly == value)
        {
            return;
        }

        if (!await AcquireControlAsync())
        {
            return;
        }

        AdminOnly = value;
        BroadcastSettings();
    }

    public async Task SetAllowAnyoneToTakeControl(bool value)
    {
        if (AllowAnyoneToTakeControl == value)
        {
            return;
        }

        if (!await AcquireControlAsync())
        {
            return;
        }

        AllowAnyoneToTakeControl = value;
        BroadcastSettings();
    }

    public async Task SetDriftSeekThresholdSeconds(float value)
    {
        float clamped = Mathf.Max(0f, value);
        if (Mathf.Approximately(DriftSeekThresholdSeconds, clamped))
        {
            return;
        }

        if (!await AcquireControlAsync())
        {
            return;
        }

        DriftSeekThresholdSeconds = clamped;
        BroadcastSettings();
    }

    private async Task<bool> AcquireControlAsync()
    {
        if (!HasNetworkID)
        {
            return true;
        }

        if (IsOwnedLocallyOnClient)
        {
            return true;
        }

        if (!IsLocalAdmin())
        {
            if (AdminOnly)
            {
                if (VerboseLogging)
                {
                    BasisDebug.LogWarning($"{nameof(BasisMediaPlayerNetworking)} control rejected: AdminOnly is on and this client lacks the {PermControl} (or {PermAdmin}) permission.", BasisDebug.LogTag.Video);
                }

                return false;
            }

            if (!AllowAnyoneToTakeControl)
            {
                if (VerboseLogging)
                {
                    BasisDebug.LogWarning($"{nameof(BasisMediaPlayerNetworking)} control rejected: AllowAnyoneToTakeControl is false and this client is not the owner.", BasisDebug.LogTag.Video);
                }

                return false;
            }
        }

        var result = await TakeOwnershipAsync();
        if (!result.Success && VerboseLogging)
        {
            BasisDebug.LogWarning($"{nameof(BasisMediaPlayerNetworking)} ownership request was denied by the server.", BasisDebug.LogTag.Video);
        }

        return result.Success;
    }

    public override void OnNetworkMessage(ushort senderId, byte[] buffer, DeliveryMethod deliveryMethod)
    {
        if (buffer == null || buffer.Length < 1)
        {
            return;
        }

        var id = (MessageId)buffer[0];

        switch (id)
        {
            case MessageId.RequestState:
                if (IsOwnedLocallyOnClient)
                {
                    singleRecipient[0] = senderId;
                    SendFullStateTo(singleRecipient);
                }

                return;

            case MessageId.FullState:
                if (IsOwnedLocallyOnClient)
                {
                    return;
                }

                if (!TryDeserializeFullState(buffer, out string url, out var state, out long fullPos))
                {
                    return;
                }

                ApplyRemoteFullState(url, state, fullPos);
                return;

            case MessageId.Play:
                if (IsOwnedLocallyOnClient)
                {
                    return;
                }

                ApplyRemotePlay();
                return;

            case MessageId.Pause:
                if (IsOwnedLocallyOnClient)
                {
                    return;
                }

                ApplyRemotePause();
                return;

            case MessageId.Stop:
                if (IsOwnedLocallyOnClient)
                {
                    return;
                }

                ApplyRemoteStop();
                return;

            case MessageId.Seek:
                if (IsOwnedLocallyOnClient)
                {
                    return;
                }

                if (buffer.Length < SeekPayloadSize)
                {
                    return;
                }

                long seekTicks = ReadLong(buffer, 1);
                ApplyRemoteSeek(seekTicks);
                return;

            case MessageId.Settings:
                if (IsOwnedLocallyOnClient)
                {
                    return;
                }

                if (buffer.Length < SettingsPayloadSize)
                {
                    return;
                }

                ApplyRemoteSettings(buffer, 1);
                return;
        }
    }

    private void ApplyRemotePlay()
    {
        applyingRemoteCommand = true;
        try
        {
            if (mediaPlayer.IsPlaying && mediaPlayer.IsPaused)
            {
                mediaPlayer.Resume();
            }
            else if (!mediaPlayer.IsPlaying)
            {
                mediaPlayer.Play();
            }
        }
        finally
        {
            applyingRemoteCommand = false;
        }
    }

    private void ApplyRemotePause()
    {
        applyingRemoteCommand = true;
        try
        {
            if (!mediaPlayer.IsPlaying)
            {
                mediaPlayer.Play();
            }

            if (!mediaPlayer.IsPaused)
            {
                mediaPlayer.Pause();
            }
        }
        finally
        {
            applyingRemoteCommand = false;
        }
    }

    private void ApplyRemoteStop()
    {
        applyingRemoteCommand = true;
        try
        {
            if (mediaPlayer.IsPlaying)
            {
                mediaPlayer.Stop();
            }
        }
        finally
        {
            applyingRemoteCommand = false;
        }
    }

    private void ApplyRemoteSeek(long ticks)
    {
        if (ticks < 0)
        {
            return;
        }

        applyingRemoteCommand = true;
        try
        {
            mediaPlayer.Seek(TimeSpan.FromTicks(ticks));
        }
        catch (NotSupportedException)
        {
        }
        finally
        {
            applyingRemoteCommand = false;
        }
    }

    private void ApplyRemoteFullState(string url, SyncedPlaybackState state, long positionTicks)
    {
        bool urlChanged = !string.IsNullOrEmpty(url) && url != currentSyncedUrl;
        applyingRemoteCommand = true;
        try
        {
            if (urlChanged)
            {
                currentSyncedUrl = url;
                var media = BasisMediaSource.FromUrl(url);
                media.StartPosition = positionTicks > 0 ? TimeSpan.FromTicks(positionTicks) : TimeSpan.Zero;

                bool savedAutoPlay = mediaPlayer.AutoPlayOnSourceAssigned;
                mediaPlayer.AutoPlayOnSourceAssigned = state == SyncedPlaybackState.Playing || state == SyncedPlaybackState.Paused;
                mediaPlayer.LoadSource(media);
                mediaPlayer.AutoPlayOnSourceAssigned = savedAutoPlay;

                if (state == SyncedPlaybackState.Paused)
                {
                    mediaPlayer.Pause();
                }
                else if (state == SyncedPlaybackState.Stopped)
                {
                    mediaPlayer.Stop();
                }

                return;
            }

            switch (state)
            {
                case SyncedPlaybackState.Stopped:
                    if (mediaPlayer.IsPlaying)
                    {
                        mediaPlayer.Stop();
                    }

                    break;

                case SyncedPlaybackState.Playing:
                    if (!mediaPlayer.IsPlaying)
                    {
                        mediaPlayer.Play();
                    }
                    else if (mediaPlayer.IsPaused)
                    {
                        mediaPlayer.Resume();
                    }

                    MaybeCorrectDrift(positionTicks);
                    break;

                case SyncedPlaybackState.Paused:
                    if (!mediaPlayer.IsPlaying)
                    {
                        mediaPlayer.Play();
                    }

                    if (!mediaPlayer.IsPaused)
                    {
                        mediaPlayer.Pause();
                    }

                    MaybeCorrectDrift(positionTicks);
                    break;
            }
        }
        finally
        {
            applyingRemoteCommand = false;
        }
    }

    private void MaybeCorrectDrift(long positionTicks)
    {
        if (DriftSeekThresholdSeconds <= 0f)
        {
            return;
        }

        if (positionTicks <= 0)
        {
            return;
        }

        var target = TimeSpan.FromTicks(positionTicks);
        var current = mediaPlayer.Position;
        double diff = Math.Abs((target - current).TotalSeconds);
        if (diff <= DriftSeekThresholdSeconds)
        {
            return;
        }

        try
        {
            mediaPlayer.Seek(target);
            if (VerboseLogging)
            {
                BasisDebug.Log($"{nameof(BasisMediaPlayerNetworking)} drift-corrected by {diff:F2}s.", BasisDebug.LogTag.Video);
            }
        }
        catch (NotSupportedException)
        {
            // Live / non-seekable sources can't drift-correct; that's fine.
        }
    }

    private void HookPlayerEvents()
    {
        if (eventsHooked || mediaPlayer == null)
        {
            return;
        }

        mediaPlayer.OnReady += HandleLocalReady;
        mediaPlayer.OnStarted += HandleLocalStarted;
        mediaPlayer.OnPaused += HandleLocalPaused;
        mediaPlayer.OnEnded += HandleLocalEnded;
        mediaPlayer.OnSeekCompleted += HandleLocalSeekCompleted;
        eventsHooked = true;
    }

    private void UnhookPlayerEvents()
    {
        if (!eventsHooked || mediaPlayer == null)
        {
            return;
        }

        mediaPlayer.OnReady -= HandleLocalReady;
        mediaPlayer.OnStarted -= HandleLocalStarted;
        mediaPlayer.OnPaused -= HandleLocalPaused;
        mediaPlayer.OnEnded -= HandleLocalEnded;
        mediaPlayer.OnSeekCompleted -= HandleLocalSeekCompleted;
        eventsHooked = false;
    }

    private void HandleLocalReady()
    {
        if (applyingRemoteCommand)
        {
            return;
        }

        if (!IsOwnedLocallyOnClient)
        {
            return;
        }

        var media = mediaPlayer.ActiveMediaSource;
        if (media != null && !string.IsNullOrEmpty(media.Uri))
        {
            currentSyncedUrl = media.Uri;
        }

        BroadcastFullState();
    }

    private void HandleLocalStarted()
    {
        if (applyingRemoteCommand)
        {
            return;
        }

        if (!IsOwnedLocallyOnClient)
        {
            return;
        }

        SendOwnerSimple(MessageId.Play);
    }

    private void HandleLocalPaused()
    {
        if (applyingRemoteCommand)
        {
            return;
        }

        if (!IsOwnedLocallyOnClient)
        {
            return;
        }

        SendOwnerSimple(MessageId.Pause);
    }

    private void HandleLocalEnded()
    {
        if (applyingRemoteCommand)
        {
            return;
        }

        if (!IsOwnedLocallyOnClient)
        {
            return;
        }

        SendOwnerSimple(MessageId.Stop);
    }

    private void HandleLocalSeekCompleted(TimeSpan position)
    {
        if (applyingRemoteCommand)
        {
            return;
        }

        if (!IsOwnedLocallyOnClient)
        {
            return;
        }

        if (!HasNetworkID)
        {
            return;
        }

        seekScratch[0] = (byte)MessageId.Seek;
        WriteLong(seekScratch, 1, position.Ticks);
        SendCustomNetworkEvent(seekScratch, DeliveryMethod.ReliableOrdered);
    }

    private void SendOwnerSimple(MessageId id)
    {
        if (!HasNetworkID)
        {
            sendOnNetworkReady = true;
            return;
        }

        byte[] payload = id switch
        {
            MessageId.Play => PlayBytes,
            MessageId.Pause => PauseBytes,
            MessageId.Stop => StopBytes,
            MessageId.RequestState => RequestStateBytes,
            _ => new byte[] { (byte)id },
        };
        SendCustomNetworkEvent(payload, DeliveryMethod.ReliableOrdered);
    }

    private void BroadcastFullState()
    {
        if (!HasNetworkID)
        {
            sendOnNetworkReady = true;
            return;
        }

        SendCustomNetworkEvent(SerializeFullState(), DeliveryMethod.ReliableOrdered);
    }

    private void SendFullStateTo(ushort[] recipients)
    {
        if (!HasNetworkID)
        {
            return;
        }

        SendCustomNetworkEvent(SerializeFullState(), DeliveryMethod.ReliableOrdered, recipients);
    }

    private SyncedPlaybackState GetLocalState()
    {
        if (!mediaPlayer.IsPlaying)
        {
            return SyncedPlaybackState.Stopped;
        }

        return mediaPlayer.IsPaused ? SyncedPlaybackState.Paused : SyncedPlaybackState.Playing;
    }

    private string GetActiveUrl()
    {
        var media = mediaPlayer.ActiveMediaSource;
        if (media != null && !string.IsNullOrEmpty(media.Uri))
        {
            return media.Uri;
        }

        return currentSyncedUrl ?? string.Empty;
    }

    private byte[] SerializeFullState()
    {
        string url = GetActiveUrl();
        bool urlChanged = !string.Equals(cachedUrlBytesSource, url, StringComparison.Ordinal);
        if (urlChanged)
        {
            cachedUrlBytes = string.IsNullOrEmpty(url) ? Array.Empty<byte>() : UrlEncoding.GetBytes(url);
            if (cachedUrlBytes.Length > ushort.MaxValue)
            {
                BasisDebug.LogError($"{nameof(BasisMediaPlayerNetworking)} URL exceeds {ushort.MaxValue} bytes; truncating.", BasisDebug.LogTag.Video);
                Array.Resize(ref cachedUrlBytes, ushort.MaxValue);
            }

            cachedUrlBytesSource = url;
        }

        byte[] urlBytes = cachedUrlBytes;
        int totalSize = FullStateHeaderSize + urlBytes.Length;
        bool sizeChanged = fullStateScratch.Length != totalSize;
        if (sizeChanged)
        {
            fullStateScratch = new byte[totalSize];
            fullStateScratch[0] = (byte)MessageId.FullState;
            WriteUShort(fullStateScratch, FullStateUrlLenOffset, (ushort)urlBytes.Length);
        }

        if ((urlChanged || sizeChanged) && urlBytes.Length > 0)
        {
            Buffer.BlockCopy(urlBytes, 0, fullStateScratch, FullStateHeaderSize, urlBytes.Length);
        }

        fullStateScratch[1] = (byte)GetLocalState();
        long positionTicks = mediaPlayer.Duration > TimeSpan.Zero ? mediaPlayer.Position.Ticks : 0L;
        WriteLong(fullStateScratch, 2, positionTicks);
        WriteSettingsBlock(fullStateScratch, FullStateSettingsOffset);
        return fullStateScratch;
    }

    private byte[] SerializeSettings()
    {
        settingsScratch[0] = (byte)MessageId.Settings;
        WriteSettingsBlock(settingsScratch, 1);
        return settingsScratch;
    }

    private void WriteSettingsBlock(byte[] buf, int offset)
    {
        SettingsFlags flags = SettingsFlags.None;
        if (AdminOnly)
        {
            flags |= SettingsFlags.AdminOnly;
        }

        if (AllowAnyoneToTakeControl)
        {
            flags |= SettingsFlags.AllowAnyoneToTakeControl;
        }

        buf[offset] = (byte)flags;
        WriteFloat(buf, offset + 1, DriftSeekThresholdSeconds);
    }

    private void ReadSettingsBlock(byte[] buf, int offset)
    {
        var flags = (SettingsFlags)buf[offset];
        AdminOnly = (flags & SettingsFlags.AdminOnly) != 0;
        AllowAnyoneToTakeControl = (flags & SettingsFlags.AllowAnyoneToTakeControl) != 0;
        float drift = ReadFloat(buf, offset + 1);
        if (drift < 0f || float.IsNaN(drift) || float.IsInfinity(drift))
        {
            drift = 0f;
        }

        DriftSeekThresholdSeconds = drift;
    }

    private void ApplyRemoteSettings(byte[] buffer, int offset)
    {
        ReadSettingsBlock(buffer, offset);
        if (VerboseLogging)
        {
            BasisDebug.Log($"{nameof(BasisMediaPlayerNetworking)} applied remote settings: AdminOnly={AdminOnly}, AllowAnyoneToTakeControl={AllowAnyoneToTakeControl}, DriftSeekThresholdSeconds={DriftSeekThresholdSeconds}.", BasisDebug.LogTag.Video);
        }
    }

    private void BroadcastSettings()
    {
        if (!HasNetworkID)
        {
            sendOnNetworkReady = true;
            return;
        }

        SendCustomNetworkEvent(SerializeSettings(), DeliveryMethod.ReliableOrdered);
    }

    private bool TryDeserializeFullState(byte[] buffer, out string url, out SyncedPlaybackState state, out long positionTicks)
    {
        url = string.Empty;
        state = SyncedPlaybackState.Stopped;
        positionTicks = 0;
        if (buffer == null || buffer.Length < FullStateHeaderSize)
        {
            return false;
        }

        byte stateByte = buffer[1];
        if (stateByte > (byte)SyncedPlaybackState.Paused)
        {
            return false;
        }

        state = (SyncedPlaybackState)stateByte;
        positionTicks = ReadLong(buffer, 2);
        ReadSettingsBlock(buffer, FullStateSettingsOffset);
        ushort urlLen = ReadUShort(buffer, FullStateUrlLenOffset);
        if (buffer.Length < FullStateHeaderSize + urlLen)
        {
            return false;
        }

        if (urlLen > 0)
        {
            url = UrlEncoding.GetString(buffer, FullStateHeaderSize, urlLen);
        }

        return true;
    }

    private static void WriteLong(byte[] buf, int offset, long value)
    {
        for (int i = 0; i < 8; i++)
        {
            buf[offset + i] = (byte)(value >> (i * 8));
        }
    }

    private static long ReadLong(byte[] buf, int offset)
    {
        long v = 0;
        for (int i = 0; i < 8; i++)
        {
            v |= (long)buf[offset + i] << (i * 8);
        }

        return v;
    }

    private static void WriteUShort(byte[] buf, int offset, ushort value)
    {
        buf[offset] = (byte)(value & 0xFF);
        buf[offset + 1] = (byte)((value >> 8) & 0xFF);
    }

    private static ushort ReadUShort(byte[] buf, int offset)
    {
        return (ushort)(buf[offset] | (buf[offset + 1] << 8));
    }

    private static void WriteFloat(byte[] buf, int offset, float value)
    {
        int bits = BitConverter.SingleToInt32Bits(value);
        buf[offset] = (byte)bits;
        buf[offset + 1] = (byte)(bits >> 8);
        buf[offset + 2] = (byte)(bits >> 16);
        buf[offset + 3] = (byte)(bits >> 24);
    }

    private static float ReadFloat(byte[] buf, int offset)
    {
        int bits = buf[offset] | (buf[offset + 1] << 8) | (buf[offset + 2] << 16) | (buf[offset + 3] << 24);
        return BitConverter.Int32BitsToSingle(bits);
    }
}
