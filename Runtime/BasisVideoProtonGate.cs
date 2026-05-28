using System;
using System.Collections.Generic;
using Basis.BasisUI;
using Basis.Scripts.Device_Management;

// Gate for starting the OS-codec video engine under Wine/Proton.
//
// The native plugin uses Windows Media Foundation + D3D11 keyed-mutex shared
// textures. Under a Windows-on-Linux compatibility layer (Wine, and Valve's
// Proton) those may be missing or incomplete, and initializing them can hard-
// crash the process — a native access violation that managed try/catch cannot
// recover. So on Wine/Proton we never touch the native plugin until the user
// explicitly opts in via a dialog. The decision is made once per session and
// shared by every BasisMediaPlayer; if Media Foundation turns out to be absent
// even after opting in, loading fails cleanly (caught) rather than playing.
//
// On native Windows / Android / Quest this gate is transparent: RequiresGate is
// false and Request() runs the "allowed" path immediately.
public static class BasisVideoProtonGate
{
    private enum Decision { Unknown, Allowed, Denied }

    private static Decision _decision = Decision.Unknown;
    private static bool _prompting;
    private static readonly List<(Action onAllowed, Action onDenied)> _waiters = new List<(Action, Action)>();

    // True only when we're on a Wine/Proton host and must ask before loading.
    public static bool RequiresGate => BasisProtonDetection.IsWine;

    // True once the user has answered the prompt this session.
    public static bool Decided => _decision != Decision.Unknown;

    // True when the engine is cleared to load (native Windows, or the user said yes).
    public static bool Allowed => !RequiresGate || _decision == Decision.Allowed;

    // Run onAllowed if the engine may start now, onDenied if it must not. When the
    // decision is still pending the callbacks are queued and resolved together once
    // the user answers, so multiple players only ever raise a single dialog.
    public static void Request(Action onAllowed, Action onDenied)
    {
        if (!RequiresGate || _decision == Decision.Allowed) { onAllowed?.Invoke(); return; }
        if (_decision == Decision.Denied) { onDenied?.Invoke(); return; }

        _waiters.Add((onAllowed, onDenied));
        if (!_prompting) Prompt();
    }

    // Re-arm the prompt (e.g. from a settings toggle) so the next load asks again.
    public static void Reset() => _decision = Decision.Unknown;

    private static void Prompt()
    {
        _prompting = true;
        string ver = BasisProtonDetection.WineVersion;
        string host = string.IsNullOrEmpty(ver) ? "Proton/Wine" : $"Proton/Wine ({ver})";

        if (!BasisMainMenu.Instance) BasisMainMenu.Open();

        var panel = BasisMenuDialoguePanel.CreateNew(
            "Media Player",
            $"This session is running through {host}. The media player needs Windows Media Foundation, " +
            "which may not be installed in this compatibility layer. If it is missing, video will not play " +
            "(and initializing it can be unstable here). Try to load the media player anyway?",
            "Load", "Skip",
            Resolve);

        if (panel == null)
        {
            // No menu to ask with yet: skip the loads waiting now (fail safe, no
            // crash) but leave the decision Unknown so a later load re-asks once the
            // menu is available — don't lock the whole session out of video.
            _prompting = false;
            var pending = _waiters.ToArray();
            _waiters.Clear();
            foreach (var w in pending) w.onDenied?.Invoke();
        }
    }

    private static void Resolve(bool accepted)
    {
        _decision = accepted ? Decision.Allowed : Decision.Denied;
        _prompting = false;

        var pending = _waiters.ToArray();
        _waiters.Clear();
        foreach (var w in pending)
        {
            if (accepted) w.onAllowed?.Invoke();
            else w.onDenied?.Invoke();
        }
    }
}
