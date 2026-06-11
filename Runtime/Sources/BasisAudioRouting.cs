public enum BasisAudioRouting
{
    // Audio frames are decoded by the player pipeline and pushed into a
    // BasisMediaPlayerAudio component on the same GameObject, which feeds a
    // streaming AudioClip on a Unity AudioSource. Spatialization, mixer routing,
    // and per-instance volume all flow through Unity's audio graph.
    //
    UnityAudioSource = 0,

    // Decoded audio is de-interleaved into one mono Unity AudioSource per
    // channel via a BasisMediaPlayerMultiChannelAudio component, letting a
    // surround stream (e.g. 5.1) be positioned channel-by-channel in the world.
    // Each channel spatializes independently through Unity's audio graph.
    UnityMultiChannelSources = 1,
}
