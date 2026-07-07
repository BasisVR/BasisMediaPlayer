using UnityEditor;
using UnityEditor.UIElements;
using UnityEngine;
using UnityEngine.UIElements;

[CustomEditor(typeof(BasisMediaPlayerPlaylist))]
public class BasisMediaPlayerPlaylistInspector : Editor
{
    private const string UxmlPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerPlaylistSDK.uxml";
    private const string UssPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerSDK.uss";

    private BasisMediaPlayerPlaylist _target;
    private VisualElement _root;
    private Label _status;
    private Button _previousBtn, _nextBtn;

    public override VisualElement CreateInspectorGUI()
    {
        _target = (BasisMediaPlayerPlaylist)target;
        _root = new VisualElement();

        var tree = AssetDatabase.LoadAssetAtPath<VisualTreeAsset>(UxmlPath);
        var sheet = AssetDatabase.LoadAssetAtPath<StyleSheet>(UssPath);
        if (tree == null)
        {
            _root.Add(new HelpBox("MediaPlayerPlaylistSDK.uxml missing.", HelpBoxMessageType.Error));
            return _root;
        }
        tree.CloneTree(_root);
        if (sheet != null) _root.styleSheets.Add(sheet);
        _root.Bind(serializedObject);

        _status = _root.Q<Label>("StatusLabel");
        _previousBtn = _root.Q<Button>("PreviousButton");
        _nextBtn = _root.Q<Button>("NextButton");

        if (_previousBtn != null) _previousBtn.clicked += () => { if (_target != null) _target.Previous(); };
        if (_nextBtn != null) _nextBtn.clicked += () => { if (_target != null) _target.Next(); };

        _root.schedule.Execute(Refresh).Every(250);
        Refresh();
        return _root;
    }

    private void Refresh()
    {
        bool playing = Application.isPlaying;
        DisplayStyle buttons = playing ? DisplayStyle.Flex : DisplayStyle.None;
        if (_previousBtn != null) _previousBtn.style.display = buttons;
        if (_nextBtn != null) _nextBtn.style.display = buttons;

        if (_status == null || _target == null) return;
        int count = _target.Entries != null ? _target.Entries.Count : 0;
        if (!playing)
        {
            _status.text = count == 1 ? "1 entry" : $"{count} entries";
            return;
        }
        int i = _target.CurrentIndex;
        if (i < 0 || i >= count)
        {
            _status.text = $"Nothing loaded from this playlist yet ({count} entries).";
            return;
        }
        var entry = _target.Entries[i];
        string name = entry == null ? "" : (!string.IsNullOrEmpty(entry.DisplayName) ? entry.DisplayName : entry.Url);
        _status.text = $"Entry {i + 1}/{count} — {name}";
    }
}
