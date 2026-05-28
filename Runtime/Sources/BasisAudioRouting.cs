public enum BasisAudioRouting
{
    // Audio frames are decoded by the player pipeline and pushed into a
    // BasisMediaPlayerAudio component on the same GameObject, which feeds a
    // streaming AudioClip on a Unity AudioSource. Spatialization, mixer routing,
    // and per-instance volume all flow through Unity's audio graph.
    //
    // Only routing currently supported. The enum exists as a stable property
    // type so additional routings (e.g. direct OS-audio output from a native
    // backend) can be added without touching call sites.
    UnityAudioSource = 0,
}
