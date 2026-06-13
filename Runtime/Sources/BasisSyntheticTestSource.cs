using System;
using System.Threading;

// Generates a moving gradient on a background thread. Lets the full
// source -> queue -> clock -> renderer pipeline be exercised without any network
// or codec dependency.
//
// Note: a fresh buffer is allocated per frame so the consumer can safely hold the
// reference. This is intentionally simple; production sources should pool buffers.
public sealed class BasisSyntheticTestSource : IBasisFrameSource
{
    public enum Pattern
    {
        Gradient = 0,
        Checkerboard = 1,
        SolidColor = 2,
        Noise = 3,
    }

    public int Width { get; }
    public int Height { get; }
    public int FramesPerSecond { get; }
    public Pattern PatternMode { get; set; } = Pattern.Gradient;
    public int CheckerboardSquarePixels { get; set; } = 32;
    public byte SolidR { get; set; } = 64;
    public byte SolidG { get; set; } = 128;
    public byte SolidB { get; set; } = 192;
    public byte SolidA { get; set; } = 255;
    public int ColorCycleSpeed { get; set; } = 1;
    public int EmitEndOfStreamAfterFrames { get; set; } = 0; // 0 = never
    public int NoiseSeed { get; set; } = 0;

    private Thread thread;
    private volatile bool running;
    private long framesEmitted;

    public long FramesEmitted => System.Threading.Interlocked.Read(ref framesEmitted);

    public event Action<BasisVideoFrame> OnVideoFrame;
    public event Action<Exception> OnError;
    public event Action OnEndOfStream;
    public event Action OnReady;
    public event Action<int, int> OnVideoSizeChanged;

    public bool IsRunning => running;

    public BasisSyntheticTestSource(int width = 640, int height = 360, int framesPerSecond = 30)
    {
        if (width <= 0 || height <= 0) throw new ArgumentOutOfRangeException(nameof(width));
        if (framesPerSecond <= 0) throw new ArgumentOutOfRangeException(nameof(framesPerSecond));
        Width = width;
        Height = height;
        FramesPerSecond = framesPerSecond;
    }

    public void Start()
    {
        if (running) return;
        running = true;
        thread = new Thread(Run) { IsBackground = true, Name = "BasisSyntheticTestSource" };
        thread.Start();
    }

    public void Stop()
    {
        running = false;
        thread?.Join(500);
        thread = null;
    }

    public void Dispose() => Stop();

    private void Run()
    {
        try
        {
            long frameDurationUs = 1_000_000L / FramesPerSecond;
            long pts = 0;
            int frameIndex = 0;
            var rng = new Random(NoiseSeed);

            OnVideoSizeChanged?.Invoke(Width, Height);
            OnReady?.Invoke();

            var sw = System.Diagnostics.Stopwatch.StartNew();
            long ticksPerSecond = System.Diagnostics.Stopwatch.Frequency;
            long frameDurationTicks = ticksPerSecond / FramesPerSecond;
            long nextFrameTicks = sw.ElapsedTicks + frameDurationTicks;

            while (running)
            {
                byte[] buffer = new byte[Width * Height * 4];
                FillPattern(buffer, frameIndex, rng);

                OnVideoFrame?.Invoke(new BasisVideoFrame
                {
                    PresentationTimeUs = pts,
                    Width = Width,
                    Height = Height,
                    Format = BasisVideoFrameFormat.Rgba32,
                    Data = buffer,
                    DataLength = buffer.Length,
                });

                pts += frameDurationUs;
                frameIndex++;
                System.Threading.Interlocked.Increment(ref framesEmitted);

                if (EmitEndOfStreamAfterFrames > 0 && frameIndex >= EmitEndOfStreamAfterFrames)
                {
                    OnEndOfStream?.Invoke();
                    running = false;
                    break;
                }

                long waitTicks = nextFrameTicks - sw.ElapsedTicks;
                if (waitTicks > 0)
                {
                    long waitMs = waitTicks * 1000L / ticksPerSecond;
                    if (waitMs > 2) Thread.Sleep((int)(waitMs - 1));
                    while (running && sw.ElapsedTicks < nextFrameTicks) Thread.SpinWait(50);
                }
                nextFrameTicks += frameDurationTicks;
            }
        }
        catch (Exception ex)
        {
            OnError?.Invoke(ex);
        }
    }

    private void FillPattern(byte[] buffer, int frameIndex, Random rng)
    {
        switch (PatternMode)
        {
            case Pattern.Gradient: FillGradient(buffer, Width, Height, frameIndex * ColorCycleSpeed); break;
            case Pattern.Checkerboard: FillCheckerboard(buffer, Width, Height, frameIndex * ColorCycleSpeed, CheckerboardSquarePixels); break;
            case Pattern.SolidColor: FillSolid(buffer, SolidR, SolidG, SolidB, SolidA); break;
            case Pattern.Noise: FillNoise(buffer, rng); break;
        }
    }

    private static void FillGradient(byte[] buffer, int w, int h, int phaseSource)
    {
        int phase = phaseSource & 0xFF;
        int i = 0;
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                buffer[i + 0] = (byte)((x + phase) & 0xFF);
                buffer[i + 1] = (byte)((y + phase) & 0xFF);
                buffer[i + 2] = (byte)((x + y + phase) & 0xFF);
                buffer[i + 3] = 0xFF;
                i += 4;
            }
        }
    }

    private static void FillCheckerboard(byte[] buffer, int w, int h, int phaseSource, int squarePixels)
    {
        if (squarePixels < 1) squarePixels = 1;
        int phase = phaseSource & 0xFF;
        int i = 0;
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                bool dark = (((x / squarePixels) + (y / squarePixels)) & 1) == 0;
                byte v = (byte)((dark ? 32 : 224) ^ phase);
                buffer[i + 0] = v;
                buffer[i + 1] = v;
                buffer[i + 2] = v;
                buffer[i + 3] = 0xFF;
                i += 4;
            }
        }
    }

    private static void FillSolid(byte[] buffer, byte r, byte g, byte b, byte a)
    {
        for (int i = 0; i < buffer.Length; i += 4)
        {
            buffer[i + 0] = r;
            buffer[i + 1] = g;
            buffer[i + 2] = b;
            buffer[i + 3] = a;
        }
    }

    private static void FillNoise(byte[] buffer, Random rng)
    {
        rng.NextBytes(buffer);
        for (int i = 3; i < buffer.Length; i += 4) buffer[i] = 0xFF;
    }
}
