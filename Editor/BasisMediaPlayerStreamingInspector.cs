using UnityEditor;
using UnityEditor.UIElements;
using UnityEngine;
using UnityEngine.UIElements;

[CustomEditor(typeof(BasisMediaPlayerStreaming))]
public class BasisMediaPlayerStreamingInspector : Editor
{
    private const string UxmlPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerStreamingSDK.uxml";
    private const string UssPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerSDK.uss";

    private BasisMediaPlayerStreaming _target;
    private VisualElement _root;
    private TextField _pcUrl, _questUrl;
    private Toggle _autoSelect;
    private Button _configureBtn;

    public override VisualElement CreateInspectorGUI()
    {
        _target = (BasisMediaPlayerStreaming)target;
        _root = new VisualElement();

        var tree = AssetDatabase.LoadAssetAtPath<VisualTreeAsset>(UxmlPath);
        var sheet = AssetDatabase.LoadAssetAtPath<StyleSheet>(UssPath);
        if (tree == null)
        {
            _root.Add(new HelpBox("MediaPlayerStreamingSDK.uxml missing.", HelpBoxMessageType.Error));
            return _root;
        }
        tree.CloneTree(_root);
        if (sheet != null) _root.styleSheets.Add(sheet);

        BindByName("StreamUrlField", "StreamUrl");
        BindByName("AutoSelectField", "AutoSelectPerPlatform");
        BindByName("PcUrlField", "PcUrl");
        BindByName("QuestUrlField", "QuestUrl");
        BindByName("ConfigureOnStartField", "ConfigureOnStart");
        _root.Bind(serializedObject);

        _pcUrl = _root.Q<TextField>("PcUrlField");
        _questUrl = _root.Q<TextField>("QuestUrlField");
        _autoSelect = _root.Q<Toggle>("AutoSelectField");

        _autoSelect.RegisterValueChangedCallback(_ => RefreshAutoSelectVisibility());
        RefreshAutoSelectVisibility();

        _configureBtn = _root.Q<Button>("ConfigureButton");
        if (_configureBtn != null) _configureBtn.clicked += () =>
        {
            if (_target != null) _target.Configure();
        };

        var debugBtn = _root.Q<Button>("OpenDebugButton");
        if (debugBtn != null) debugBtn.clicked += BasisMediaPlayerDebugWindow.ShowWindow;

        _root.schedule.Execute(RefreshPlayButton).Every(250);
        return _root;
    }

    private void RefreshAutoSelectVisibility()
    {
        bool show = _autoSelect != null && _autoSelect.value;
        if (_pcUrl != null) _pcUrl.style.display = show ? DisplayStyle.Flex : DisplayStyle.None;
        if (_questUrl != null) _questUrl.style.display = show ? DisplayStyle.Flex : DisplayStyle.None;
    }

    private void RefreshPlayButton()
    {
        if (_configureBtn != null) _configureBtn.style.display = Application.isPlaying ? DisplayStyle.Flex : DisplayStyle.None;
    }

    private void BindByName(string name, string property)
    {
        if (_root.Q<VisualElement>(name) is IBindable bindable) bindable.bindingPath = property;
    }
}
