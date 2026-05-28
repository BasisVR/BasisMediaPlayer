using System;
using System.Collections.Generic;

public static class BasisMediaPlayerRegistry
{
    private static readonly List<BasisMediaPlayer> players = new List<BasisMediaPlayer>();

    public static IReadOnlyList<BasisMediaPlayer> Players => players;
    public static int Count => players.Count;

    public static event Action OnChanged;

    public static void Add(BasisMediaPlayer player)
    {
        if (player == null) return;
        if (players.Contains(player)) return;
        players.Add(player);
        OnChanged?.Invoke();
    }

    public static void Remove(BasisMediaPlayer player)
    {
        if (player == null) return;
        if (players.Remove(player)) OnChanged?.Invoke();
    }
}
