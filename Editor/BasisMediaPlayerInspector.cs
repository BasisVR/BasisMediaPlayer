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
    private Label _nowPlayingTitle, _nowPlayingDetail;

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
        _nowPlayingTitle = _root.Q<Label>("NowPlayingTitle");
        _nowPlayingDetail = _root.Q<Label>("NowPlayingDetail");
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
        if (_nowPlayingTitle != null) _nowPlayingTitle.text = meta.Title;
        if (_nowPlayingDetail != null)
        {
            string detail = !string.IsNullOrEmpty(meta.Uploader) ? meta.Uploader : meta.FileName;
            _nowPlayingDetail.text = detail ?? string.Empty;
            _nowPlayingDetail.style.display = string.IsNullOrEmpty(detail) ? DisplayStyle.None : DisplayStyle.Flex;
        }
    }

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
