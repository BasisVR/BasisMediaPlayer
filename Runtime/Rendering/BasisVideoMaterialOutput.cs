using UnityEngine;

// Renderer/material sink for BasisMediaPlayer. Subscribes to the player's
// OnOutputTextureChanged and binds the current OutputTexture to a target
// Renderer's material property.
//
// Two binding strategies:
//   * UseSharedMaterial = false (default): assigns to TargetRenderer.material,
//     which clones the shared material the first time it's accessed. Safe for
//     per-instance video; avoids stomping other renderers sharing the asset.
//   * UseSharedMaterial = true: assigns to TargetRenderer.sharedMaterial,
//     mutating the project asset. Use only when you intend every renderer
//     sharing this material to receive the same video texture.
//
// On disable, the original texture is restored so editor scenes don't end up
// with the runtime video texture baked into the material.
[DisallowMultipleComponent]
public sealed class BasisVideoMaterialOutput : MonoBehaviour
{
    [Tooltip("Player to subscribe to. If unassigned, GetComponentInParent<BasisMediaPlayer>() is used.")]
    public BasisMediaPlayer Player;

    [Tooltip("Renderer whose material receives the OutputTexture.")]
    public Renderer TargetRenderer;

    [Tooltip("Index into TargetRenderer.materials for multi-material renderers. 0 by default.")]
    [Min(0)] public int MaterialIndex = 0;

    [Tooltip("Shader texture property name to bind the frame texture to. URP uses _BaseMap; legacy BiRP uses _MainTex.")]
    public string TexturePropertyName = "_BaseMap";

    [Tooltip("If true, the renderer's sharedMaterial is mutated — every renderer using that material will see the video. Otherwise a per-instance copy is created via Renderer.material.")]
    public bool UseSharedMaterial = false;

    [Tooltip("Optional fallback bound when the player has no output texture (before first frame, after Stop). Leave empty to leave the material's prior texture in place.")]
    public Texture PlaceholderTexture;

    [Tooltip("If true, the placeholder is rebound whenever the player raises OnEnded.")]
    public bool RestorePlaceholderOnEnded = true;

    [Tooltip("Flip the video vertically. Native GPU textures (D3D11/D3D12) are top-left origin and come in upside-down for Unity sampling, so this defaults ON. Toggle if your content/platform is already the right way up.")]
    public bool FlipVertically = true;

    [Tooltip("Flip the video horizontally. Use when the screen mesh's UV winding presents the video mirrored to the viewer.")]
    public bool FlipHorizontally = false;

    [Header("Projection")]
    [Tooltip("How the source frame is laid out. Mono/SBS/OU select the correct UV half via UV transform; Equirect360/VR180/Fisheye set a shader keyword (BASIS_PROJ_*) for compatible shaders.")]
    public BasisVideoProjectionMode ProjectionMode = BasisVideoProjectionMode.Mono;

    [Tooltip("Which half of a stereo SBS/OU stream to display on a non-VR or single-eye target. Stereo VR rendering on a per-eye material would use UnityStereoEyeIndex in the shader instead.")]
    public BasisVideoStereoEye StereoEye = BasisVideoStereoEye.Left;

    [Header("Aspect")]
    [Tooltip("Original = sample untransformed; Stretch = same; FitInside = letterbox to preserve source aspect; FitOutside = crop to fill display; PixelPerfect = 1:1 source pixels at the chosen scale.")]
    public BasisVideoAspectMode AspectMode = BasisVideoAspectMode.Original;

    [Tooltip("Display aspect ratio for FitInside/FitOutside/PixelPerfect. Defaults to the renderer's local-bounds aspect when 0. Override for non-square quads or skewed targets.")]
    [Min(0f)] public float DisplayAspectOverride = 0f;

    [Header("Picture")]
    [Tooltip("Per-output picture controls. Shader must read _BasisBrightness/_BasisContrast/_BasisSaturation/_BasisGamma from the MaterialPropertyBlock; unsupported shaders just ignore them.")]
    public BasisVideoPicture Picture = BasisVideoPicture.Default;

    private static readonly int FallbackPropertyId = Shader.PropertyToID("_MainTex");
    private static readonly int PropBrightness = Shader.PropertyToID("_BasisBrightness");
    private static readonly int PropContrast = Shader.PropertyToID("_BasisContrast");
    private static readonly int PropSaturation = Shader.PropertyToID("_BasisSaturation");
    private static readonly int PropGamma = Shader.PropertyToID("_BasisGamma");

    private Texture originalTexture;
    private Vector2 originalScale = Vector2.one;
    private Vector2 originalOffset = Vector2.zero;
    private bool capturedOriginal;
    private int propertyId;
    private MaterialPropertyBlock mpb;
    private string activeShaderKeyword;

    private void Reset()
    {
        if (TargetRenderer == null) TargetRenderer = GetComponent<Renderer>();
        if (Player == null) Player = GetComponentInParent<BasisMediaPlayer>();
    }

    private void OnEnable()
    {
        if (Player == null) Player = GetComponentInParent<BasisMediaPlayer>();
        if (Player == null)
        {
            BasisDebug.LogWarning("BasisVideoMaterialOutput: no BasisMediaPlayer found in parents and Player field is empty.", BasisDebug.LogTag.Video);
            return;
        }
        if (TargetRenderer == null)
        {
            BasisDebug.LogWarning("BasisVideoMaterialOutput: TargetRenderer is null; cannot bind output texture.", BasisDebug.LogTag.Video);
            return;
        }

        propertyId = !string.IsNullOrEmpty(TexturePropertyName)
            ? Shader.PropertyToID(TexturePropertyName)
            : FallbackPropertyId;

        CaptureOriginal();

        Player.OnOutputTextureChanged += HandleTextureChanged;
        Player.OnEnded += HandleEnded;

        HandleTextureChanged(Player.OutputTexture);
    }

    private void OnDisable()
    {
        if (Player != null)
        {
            Player.OnOutputTextureChanged -= HandleTextureChanged;
            Player.OnEnded -= HandleEnded;
        }
        RestoreOriginal();
    }

    private void HandleTextureChanged(Texture texture)
    {
        if (TargetRenderer == null) return;
        if (texture == null) texture = PlaceholderTexture;
        SetTexture(texture);
    }

    private void HandleEnded()
    {
        if (!RestorePlaceholderOnEnded || TargetRenderer == null) return;
        SetTexture(PlaceholderTexture);
    }

    private void CaptureOriginal()
    {
        if (capturedOriginal) return;
        var material = GetMaterial();
        if (material == null) return;
        if (material.HasProperty(propertyId))
        {
            originalTexture = material.GetTexture(propertyId);
            originalScale = material.GetTextureScale(propertyId);
            originalOffset = material.GetTextureOffset(propertyId);
            capturedOriginal = true;
        }
    }

    private void RestoreOriginal()
    {
        if (!capturedOriginal || TargetRenderer == null) return;
        var material = GetMaterial();
        if (material == null) return;
        if (material.HasProperty(propertyId))
        {
            material.SetTexture(propertyId, originalTexture);
            material.SetTextureScale(propertyId, originalScale);
            material.SetTextureOffset(propertyId, originalOffset);
        }
        capturedOriginal = false;
        originalTexture = null;
    }

    private void SetTexture(Texture texture)
    {
        var material = GetMaterial();
        if (material == null) return;
        if (!material.HasProperty(propertyId)) return;

        material.SetTexture(propertyId, texture);

        bool isLiveVideo = texture != null && texture != PlaceholderTexture;
        if (!isLiveVideo)
        {
            material.SetTextureScale(propertyId, originalScale);
            material.SetTextureOffset(propertyId, originalOffset);
            ApplyShaderKeyword(material, null);
            ClearPicture();
            return;
        }

        BasisVideoOutputMath.GetProjectionUv(ProjectionMode, StereoEye,
            out Vector2 projScale, out Vector2 projOffset, out string keyword);

        int vw = texture.width;
        int vh = texture.height;
        if (ProjectionMode == BasisVideoProjectionMode.SideBySideLR || ProjectionMode == BasisVideoProjectionMode.SideBySideRL) vw /= 2;
        else if (ProjectionMode == BasisVideoProjectionMode.OverUnderTB || ProjectionMode == BasisVideoProjectionMode.OverUnderBT) vh /= 2;

        float displayAspect = DisplayAspectOverride > 0f ? DisplayAspectOverride : EstimateRendererAspect();
        BasisVideoOutputMath.GetAspectUv(AspectMode, vw, vh, displayAspect,
            out Vector2 aspScale, out Vector2 aspOffset);

        BasisVideoOutputMath.Compose(projScale, projOffset, aspScale, aspOffset,
            out Vector2 scale, out Vector2 offset);

        scale.x *= originalScale.x;
        scale.y *= originalScale.y;
        offset.x = originalOffset.x + originalScale.x * offset.x;
        offset.y = originalOffset.y + originalScale.y * offset.y;

        if (FlipVertically && FlipHorizontally) BasisVideoOutputMath.ApplyBothFlip(ref scale, ref offset);
        else if (FlipVertically) BasisVideoOutputMath.ApplyVerticalFlip(ref scale, ref offset);
        else if (FlipHorizontally) BasisVideoOutputMath.ApplyHorizontalFlip(ref scale, ref offset);

        material.SetTextureScale(propertyId, scale);
        material.SetTextureOffset(propertyId, offset);

        ApplyShaderKeyword(material, keyword);
        ApplyPicture();
    }

    private float EstimateRendererAspect()
    {
        if (TargetRenderer == null) return 1f;
        var b = TargetRenderer.localBounds.size;
        if (b.x > 0f && b.y > 0f) return b.x / b.y;
        return 1f;
    }

    private void ApplyShaderKeyword(Material material, string keyword)
    {
        if (material == null) return;
        if (!string.IsNullOrEmpty(activeShaderKeyword) && activeShaderKeyword != keyword)
        {
            material.DisableKeyword(activeShaderKeyword);
        }
        activeShaderKeyword = keyword;
        if (!string.IsNullOrEmpty(keyword)) material.EnableKeyword(keyword);
    }

    private void ApplyPicture()
    {
        if (TargetRenderer == null) return;
        mpb ??= new MaterialPropertyBlock();
        TargetRenderer.GetPropertyBlock(mpb, MaterialIndex);
        mpb.SetFloat(PropBrightness, Picture.Brightness);
        mpb.SetFloat(PropContrast, Picture.Contrast);
        mpb.SetFloat(PropSaturation, Picture.Saturation);
        mpb.SetFloat(PropGamma, Picture.Gamma <= 0f ? 1f : Picture.Gamma);
        TargetRenderer.SetPropertyBlock(mpb, MaterialIndex);
    }

    private void ClearPicture()
    {
        if (TargetRenderer == null || mpb == null) return;
        TargetRenderer.GetPropertyBlock(mpb, MaterialIndex);
        mpb.SetFloat(PropBrightness, 1f);
        mpb.SetFloat(PropContrast, 1f);
        mpb.SetFloat(PropSaturation, 1f);
        mpb.SetFloat(PropGamma, 1f);
        TargetRenderer.SetPropertyBlock(mpb, MaterialIndex);
    }

    // Re-apply when the flip toggle changes in the inspector during play.
    private void OnValidate()
    {
        if (Application.isPlaying && isActiveAndEnabled && Player != null)
            SetTexture(Player.OutputTexture);
    }

    private Material GetMaterial()
    {
        if (TargetRenderer == null) return null;
        if (UseSharedMaterial)
        {
            var shared = TargetRenderer.sharedMaterials;
            if (MaterialIndex < 0 || MaterialIndex >= shared.Length) return null;
            return shared[MaterialIndex];
        }
        // Access .materials once to take ownership of a cloned array, then
        // index into it; accessing .materials repeatedly leaks instances.
        var instances = TargetRenderer.materials;
        if (MaterialIndex < 0 || MaterialIndex >= instances.Length) return null;
        return instances[MaterialIndex];
    }
}
