using System;
using UnityEditor;
using UnityEditor.UIElements;
using UnityEngine;
using UnityEngine.UIElements;

public class BasisMediaPlayerDebugWindow : EditorWindow
{
    private const string UxmlPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerDebug.uxml";
    private const string UssPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerSDK.uss";

    private BasisMediaPlayer _target;
    private ObjectField _targetField;
    private VisualElement _missingBanner;
    private VisualElement _editModeHint;

    private double _fpsLastTime;
    private ulong _fpsLastDecoded;
    private long _fpsLastPresented;
    private long _fpsLastRendered;
    private float _decodedFps;
    private float _displayedFps;
    private float _renderFps;

    private Label _diagPath, _diagSnapshots;
    private Button _diagAttach, _diagStart, _diagStop, _diagFlush, _diagReveal;

    [MenuItem("Basis/Debug/Media Player Debug")]
    public static void ShowWindow()
    {
        var w = GetWindow<BasisMediaPlayerDebugWindow>("Media Player Debug");
        w.minSize = new Vector2(440, 600);
    }

    public void CreateGUI()
    {
        var tree = AssetDatabase.LoadAssetAtPath<VisualTreeAsset>(UxmlPath);
        var sheet = AssetDatabase.LoadAssetAtPath<StyleSheet>(UssPath);
        if (tree == null)
        {
            rootVisualElement.Add(new HelpBox("MediaPlayerDebug.uxml missing.", HelpBoxMessageType.Error));
            return;
        }

        tree.CloneTree(rootVisualElement);
        if (sheet != null) rootVisualElement.styleSheets.Add(sheet);

        _targetField = rootVisualElement.Q<ObjectField>("TargetPlayer");
        _targetField.objectType = typeof(BasisMediaPlayer);
        _targetField.RegisterValueChangedCallback(evt => _target = evt.newValue as BasisMediaPlayer);

        _missingBanner = rootVisualElement.Q<VisualElement>("MissingTargetBanner");
        _editModeHint = rootVisualElement.Q<VisualElement>("EditModeHint");

        BindDiagnostics();
        rootVisualElement.schedule.Execute(Refresh).Every(250);
    }

    private void BindDiagnostics()
    {
        _diagPath = rootVisualElement.Q<Label>("DiagPathLabel");
        _diagSnapshots = rootVisualElement.Q<Label>("DiagSnapshotsLabel");
        _diagAttach = rootVisualElement.Q<Button>("DiagAttachButton");
        _diagStart = rootVisualElement.Q<Button>("DiagStartButton");
        _diagStop = rootVisualElement.Q<Button>("DiagStopButton");
        _diagFlush = rootVisualElement.Q<Button>("DiagFlushButton");
        _diagReveal = rootVisualElement.Q<Button>("DiagRevealButton");

        _diagAttach.clicked += () =>
        {
            if (_target != null) Undo.AddComponent<BasisMediaPlayerDiagnostics>(_target.gameObject);
        };
        _diagStart.clicked += () =>
        {
            var d = _target != null ? _target.GetComponent<BasisMediaPlayerDiagnostics>() : null;
            if (d != null) d.StartLogging();
        };
        _diagStop.clicked += () =>
        {
            var d = _target != null ? _target.GetComponent<BasisMediaPlayerDiagnostics>() : null;
            if (d != null) d.StopLogging();
        };
        _diagFlush.clicked += () =>
        {
            var d = _target != null ? _target.GetComponent<BasisMediaPlayerDiagnostics>() : null;
            if (d != null) d.Flush();
        };
        _diagReveal.clicked += () =>
        {
            var d = _target != null ? _target.GetComponent<BasisMediaPlayerDiagnostics>() : null;
            if (d == null) return;
            string p = d.ResolvedLogPath;
            if (!string.IsNullOrEmpty(p) && System.IO.File.Exists(p)) EditorUtility.RevealInFinder(p);
            else EditorUtility.RevealInFinder(System.IO.Path.GetDirectoryName(p) ?? Application.persistentDataPath);
        };
    }

    private void Refresh()
    {
        if (_target == null && Application.isPlaying) _target = FindFirstObjectByTypeSafe();
        if (_target != null && _targetField.value != _target) _targetField.SetValueWithoutNotify(_target);

        _missingBanner.style.display = _target == null ? DisplayStyle.Flex : DisplayStyle.None;
        _editModeHint.style.display = Application.isPlaying ? DisplayStyle.None : DisplayStyle.Flex;

        RefreshDiagnosticsControls();
        if (_target == null) return;

        RefreshPlayer();
        RefreshSource();
        RefreshVideoDecode();
        RefreshVideoQueue();
        RefreshClock();
        RefreshAudioDecode();
        RefreshAudioQueue();
        RefreshAudioOutput();
    }

    private void RefreshDiagnosticsControls()
    {
        var diag = _target != null ? _target.GetComponent<BasisMediaPlayerDiagnostics>() : null;
        bool hasTarget = _target != null;
        bool hasDiag = diag != null;
        bool playing = Application.isPlaying;
        bool logging = hasDiag && diag.IsLogging;

        Show(_diagAttach, hasTarget && !hasDiag);
        Show(_diagStart, hasDiag && playing && !logging);
        Show(_diagStop, hasDiag && playing && logging);
        Show(_diagFlush, hasDiag && playing && logging);
        Show(_diagReveal, hasDiag);

        if (_diagPath != null) _diagPath.text = "Path: " + (hasDiag && !string.IsNullOrEmpty(diag.ResolvedLogPath) ? diag.ResolvedLogPath : "(unset)");
        if (_diagSnapshots != null) _diagSnapshots.text = "Snapshots: " + (hasDiag && playing ? diag.SnapshotsWritten.ToString() : "0");
    }

    private void RefreshPlayer()
    {
        SetPill("P_Playing", _target.IsPlaying);
        SetPill("P_Paused", _target.IsPaused);
        SetText("P_FramesPresented", _target.PresentedFrameCount.ToString());
        SetText("P_MaxQueue", _target.MaxQueueLength.ToString());
        SetText("P_LateSkip", _target.LateFrameSkipUs + " us");
        SetText("P_PresOffset", _target.PresentationOffsetUs + " us");
        SetText("P_OverflowPolicy", _target.OverflowPolicy.ToString());

        if (_target.Source == null && _target.NativeEngine == null)
        {
            ShowHelp("P_Help", "Player has no Source assigned. Add a BasisMediaPlayerStreaming or BasisMediaPlayerSynthetic on the same GameObject, or assign player.Source from code.", "bvp-help-warn");
        }
        else if (Application.isPlaying && _target.NativeEngine == null && _target.IsPlaying && _target.PresentedFrameCount == 0 && _target.QueuedFrameCount == 0)
        {
            ShowHelp("P_Help", "Playing but no frames have arrived or been presented. Check the source/transport status below.", "bvp-help-warn");
        }
        else
        {
            HideHelp("P_Help");
        }
    }

    private void RefreshSource()
    {
        var eng = _target.NativeEngine;
        if (eng != null)
        {
            SetText("S_Backend", "OS-codec engine (zero-copy)");
            SetText("S_UrlOrType", eng.Url ?? "(no url)");
            SetPill("S_Running", eng.IsRunning);
            SetText("S_State", eng.State.ToString());
            var sz = eng.VideoSize;
            SetText("S_VideoSize", sz.x > 0 ? $"{sz.x} x {sz.y}" : "(unknown)");
            SetText("S_Position", FormatUs(eng.PositionUs < 0 ? 0 : eng.PositionUs));
            SetPill("S_OutputTex", eng.OutputTexture != null);

            if (Application.isPlaying && eng.State == BasisMediaEngineState.Error)
                ShowHelp("S_Help", "Engine reported an error — see the Console for the message, and verify basis_media_native is built and present under Plugins/.", "bvp-help-error");
            else if (Application.isPlaying && eng.IsRunning && eng.OutputTexture == null)
                ShowHelp("S_Help", "Engine running but no frame published yet (connecting/buffering), or the native library isn't producing frames.", "bvp-help-info");
            else HideHelp("S_Help");
            return;
        }

        var source = _target.Source;
        if (source == null)
        {
            SetText("S_Backend", "(no source assigned)");
            SetText("S_UrlOrType", "—");
            SetPill("S_Running", false);
            SetText("S_State", "—");
            SetText("S_VideoSize", "—");
            SetText("S_Position", "—");
            SetPill("S_OutputTex", false);
            HideHelp("S_Help");
            return;
        }

        SetText("S_Backend", "CPU source");
        SetText("S_UrlOrType", source.GetType().Name);
        SetPill("S_Running", source.IsRunning);

        if (source is BasisSyntheticTestSource synth)
        {
            SetText("S_State", $"{synth.PatternMode}");
            SetText("S_VideoSize", $"{synth.Width} x {synth.Height}");
            SetText("S_Position", $"{synth.FramesEmitted} frames @ {synth.FramesPerSecond} fps");
            SetPill("S_OutputTex", true);
        }
        else
        {
            SetText("S_State", "—");
            SetText("S_VideoSize", "—");
            SetText("S_Position", "—");
            SetPill("S_OutputTex", true);
        }
        HideHelp("S_Help");
    }

    private void RefreshVideoDecode()
    {
        var eng = _target.NativeEngine;
        if (eng == null)
        {
            SetText("VD_Decoder", "(CPU source — OS hardware decode not in use)");
            SetText("VD_DecodedSize", "—");
            SetText("VD_LastPts", "—");
            SetText("VD_OutputTex", "—");
            HideHelp("VD_Help");
            return;
        }

        SetText("VD_Decoder", "OS hardware (Media Foundation / MediaCodec)");
        var sz = eng.VideoSize;
        SetText("VD_DecodedSize", sz.x > 0 ? $"{sz.x} x {sz.y}" : "(none yet)");
        SetText("VD_LastPts", FormatUs(eng.PositionUs < 0 ? 0 : eng.PositionUs));
        var tex = eng.OutputTexture;
        SetText("VD_OutputTex", tex != null ? $"{tex.width} x {tex.height}" : "(none)");

        if (Application.isPlaying && eng.IsRunning && tex == null)
            ShowHelp("VD_Help", "No decoded frame yet. Check that basis_media_native is present for this platform/arch and that the stream URL is reachable.", "bvp-help-warn");
        else HideHelp("VD_Help");
    }

    private void RefreshVideoQueue()
    {
        var eng = _target.NativeEngine;
        if (eng != null)
        {
            string dbg = eng.DebugInfo ?? string.Empty;
            UpdateNativeFps(eng, dbg);

            SetText("VQ_Backend", "OS-codec PTS-paced ring (no CPU queue)");
            SetText("VQ_State", eng.State.ToString());
            long ttffMs = ParseCounter(dbg, "ttff=");
            SetText("VQ_TTFF", ttffMs >= 0 ? $"{ttffMs} ms" : "— (connecting)");

            SetFpsText("VQ_FpsDecoded", _decodedFps);
            SetFpsText("VQ_FpsDisplayed", _displayedFps);
            SetFpsText("VQ_FpsRender", _renderFps);

            long bufMs = ParseCounter(dbg, "buf=");
            long lagMs = ParseCounter(dbg, "lag=");
            SetText("VQ_BufferLabel", $"Buffered ahead ({_target.BufferMode}, target {bufMs} ms)");
            SetText("VQ_BufferValue", $"{lagMs} / {bufMs} ms");
            float bufFill = bufMs > 0 ? Mathf.Clamp01((float)lagMs / bufMs) : 0f;
            string fillClass = bufFill >= 0.75f ? "bvp-bar-fill-good" : (bufFill >= 0.4f ? "bvp-bar-fill-warn" : "bvp-bar-fill-bad");
            SetBar("VQ_BufferFill", bufFill, fillClass);

            SetText("VQ_DepthValue", "(not used on OS-codec path)");
            SetBar("VQ_DepthFill", 0f, null);

            SetText("VQ_ClockNow", "(OS-codec internal)");
            SetText("VQ_HeadPts", "—");
            SetText("VQ_TailPts", "—");
            SetText("VQ_QueueSpan", "—");

            SetText("VQ_DropOverflow", "—");
            SetText("VQ_DropLate", "—");
            SetText("VQ_DropFormat", "—");
            SetText("VQ_DropCatchup", "—");

            SetText("VQ_NativeCounters", string.IsNullOrEmpty(dbg) ? "(no data — is the plugin built?)" : dbg);
            HideHelp("VQ_Help");
            return;
        }

        int depth = _target.QueuedFrameCount;
        int max = Mathf.Max(1, _target.MaxQueueLength);
        float fill = (float)depth / max;

        SetText("VQ_Backend", "CPU queue");
        SetText("VQ_State", _target.IsPlaying ? "Playing" : (_target.IsPaused ? "Paused" : "Idle"));
        SetText("VQ_TTFF", "—");
        SetFpsText("VQ_FpsDecoded", 0);
        SetFpsText("VQ_FpsDisplayed", 0);
        SetFpsText("VQ_FpsRender", 0);

        SetText("VQ_BufferLabel", "(OS-codec path only)");
        SetText("VQ_BufferValue", "—");
        SetBar("VQ_BufferFill", 0f, null);

        SetText("VQ_DepthValue", $"{depth} / {max}");
        SetBar("VQ_DepthFill", fill, null);

        long clockNow = _target.Clock.CurrentMediaTimeUs;
        long headPts = _target.HeadFramePtsUs;
        long tailPts = _target.TailFramePtsUs;

        SetText("VQ_ClockNow", FormatUs(clockNow));
        SetText("VQ_HeadPts", headPts >= 0 ? FormatUs(headPts) + $"  ({DeltaLabel(headPts - clockNow)})" : "(empty)");
        SetText("VQ_TailPts", tailPts > 0 ? FormatUs(tailPts) + $"  ({DeltaLabel(tailPts - clockNow)})" : "—");
        SetText("VQ_QueueSpan", tailPts > 0 ? FormatUs(tailPts - (headPts < 0 ? tailPts : headPts)) : "—");

        SetText("VQ_DropOverflow", _target.OverflowDropCount.ToString());
        SetText("VQ_DropLate", _target.LateSkipCount.ToString() + $" (>{_target.LateFrameSkipUs}us behind)");
        SetText("VQ_DropFormat", _target.FormatErrorCount.ToString());
        SetText("VQ_DropCatchup", _target.CatchUpSkipCount.ToString() + " (informational)");

        SetText("VQ_NativeCounters", "(CPU source — no native counters)");

        if (Application.isPlaying && depth >= max && _target.OverflowDropCount > 0)
        {
            bool headInFuture = headPts > clockNow + 50_000;
            bool clockMaybeStalled = clockNow == 0 && _target.IsPlaying && depth > 0;
            if (clockMaybeStalled)
                ShowHelp("VQ_Help", "Queue full + Clock stuck at 0. The A/V clock never anchored. Most common cause: audio is wired as the master clock but the AudioSource isn't actually playing (or no streaming clip was assigned), so HasMediaTime stays false AND the local wall-clock anchor isn't running either. Verify section 8 (Audio Output) shows 'Is Playing: YES' and 'Has Media Anchor: YES'.", "bvp-help-error");
            else if (headInFuture)
                ShowHelp("VQ_Help", $"Queue is full of FUTURE frames (head is {DeltaLabel(headPts - clockNow)}). The server is sending faster than real time.\nFixes, in order:\n  1. Throttle the server to real-time pacing.\n  2. Set OverflowPolicy = DropNewest so far-future frames are shed instead of near-due ones.\n  3. Raise MaxQueueLength to absorb the burst.", "bvp-help-error");
            else
                ShowHelp("VQ_Help", "Queue full and overflow drops accumulating, but head PTS is near the clock — frames are arriving in tight bursts faster than Update() can drain them. Raise MaxQueueLength.", "bvp-help-warn");
        }
        else if (Application.isPlaying && _target.CatchUpSkipCount > _target.PresentedFrameCount && _target.CatchUpSkipCount > 30)
        {
            ShowHelp("VQ_Help", "Catch-up coalesce dominates: more frames walked-past than presented. Normal for bursty network or clock jumps. Not data loss — earlier frames decoded successfully, only the most recent is rendered.", "bvp-help-info");
        }
        else if (Application.isPlaying && _target.LateSkipCount > 0)
        {
            ShowHelp("VQ_Help", "Late-skip is firing: frames arrived too far behind the clock (set by LateFrameSkipUs). Either the producer is delayed, or the clock has jumped ahead. Set LateFrameSkipUs=0 to disable, or raise it.", "bvp-help-warn");
        }
        else
        {
            HideHelp("VQ_Help");
        }
    }

    private void RefreshClock()
    {
        var clock = _target.Clock;
        SetPill("C_Started", clock.IsStarted);
        SetText("C_CurrentTime", FormatUs(clock.CurrentMediaTimeUs));
        bool hasExternal = clock.ExternalSource != null;
        SetPill("C_HasExternal", hasExternal);
        SetPill("C_ExtHasMedia", hasExternal && clock.ExternalSource.HasMediaTime);
        SetText("C_ExtTime", hasExternal ? FormatUs(clock.ExternalSource.CurrentMediaTimeUs) : "—");
        SetText("C_ExtType", hasExternal ? clock.ExternalSource.GetType().Name : "—");
    }

    private void RefreshAudioDecode()
    {
        var eng = _target.NativeEngine;
        if (eng == null)
        {
            SetText("AD_Decoder", "(CPU source — no OS audio decode)");
            SetText("AD_Format", "—");
            return;
        }
        SetText("AD_Decoder", "OS hardware AAC");
        SetText("AD_Format", eng.TryGetPcmFormat(out int sr, out int ch) ? $"{sr} Hz / {ch} ch" : "(pending first audio frame)");
    }

    private void RefreshAudioQueue()
    {
        var audio = _target.AudioComponent;
        var eng = _target.NativeEngine;

        if (eng == null)
        {
            SetText("AQ_Backend", "(CPU source — no OS audio decode)");
            SetText("AQ_Format", "—");
            SetPill("AQ_SourcePlaying", false);
            SetText("AQ_PcmLevels", "—");
            SetText("AQ_Dropped", "—");
            SetText("AQ_DropPolicy", "—");
            SetBar("AQ_DepthFill", 0f, null);
            SetText("AQ_DepthValue", "—");
            HideHelp("AQ_Help");
            return;
        }

        SetText("AQ_Backend", "OS-codec PCM ring (pulled on audio thread)");
        SetText("AQ_Format", eng.TryGetPcmFormat(out int sr, out int ch) ? $"{sr} Hz / {ch} ch" : "(pending first audio frame)");
        if (audio != null)
        {
            // The native ring is drained directly on the audio thread — there's no
            // managed frame queue, so depth/drop metrics don't apply here.
            SetPill("AQ_SourcePlaying", audio.IsAnyOutputPlaying);
            SetText("AQ_PcmLevels", $"{audio.LastPcmPeak:F3} / {audio.LastPcmRms:F3}");
            SetText("AQ_Dropped", "—");
            SetText("AQ_DropPolicy", "native ring");
            SetBar("AQ_DepthFill", 0f, null);
            SetText("AQ_DepthValue", "n/a (native ring)");
            HideHelp("AQ_Help");
        }
        else
        {
            SetPill("AQ_SourcePlaying", false);
            SetText("AQ_PcmLevels", "—");
            SetText("AQ_Dropped", "—");
            SetText("AQ_DropPolicy", "—");
            SetBar("AQ_DepthFill", 0f, null);
            SetText("AQ_DepthValue", "—");
            ShowHelp("AQ_Help", "No BasisMediaPlayerAudio on the GameObject — add one (with an AudioSource) for sound.", "bvp-help-info");
        }
    }

    private void RefreshAudioOutput()
    {
        var audio = _target.AudioComponent;
        if (audio == null)
        {
            SetText("AO_SourceName", "(no BasisMediaPlayerAudio component)");
            SetPill("AO_IsPlaying", false);
            SetText("AO_Volume", "—");
            SetText("AO_SpatialBlend", "—");
            SetPill("AO_Mute", false);
            SetText("AO_Gain", "—");
            SetText("AO_Clip", "—");
            SetText("AO_Format", "—");
            SetText("AO_Consumed", "—");
            SetPill("AO_HasAnchor", false);
            SetText("AO_MediaTime", "—");
            HideHelp("AO_Help");
            return;
        }

        int outputCount = audio.Outputs != null ? audio.Outputs.Length : 0;
        SetText("AO_SourceName", outputCount > 0 ? $"{outputCount} output(s)" : "(no outputs)");
        SetPill("AO_IsPlaying", audio.IsAnyOutputPlaying);
        SetText("AO_Volume", audio.RepresentativeVolume.ToString("F2"));
        SetText("AO_SpatialBlend", audio.RepresentativeSpatialBlend.ToString("F2"));
        SetPill("AO_Mute", audio.Mute);
        SetText("AO_Gain", audio.VolumeGain.ToString("F2"));
        SetText("AO_Clip", "—");
        SetText("AO_Format", $"{audio.SampleRate} Hz / {audio.ChannelCount} ch");
        SetText("AO_Consumed", audio.ConsumedSampleCount.ToString());
        SetPill("AO_HasAnchor", audio.HasMediaTime);
        SetText("AO_MediaTime", FormatUs(audio.CurrentMediaTimeUs));

        if (outputCount == 0)
            ShowHelp("AO_Help", "No audio output configured. Add one or more AudioSources to 'Outputs', each with a BasisMediaAudioChannel: choose Stereo for a stereo downmix, or a specific channel for surround.", "bvp-help-error");
        else HideHelp("AO_Help");
    }

    private void UpdateNativeFps(BasisNativeVideoSource eng, string dbg)
    {
        double now = EditorApplication.timeSinceStartup;
        ulong decoded = eng.DecodedFrameCount;
        long presented = ParseCounter(dbg, "copy=");
        long rendered = ParseCounter(dbg, "render=");

        if (_fpsLastTime <= 0 || now < _fpsLastTime || decoded < _fpsLastDecoded)
        {
            _fpsLastTime = now;
            _fpsLastDecoded = decoded;
            _fpsLastPresented = presented;
            _fpsLastRendered = rendered;
            return;
        }

        double dt = now - _fpsLastTime;
        if (dt < 0.5) return;

        _decodedFps = (float)((decoded - _fpsLastDecoded) / dt);
        _displayedFps = (float)(Math.Max(0L, presented - _fpsLastPresented) / dt);
        _renderFps = (float)(Math.Max(0L, rendered - _fpsLastRendered) / dt);
        _fpsLastTime = now;
        _fpsLastDecoded = decoded;
        _fpsLastPresented = presented;
        _fpsLastRendered = rendered;
    }

    private static long ParseCounter(string s, string key)
    {
        if (string.IsNullOrEmpty(s)) return 0;
        int i = s.IndexOf(key, StringComparison.Ordinal);
        if (i < 0) return 0;
        i += key.Length;
        int start = i;
        if (i < s.Length && (s[i] == '-' || s[i] == '+')) i++;
        while (i < s.Length && char.IsDigit(s[i])) i++;
        return (i > start && long.TryParse(s.Substring(start, i - start), out long v)) ? v : 0;
    }

    private static string DeltaLabel(long deltaUs)
    {
        if (deltaUs > 0) return $"+{FormatUs(deltaUs)} future";
        if (deltaUs < 0) return $"-{FormatUs(-deltaUs)} past";
        return "now";
    }

    private static string FormatUs(long us)
    {
        if (us <= 0) return "0 us";
        long ms = us / 1000;
        if (ms < 1000) return $"{us} us ({ms} ms)";
        double s = us / 1_000_000.0;
        return $"{us} us ({s:F3} s)";
    }

    private static void Show(VisualElement el, bool show)
    {
        if (el != null) el.style.display = show ? DisplayStyle.Flex : DisplayStyle.None;
    }

    private void SetText(string name, string text)
    {
        var l = rootVisualElement.Q<Label>(name);
        if (l != null) l.text = text;
    }

    private void SetFpsText(string name, float fps)
    {
        var l = rootVisualElement.Q<Label>(name);
        if (l == null) return;
        l.text = $"{fps:F1} fps";
        l.style.color = fps >= 24f ? new Color(0.3f, 0.9f, 0.3f) : (fps >= 12f ? new Color(1f, 0.8f, 0.2f) : new Color(1f, 0.3f, 0.3f));
    }

    private void SetPill(string name, bool value)
    {
        var l = rootVisualElement.Q<Label>(name);
        if (l == null) return;
        l.text = value ? "YES" : "NO";
        l.RemoveFromClassList("bvp-pill-neutral");
        l.RemoveFromClassList("bvp-pill-good");
        l.RemoveFromClassList("bvp-pill-bad");
        l.AddToClassList(value ? "bvp-pill-good" : "bvp-pill-bad");
    }

    private void SetBar(string name, float fill, string fillClass)
    {
        var el = rootVisualElement.Q<VisualElement>(name);
        if (el == null) return;
        fill = Mathf.Clamp01(fill);
        el.style.width = new Length(fill * 100f, LengthUnit.Percent);
        if (fillClass != null)
        {
            el.RemoveFromClassList("bvp-bar-fill-good");
            el.RemoveFromClassList("bvp-bar-fill-warn");
            el.RemoveFromClassList("bvp-bar-fill-bad");
            el.AddToClassList(fillClass);
        }
    }

    private void ShowHelp(string name, string message, string cssClass)
    {
        var l = rootVisualElement.Q<Label>(name);
        if (l == null) return;
        l.text = message;
        l.RemoveFromClassList("bvp-help-info");
        l.RemoveFromClassList("bvp-help-warn");
        l.RemoveFromClassList("bvp-help-error");
        l.AddToClassList(cssClass);
        l.style.display = DisplayStyle.Flex;
    }

    private void HideHelp(string name)
    {
        var l = rootVisualElement.Q<Label>(name);
        if (l != null) l.style.display = DisplayStyle.None;
    }

    private BasisMediaPlayer FindFirstObjectByTypeSafe()
    {
#if UNITY_2023_1_OR_NEWER
        return FindAnyObjectByType<BasisMediaPlayer>(FindObjectsInactive.Include);
#else
        var all = Resources.FindObjectsOfTypeAll<BasisMediaPlayer>();
        for (int i = 0; i < all.Length; i++)
        {
            var p = all[i];
            if (p != null && p.gameObject.scene.IsValid()) return p;
        }
        return null;
#endif
    }
}
