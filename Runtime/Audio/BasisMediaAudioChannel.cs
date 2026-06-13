using UnityEngine;

// Declares which decoded channel(s) one AudioSource plays inside a
// BasisMediaPlayerAudio output set. Sits on the same GameObject as the
// AudioSource. Channel selections take a single decoded channel; Stereo plays a
// stereo downmix of the whole stream as a 2-channel clip.
//
// What each channel carries depends on the stream's layout — e.g. a 5.1 stream
// decodes in WAVE order (1 Front Left, 2 Front Right, 3 Front Centre, 4 LFE,
// 5 Back Left, 6 Back Right), while a custom mix can use them as arbitrary
// content lanes.
[RequireComponent(typeof(AudioSource))]
public sealed class BasisMediaAudioChannel : MonoBehaviour
{
    public enum Selection
    {
        [InspectorName("Channel 1")] Channel1 = 0,
        [InspectorName("Channel 2")] Channel2 = 1,
        [InspectorName("Channel 3")] Channel3 = 2,
        [InspectorName("Channel 4")] Channel4 = 3,
        [InspectorName("Channel 5")] Channel5 = 4,
        [InspectorName("Channel 6")] Channel6 = 5,
        [InspectorName("Channel 7")] Channel7 = 6,
        [InspectorName("Channel 8")] Channel8 = 7,
        // Values 0-63 are direct decoded channel indices; 100+ are reserved for
        // mixed/virtual output modes so future channels don't collide.
        [InspectorName("Stereo (downmix)")] Stereo = 100,
    }

    [Tooltip("Which decoded channel(s) this AudioSource plays. Channel N picks one decoded channel; Stereo plays a stereo downmix of the whole stream.")]
    public Selection Channel = Selection.Channel1;

    public bool IsStereo => Channel == Selection.Stereo;

    // Decoded-stream channel index a mono selection draws from (0-based).
    public int PrimaryChannel => Channel == Selection.Stereo ? 0 : (int)Channel;
}
