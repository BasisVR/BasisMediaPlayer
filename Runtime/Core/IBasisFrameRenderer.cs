using System;
using UnityEngine;

// Uploads a decoded frame to a Unity texture exposed as OutputTexture.
// Renderers own the texture they produce and are responsible for rebuilding it
// when frame dimensions or pixel format change. Consumers bind OutputTexture
// to materials, UI elements, etc. by subscribing to OnOutputTextureChanged.
//
// Split out from the player so YUV/RGBA paths can be swapped without touching
// the scheduling logic.
public interface IBasisFrameRenderer : IDisposable
{
    bool SupportsFormat(BasisVideoFrameFormat format);

    // The current output texture. Null until the first frame has been
    // presented or until SupportsFormat would have rejected every prior frame.
    Texture OutputTexture { get; }

    // Fired whenever OutputTexture is rebuilt (initial creation, dimension
    // change, format change). The new texture is passed in the argument; it
    // may be null if the renderer transitioned to a not-yet-ready state.
    event Action<Texture> OnOutputTextureChanged;

    // Uploads the frame's pixel data into OutputTexture. Returns true on success.
    // Renderers must not bind OutputTexture to materials themselves; that's a
    // consumer responsibility (BasisVideoMaterialOutput / BasisVideoDisplay).
    bool Present(BasisVideoFrame frame);
}
