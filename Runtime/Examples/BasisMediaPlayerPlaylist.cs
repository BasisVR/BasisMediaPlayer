using System;
using System.Collections.Generic;
using UnityEngine;

// One playlist entry: a URL (page or direct — it loads through the player's
// normal routing) and an optional display name, shown until a richer title
// arrives at load time.
[Serializable]
public sealed class BasisMediaPlaylistEntry
{
    public string Url;
    public string DisplayName;
}

// How the playlist advances when the current entry finishes (OnEnded).
public enum BasisMediaPlaylistAdvance
{
    None = 0,        // never auto-advance; PlayAt/Next/Previous only
    Sequential = 1,  // advance to the next entry, stop after the last
    LoopAll = 2,     // advance and wrap back to the first entry
}

// Ordered play queue driving a BasisMediaPlayer. Purely orchestration: each
// entry loads through the player's normal path (resolver steering, security
// gates), so metadata and networking behave exactly as for a manual load.
//
// Networked players: entry changes route through the sibling
// BasisMediaPlayerNetworking (SetUrl), so the existing URL sync carries every
// change to remote clients — the playlist itself is not networked and only the
// controlling client needs it populated. Auto-advance runs only on the owning
// client (every client sees OnEnded; one authority picks the next entry).
// Live entries never end, so they never auto-advance.
[DisallowMultipleComponent]
public class BasisMediaPlayerPlaylist : MonoBehaviour
{
    [Tooltip("Player this playlist drives. Defaults to a BasisMediaPlayer on the same GameObject.")]
    public BasisMediaPlayer Player;

    [Tooltip("Entries in play order. Page URLs resolve per-client exactly like a manual load.")]
    public List<BasisMediaPlaylistEntry> Entries = new List<BasisMediaPlaylistEntry>();

    [Tooltip("Advance policy when the current entry ends.")]
    public BasisMediaPlaylistAdvance Advance = BasisMediaPlaylistAdvance.Sequential;

    [Tooltip("If true, the first entry loads on Start (locally, like BasisMediaPlayerStreaming's ConfigureOnStart; networking settles who wins). When a playlist drives the player, disable BasisMediaPlayerStreaming's ConfigureOnStart or both will load a source on start.")]
    public bool PlayOnStart = true;

    // Index of the entry this playlist last loaded; -1 = nothing loaded from
    // the playlist yet. Not reset by direct loads made outside the playlist.
    public int CurrentIndex = -1;

    // Raised after the playlist starts loading an entry (index into Entries).
    public event Action<int> OnEntryChanged;

    private BasisMediaPlayerNetworking networking;

    private void Awake()
    {
        if (Player == null) TryGetComponent(out Player);
        if (Player != null) Player.TryGetComponent(out networking);
    }

    private void OnEnable()
    {
        if (Player != null) Player.OnEnded += HandleEnded;
    }

    private void OnDisable()
    {
        if (Player != null) Player.OnEnded -= HandleEnded;
    }

    private void Start()
    {
        if (!PlayOnStart || CurrentIndex >= 0) return;
        if (Entries == null || Entries.Count == 0) return;
        // Local load, not SetUrl: every client starting the same entry is the
        // same shape as BasisMediaPlayerStreaming's ConfigureOnStart, and the
        // networking late-join/full-state flow settles who wins.
        LoadEntry(0, viaNetworking: false);
    }

    public void PlayAt(int index) => LoadEntry(index, viaNetworking: true);

    private void LoadEntry(int index, bool viaNetworking)
    {
        if (Player == null)
        {
            BasisDebug.LogWarning("BasisMediaPlayerPlaylist has no BasisMediaPlayer to drive.", BasisDebug.LogTag.Video);
            return;
        }
        if (Entries == null || index < 0 || index >= Entries.Count) return;
        // A client that can't take control couldn't load the entry anyway;
        // bail before touching CurrentIndex so the local cursor stays honest.
        if (viaNetworking && networking != null && !networking.CanLocallyControl) return;
        BasisMediaPlaylistEntry entry = Entries[index];
        if (entry == null || string.IsNullOrWhiteSpace(entry.Url)) return;

        CurrentIndex = index;
        if (viaNetworking && networking != null)
        {
            // Acquires control (ownership permitting); remote clients follow the URL sync.
            _ = networking.SetUrl(entry.Url);
        }
        else
        {
            Player.LoadUrl(entry.Url);
        }
        // Layer the authored name over the URL-derived defaults LoadUrl just
        // seeded; a resolver's richer title still lands on top later.
        if (!string.IsNullOrEmpty(entry.DisplayName))
        {
            Player.ApplyMetadata(new BasisMediaMetadata { Title = entry.DisplayName });
        }
        OnEntryChanged?.Invoke(index);
    }

    // Manual navigation wraps at both ends regardless of the Advance policy.
    public void Next()
    {
        int count = Entries != null ? Entries.Count : 0;
        if (count == 0) return;
        PlayAt(CurrentIndex < 0 ? 0 : (CurrentIndex + 1) % count);
    }

    public void Previous()
    {
        int count = Entries != null ? Entries.Count : 0;
        if (count == 0) return;
        PlayAt(CurrentIndex <= 0 ? count - 1 : CurrentIndex - 1);
    }

    private void HandleEnded()
    {
        if (Advance == BasisMediaPlaylistAdvance.None) return;
        if (CurrentIndex < 0) return; // the current media didn't come from this playlist
        // Every client sees OnEnded; only the owning client advances, and its
        // SetUrl carries the change to everyone else.
        if (networking != null && networking.HasNetworkID && !networking.IsOwnedLocallyOnClient) return;
        int count = Entries != null ? Entries.Count : 0;
        if (count == 0) return;
        // Scan past blank/invalid entries rather than stalling on one: LoadEntry
        // ignores them without moving CurrentIndex, so retrying the same index
        // every OnEnded would wedge the playlist.
        for (int step = 1; step <= count; step++)
        {
            int next = CurrentIndex + step;
            if (next >= count)
            {
                if (Advance != BasisMediaPlaylistAdvance.LoopAll) return;
                next -= count;
            }
            BasisMediaPlaylistEntry entry = Entries[next];
            if (entry != null && !string.IsNullOrWhiteSpace(entry.Url))
            {
                PlayAt(next);
                return;
            }
        }
    }
}
