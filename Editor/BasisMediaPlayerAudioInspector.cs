using UnityEditor;
using UnityEditor.UIElements;
using UnityEngine.UIElements;

[CustomEditor(typeof(BasisMediaPlayerAudio))]
public class BasisMediaPlayerAudioInspector : Editor
{
    private const string UxmlPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerAudioSDK.uxml";
    private const string UssPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerSDK.uss";

    private VisualElement _root;

    public override VisualElement CreateInspectorGUI()
    {
        _root = new VisualElement();

        var tree = AssetDatabase.LoadAssetAtPath<VisualTreeAsset>(UxmlPath);
        var sheet = AssetDatabase.LoadAssetAtPath<StyleSheet>(UssPath);
        if (tree == null)
        {
            _root.Add(new HelpBox("MediaPlayerAudioSDK.uxml missing.", HelpBoxMessageType.Error));
            return _root;
        }
        tree.CloneTree(_root);
        if (sheet != null) _root.styleSheets.Add(sheet);

        BindByName("OutputsField", "Outputs");
        BindByName("SampleRateField", "SampleRate");
        BindByName("ChannelCountField", "ChannelCount");
        BindByName("ClipLengthField", "ClipLengthSeconds");
        BindByName("AutoPlayField", "AutoPlayOnEnable");
        BindByName("StopOnDisableField", "StopOnDisable");
        BindByName("VolumeGainField", "VolumeGain");
        BindByName("MuteField", "Mute");
        _root.Bind(serializedObject);

        return _root;
    }

    private void BindByName(string name, string property)
    {
        if (_root.Q<VisualElement>(name) is IBindable bindable) bindable.bindingPath = property;
    }
}
