using System.IO;
using UnityEditor;
using UnityEditor.UIElements;
using UnityEngine;
using UnityEngine.UIElements;

[CustomEditor(typeof(BasisMediaPlayerDiagnostics))]
public class BasisMediaPlayerDiagnosticsInspector : Editor
{
    private const string UxmlPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerDiagnosticsSDK.uxml";
    private const string UssPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerSDK.uss";

    private BasisMediaPlayerDiagnostics _target;
    private VisualElement _root;
    private Label _isLogging, _snapshots, _resolvedPath, _editHint;
    private Button _start, _stop, _flush, _reveal;

    public override VisualElement CreateInspectorGUI()
    {
        _target = (BasisMediaPlayerDiagnostics)target;
        _root = new VisualElement();

        var tree = AssetDatabase.LoadAssetAtPath<VisualTreeAsset>(UxmlPath);
        var sheet = AssetDatabase.LoadAssetAtPath<StyleSheet>(UssPath);
        if (tree == null)
        {
            _root.Add(new HelpBox("MediaPlayerDiagnosticsSDK.uxml missing.", HelpBoxMessageType.Error));
            return _root;
        }
        tree.CloneTree(_root);
        if (sheet != null) _root.styleSheets.Add(sheet);

        BindByName("AutoStartField", "AutoStart");
        BindByName("SnapshotsPerSecondField", "SnapshotsPerSecond");
        BindByName("FlushEveryField", "FlushEveryNSnapshots");
        BindByName("AppendField", "AppendBetweenSessions");
        BindByName("LogPathOverrideField", "LogPathOverride");
        _root.Bind(serializedObject);

        _isLogging = _root.Q<Label>("StatusIsLogging");
        _snapshots = _root.Q<Label>("StatusSnapshots");
        _resolvedPath = _root.Q<Label>("StatusResolvedPath");
        _editHint = _root.Q<Label>("StatusEditModeHint");

        _start = _root.Q<Button>("ActStartButton");
        _stop = _root.Q<Button>("ActStopButton");
        _flush = _root.Q<Button>("ActFlushButton");
        _reveal = _root.Q<Button>("ActRevealButton");

        _start.clicked += () => { if (PlayModeOnly()) _target.StartLogging(); };
        _stop.clicked += () => { if (PlayModeOnly()) _target.StopLogging(); };
        _flush.clicked += () => { if (PlayModeOnly()) _target.Flush(); };
        _reveal.clicked += () =>
        {
            if (_target == null) return;
            string p = _target.ResolvedLogPath;
            if (!string.IsNullOrEmpty(p) && File.Exists(p)) EditorUtility.RevealInFinder(p);
            else EditorUtility.RevealInFinder(Path.GetDirectoryName(p) ?? Application.persistentDataPath);
        };

        _root.schedule.Execute(RefreshStatus).Every(250);
        return _root;
    }

    private bool PlayModeOnly()
    {
        if (Application.isPlaying && _target != null) return true;
        Debug.LogWarning("BasisMediaPlayerDiagnostics start/stop/flush only run in Play Mode.");
        return false;
    }

    private void RefreshStatus()
    {
        if (_target == null) _target = (BasisMediaPlayerDiagnostics)target;
        if (_target == null) return;

        bool live = Application.isPlaying;
        if (_editHint != null) _editHint.style.display = live ? DisplayStyle.None : DisplayStyle.Flex;

        SetPill(_isLogging, _target.IsLogging, live);
        SetText(_snapshots, live ? _target.SnapshotsWritten.ToString() : "—");
        SetText(_resolvedPath, string.IsNullOrEmpty(_target.ResolvedLogPath) ? "(unset)" : _target.ResolvedLogPath);

        Show(_start, live && !_target.IsLogging);
        Show(_stop, live && _target.IsLogging);
        Show(_flush, live && _target.IsLogging);
    }

    private static void Show(VisualElement el, bool show)
    {
        if (el != null) el.style.display = show ? DisplayStyle.Flex : DisplayStyle.None;
    }

    private static void SetText(Label l, string v) { if (l != null) l.text = v; }

    private static void SetPill(Label l, bool value, bool live)
    {
        if (l == null) return;
        l.RemoveFromClassList("bvp-pill-neutral");
        l.RemoveFromClassList("bvp-pill-good");
        l.RemoveFromClassList("bvp-pill-bad");
        if (!live) { l.text = "—"; l.AddToClassList("bvp-pill-neutral"); return; }
        l.text = value ? "YES" : "NO";
        l.AddToClassList(value ? "bvp-pill-good" : "bvp-pill-bad");
    }

    private void BindByName(string name, string property)
    {
        if (_root.Q<VisualElement>(name) is IBindable bindable) bindable.bindingPath = property;
    }
}
