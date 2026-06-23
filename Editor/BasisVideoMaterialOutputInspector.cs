using UnityEditor;
using UnityEditor.UIElements;
using UnityEngine.UIElements;

[CustomEditor(typeof(BasisVideoMaterialOutput))]
public class BasisVideoMaterialOutputInspector : Editor
{
    private const string UxmlPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/VideoMaterialOutputSDK.uxml";
    private const string UssPath = "Packages/com.basis.mediaplayer/Editor/StyleSheets/MediaPlayerSDK.uss";

    private VisualElement _root;

    public override VisualElement CreateInspectorGUI()
    {
        _root = new VisualElement();

        var tree = AssetDatabase.LoadAssetAtPath<VisualTreeAsset>(UxmlPath);
        var sheet = AssetDatabase.LoadAssetAtPath<StyleSheet>(UssPath);
        if (tree == null)
        {
            _root.Add(new HelpBox("VideoMaterialOutputSDK.uxml missing.", HelpBoxMessageType.Error));
            return _root;
        }
        tree.CloneTree(_root);
        if (sheet != null) _root.styleSheets.Add(sheet);

        var playerField = _root.Q<ObjectField>("PlayerField");
        if (playerField != null) playerField.objectType = typeof(BasisMediaPlayer);

        BindByName("PlayerField", "Player");
        BindByName("TargetRendererField", "TargetRenderer");
        BindByName("MaterialIndexField", "MaterialIndex");
        BindByName("TexturePropertyField", "TexturePropertyName");
        BindByName("UseSharedMaterialField", "UseSharedMaterial");
        BindByName("AdditionalTargetsField", "AdditionalTargets");
        BindByName("PlaceholderField", "PlaceholderTexture");
        BindByName("RestoreOnEndedField", "RestorePlaceholderOnEnded");
        BindByName("FlipVerticallyField", "FlipVertically");
        BindByName("FlipHorizontallyField", "FlipHorizontally");
        BindByName("ProjectionModeField", "ProjectionMode");
        BindByName("StereoEyeField", "StereoEye");
        BindByName("AspectModeField", "AspectMode");
        BindByName("DisplayAspectOverrideField", "DisplayAspectOverride");
        BindByName("PictureField", "Picture");
        _root.Bind(serializedObject);

        return _root;
    }

    private void BindByName(string name, string property)
    {
        if (_root.Q<VisualElement>(name) is IBindable bindable) bindable.bindingPath = property;
    }
}
