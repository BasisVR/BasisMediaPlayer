using System.IO;
using UnityEditor;
using UnityEngine;

public static class BasisVideoScreenMeshGenerator
{
    const string OutputPath = "Packages/com.basis.mediaplayer/Meshes/VideoScreen16x9.asset";

    // Bakes the legacy MediaPlayerStreaming prefab's visible dimensions (Plane 10x10 *
    // local scale (1.4463681, _, 0.813582) after a 90 deg X rotation) into a real mesh
    // so the prefab transform can be identity rotation + (1,1,1) scale. 16:9 aspect.
    const float Width = 14.463681f;
    const float Height = 8.13582f;

    [MenuItem("Basis/Media Player/Generate 16:9 Screen Mesh")]
    public static void Generate()
    {
        string dir = Path.GetDirectoryName(OutputPath).Replace('\\', '/');
        if (!AssetDatabase.IsValidFolder(dir))
        {
            Directory.CreateDirectory(dir);
            AssetDatabase.Refresh();
        }

        float hw = Width * 0.5f;
        float hh = Height * 0.5f;

        Mesh mesh = AssetDatabase.LoadAssetAtPath<Mesh>(OutputPath);
        bool created = mesh == null;
        if (created) mesh = new Mesh();
        mesh.name = "VideoScreen16x9";
        mesh.Clear();

        // 180 deg Y rotation baked in: X negated, normal flipped to -Z. Triangle winding
        // stays the same because rotating vertex positions already flips geometric winding.
        mesh.vertices = new[]
        {
            new Vector3( hw, -hh, 0f),
            new Vector3(-hw, -hh, 0f),
            new Vector3(-hw,  hh, 0f),
            new Vector3( hw,  hh, 0f),
        };
        mesh.uv = new[]
        {
            new Vector2(0f, 0f),
            new Vector2(1f, 0f),
            new Vector2(1f, 1f),
            new Vector2(0f, 1f),
        };
        mesh.normals = new[]
        {
            Vector3.back,
            Vector3.back,
            Vector3.back,
            Vector3.back,
        };
        mesh.triangles = new[] { 0, 1, 2, 0, 2, 3 };
        mesh.RecalculateTangents();
        mesh.RecalculateBounds();
        mesh.UploadMeshData(false);

        if (created)
        {
            AssetDatabase.CreateAsset(mesh, OutputPath);
        }
        else
        {
            EditorUtility.SetDirty(mesh);
        }
        AssetDatabase.SaveAssets();
        AssetDatabase.ImportAsset(OutputPath, ImportAssetOptions.ForceUpdate);
        EditorGUIUtility.PingObject(mesh);
        Debug.Log($"[BasisVideoScreen] Wrote {Width} x {Height} quad mesh to {OutputPath}");
    }
}
