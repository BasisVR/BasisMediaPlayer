using UnityEngine;

// Declares which decoded channel(s) one AudioSource plays inside a
// BasisMediaPlayerMultiChannelAudio output set. Sits on the same GameObject as
// the AudioSource. Mono selections take a single channel; Stereo plays a
// stereo downmix of the whole stream as a 2-channel clip.
//
// Channel numbers follow the decoded WAVE order:
//   1 Front Left  2 Front Right  3 Front Centre  4 LFE  5 Back Left  6 Back Right
[RequireComponent(typeof(AudioSource))]
public sealed class BasisMediaAudioChannel : MonoBehaviour
{
    public enum Selection
    {
        [InspectorName("Mono 1 (Front Left)")] Mono1 = 0,
        [InspectorName("Mono 2 (Front Right)")] Mono2 = 1,
        [InspectorName("Mono 3 (Front Centre)")] Mono3 = 2,
        [InspectorName("Mono 4 (LFE)")] Mono4 = 3,
        [InspectorName("Mono 5 (Back Left)")] Mono5 = 4,
        [InspectorName("Mono 6 (Back Right)")] Mono6 = 5,
        [InspectorName("Stereo (downmix)")] Stereo = 100,
    }

    [Tooltip("Which decoded channel(s) this AudioSource plays. Mono picks one channel; Stereo plays a stereo downmix of the whole stream.")]
    public Selection Channel = Selection.Mono1;

    public bool IsStereo => Channel == Selection.Stereo;

    // Decoded-stream channel index a mono selection draws from (0-based).
    public int PrimaryChannel => Channel == Selection.Stereo ? 0 : (int)Channel;
}
