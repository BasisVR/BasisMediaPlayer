using UnityEditor;
using UnityEditor.UIElements;
using UnityEngine;
using UnityEngine.UIElements;

[CustomEditor(typeof(BasisMediaPlayer))]
public class BasisMediaPlayerInspector : Editor
{
    private const string UxmlPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerSDK.uxml";
    private const string UssPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerSDK.uss";

    private VisualElement _root;
    private VisualElement _nowPlayingCard;
    private VisualElement _titleRow, _uploaderRow, _fileRow, _durationRow;
    private Label _titleValue, _uploaderValue, _fileValue, _durationValue;

    public override VisualElement CreateInspectorGUI()
    {
        _root = new VisualElement();

        var tree = AssetDatabase.LoadAssetAtPath<VisualTreeAsset>(UxmlPath);
        var sheet = AssetDatabase.LoadAssetAtPath<StyleSheet>(UssPath);
        if (tree == null)
        {
            _root.Add(new HelpBox("MediaPlayerSDK.uxml missing.", HelpBoxMessageType.Error));
            return _root;
        }

        tree.CloneTree(_root);
        if (sheet != null) _root.styleSheets.Add(sheet);

        BindFields();
        BindActions();

        _nowPlayingCard = _root.Q<VisualElement>("NowPlayingCard");
        _titleRow = _root.Q<VisualElement>("NowPlayingTitleRow");
        _uploaderRow = _root.Q<VisualElement>("NowPlayingUploaderRow");
        _fileRow = _root.Q<VisualElement>("NowPlayingFileRow");
        _durationRow = _root.Q<VisualElement>("NowPlayingDurationRow");
        _titleValue = _root.Q<Label>("NowPlayingTitle");
        _uploaderValue = _root.Q<Label>("NowPlayingUploader");
        _fileValue = _root.Q<Label>("NowPlayingFile");
        _durationValue = _root.Q<Label>("NowPlayingDuration");
        _root.schedule.Execute(RefreshNowPlaying).Every(250);
        RefreshNowPlaying();

        return _root;
    }

    private void RefreshNowPlaying()
    {
        if (_nowPlayingCard == null) return;
        var player = target as BasisMediaPlayer;
        BasisMediaMetadata meta = Application.isPlaying && player != null ? player.Metadata : null;
        if (meta == null)
        {
            _nowPlayingCard.style.display = DisplayStyle.None;
            return;
        }
        _nowPlayingCard.style.display = DisplayStyle.Flex;
        SetRow(_titleRow, _titleValue, meta.Title);
        SetRow(_uploaderRow, _uploaderValue, meta.Uploader);
        SetRow(_fileRow, _fileValue, meta.FileName);
        SetRow(_durationRow, _durationValue, meta.Duration.HasValue ? FormatDuration(meta.Duration.Value) : null);
    }

    private static void SetRow(VisualElement row, Label value, string text)
    {
        bool show = !string.IsNullOrEmpty(text);
        if (row != null) row.style.display = show ? DisplayStyle.Flex : DisplayStyle.None;
        if (value != null) value.text = show ? text : string.Empty;
    }

    private static string FormatDuration(System.TimeSpan d)
        => d.TotalHours >= 1 ? d.ToString(@"h\:mm\:ss") : d.ToString(@"m\:ss");

    private void BindFields()
    {
        serializedObject.Update();

        BindByName("DisplayNameField", "DisplayName");
        BindByName("MaxQueueLengthField", "MaxQueueLength");
        BindByName("OverflowPolicyField", "OverflowPolicy");
        BindByName("LateFrameSkipField", "LateFrameSkipUs");
        BindByName("BufferModeField", "BufferMode");
        BindByName("BufferMillisecondsField", "BufferMilliseconds");

        BindByName("AutoPlayField", "AutoPlayOnSourceAssigned");
        BindByName("StopOnDisableField", "StopOnDisable");
        BindByName("LoopField", "Loop");
        BindByName("LoopRestartDelayField", "LoopRestartDelaySeconds");
        BindByName("PresentationOffsetField", "PresentationOffsetUs");

        BindByName("VolumeField", "Volume");
        BindByName("MuteField", "Mute");
        BindByName("PlaybackRateField", "PlaybackRate");

        BindByName("FlipScreenshotField", "FlipVerticallyForScreenshot");
        BindByName("DvrEnabledField", "DvrEnabled");
        BindByName("DvrWindowField", "DvrWindowSeconds");

        BindByName("VerboseLoggingField", "VerboseLogging");

        _root.Bind(serializedObject);
    }

    private void BindByName(string name, string property)
    {
        if (_root.Q<VisualElement>(name) is IBindable bindable) bindable.bindingPath = property;
    }

    private void BindActions()
    {
        var debugBtn = _root.Q<Button>("OpenDebugWindowButton");
        if (debugBtn != null) debugBtn.clicked += BasisMediaPlayerDebugWindow.ShowWindow;
    }
}
