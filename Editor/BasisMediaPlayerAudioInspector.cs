using UnityEditor;
using UnityEditor.UIElements;
using UnityEngine.UIElements;

[CustomEditor(typeof(BasisMediaPlayerAudio))]
public class BasisMediaPlayerAudioInspector : Editor
{
    private const string UxmlPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerAudioSDK.uxml";
    private const string UssPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerSDK.uss";

    private BasisMediaPlayerAudio _target;
    private VisualElement _root;

    public override VisualElement CreateInspectorGUI()
    {
        _target = (BasisMediaPlayerAudio)target;
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

        BindByName("TargetAudioSourceField", "TargetAudioSource");
        BindByName("AssignClipOnAwakeField", "AssignClipOnAwake");
        BindByName("ClipNameField", "ClipName");
        BindByName("SampleRateField", "SampleRate");
        BindByName("ChannelCountField", "ChannelCount");
        BindByName("ClipLengthField", "ClipLengthSeconds");
        BindByName("MaxQueuedField", "MaxQueuedFrames");
        BindByName("DropOldestField", "DropOldestOnOverflow");
        BindByName("RebufferField", "RebufferFrames");
        BindByName("AutoPlayField", "AutoPlayOnEnable");
        BindByName("StopOnDisableField", "StopOnDisable");
        BindByName("ClearQueueOnEnableField", "ClearQueueOnEnable");
        BindByName("VolumeGainField", "VolumeGain");
        BindByName("MuteField", "Mute");
        _root.Bind(serializedObject);

        var clearBtn = _root.Q<Button>("ClearQueueButton");
        if (clearBtn != null) clearBtn.clicked += () => { if (_target != null) _target.ClearQueue(); };

        var resetBtn = _root.Q<Button>("ResetAnchorButton");
        if (resetBtn != null) resetBtn.clicked += () => { if (_target != null) _target.ResetSyncAnchor(); };

        return _root;
    }

    private void BindByName(string name, string property)
    {
        if (_root.Q<VisualElement>(name) is IBindable bindable) bindable.bindingPath = property;
    }
}
