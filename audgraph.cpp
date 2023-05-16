/*
 * AudioGraph filter for AviSynth
 * Richard Ling - r.ling(at)eudoramail.com
 * 
 * This filter displays the audio waveform for a video, superimposed on the
 * video.  It is mainly intended to help during editing rather than for final
 * output.  It can be useful for finding and isolating specific sequences of
 * dialogue or sound, and for checking that overdubbed audio (especially
 * speech) is in sync with video.
 * 
 * The audio is displayed as a green waveform stretching from left to right
 * across the frame.  The filter can graph the audio for the currently visible
 * frame only; or it can include the audio for several successive frames on
 * either side of the current frame.  Graphing several frames makes it easier
 * to find a sound of interest.  It is also really cool to watch the waveform
 * scrolling across the video as the video plays :-)
 * 
 * USAGE:
 * -----
 * AudioGraph(clip, int frames_either_side)
 * 
 * Parameters:
 *   clip                   The source clip.  YUY2, RGB24 or RGB32 video, with
 *                          8-bit or 16-bit mono or stereo audio.
 *   frames_either_side     The number of frames, either side of the current
 *                          frame, which should be graphed.
 * 
 * The effect of the frames_either_side parameter is perhaps better explained
 * by this table:
 * 
 * value  effect
 *   0    only audio for the currently visible frame is graphed.
 *   1    audio for the preceding, current, and following frames are graphed.
 *   2    audio for the preceding 2 frames, current frame, and following 2
 *        frames are graphed.
 * ...and so on.
 * 
 * The current frame's audio is displayed in the centre of the video frame in
 * bright green, while audio for preceding and following frames is displayed
 * in darker green.
 * 
 * EXAMPLE:
 * -------
 * The following .avs file creates a video from a WAV file.  Just replace the
 * WAVSource filename with one existing on your system.  You can also adjust
 * the length passed to BlankClip to match the duration of your WAV file.
 * 
 *   LoadPlugin("audgraph.dll")
 *   audio = WAVSource("sample.wav")
 *   return AudioGraph(AudioDub(BlankClip(1000), audio), 20)
 * 
 * TO DO:
 * -----
 * - Allow the colour of the graph to be passed as a parameter.
 * - Allow separate graphing of left or right channels of stereo audio (using
 *   different colours).
 * - Allow a vertical scale factor to be passed as a parameter, so that quiet
 *   waveforms can be seen in more detail.
 * - Fix the "feature" that the current frame's audio is not always centred
 *   on the display.  It can be offset quite far to the right, depending on
 *   the relationship between video frame width and frames_either_side.
 */

#include "windows.h"
#include "crtdbg.h"
#include "avisynth.h"


/*
 * How this filter works:
 *
 * An "audioframe" is the audio data corresponding to a video frame, converted
 * into an internal form that can be quickly drawn.  A total of (1 + 2 *
 * frames_either_side) audioframes are drawn onto each video frame.  Each
 * audioframe is thus (video frame width) / (1 + 2 * frames_either_side)
 * pixels wide.  An audioframe simply consists of a Y pixel coordinate for
 * each X pixel coordinate, so drawing an audioframe is very fast.
 *
 * When frames_either_side is nonzero, the same audioframe will be drawn
 * several times on several successive video frames.  So it makes sense to
 * cache audioframes.  The filter uses a cache of "audioframe buffers" to
 * store recently used audioframes.  Audioframes are generated from raw audio
 * data on demand, and stored in the cache.  The caching system is such that a
 * specific audioframe is only ever cached in a specific audioframe buffer, so
 * that cache lookup is very fast.  This caching also improves performance
 * when seeking back and forth in a video.
 */
class AudioGraph : public GenericVideoFilter
{
public:
    AudioGraph(PClip _child, int _frames_either_side, IScriptEnvironment* _env);
    virtual ~AudioGraph();
    void FillAudioFrame8(WORD *audioframe_buffer);
    void FillAudioFrame16(WORD *audioframe_buffer);
    WORD *GetAudioFrame(int frame, IScriptEnvironment* env);
    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env);
private:
    BYTE* audio_buffer;
    int *cache_lookup;
    WORD *audioframe_buffers;
    BYTE **sample_ranges;
    int samples_per_frame;
    int num_audioframe_buffers;
    int frames_either_side;
    int pixels_per_audioframe;
    int log_samples_per_pixel;
    int log_mono_samples_per_pixel;
};


/*
 * AudioGraph::AudioGraph
 * 
 * Parameters:
 *   _child                 The PClip which whose audio is to be graphed.
 *   _frames_either_side    The number of frames either side of the current
 *                          frame, whose audio should be graphed.
 */
AudioGraph::AudioGraph(PClip _child, int _frames_either_side, IScriptEnvironment* _env) :
    GenericVideoFilter(_child),
    audio_buffer(NULL),
    audioframe_buffers(NULL),
    cache_lookup(NULL),
    sample_ranges(NULL),
    frames_either_side(_frames_either_side)
{
    int num_visible_audioframes;
    int bytes_per_sample;

    if (! vi.HasAudio())
        _env->ThrowError("AudioGraph: clip has no audio");

    if (_frames_either_side < 0)
        _env->ThrowError("AudioGraph: negative parameter not allowed");
    /*
     * Allocate the buffer for raw audio data.  We only ever read raw audio
     * data for one frame at a time.
     */
    bytes_per_sample = vi.BytesPerAudioSample();
    samples_per_frame = vi.AudioSamplesFromFrames(1);
    audio_buffer = new BYTE[bytes_per_sample * samples_per_frame];
    /*
     * Calculate the number of visible audioframes.  For efficiency reasons,
     * the width of an audioframe is rounded up to an exact number of pixels.
     * This can mean that we actually end up drawing less audioframes than
     * num_visible_audioframes, but it's a reasonable sacrifice for the speed
     * improvement it gives.
     *
     * To prevent weird effects and math errors, it is necessary for each
     * audioframe to be at least 2 pixels wide.  So if frames_either_side is
     * too large, adjust it downwards.
     */
    if (frames_either_side > vi.width / 4)
        frames_either_side = vi.width / 4;
    num_visible_audioframes = frames_either_side * 2 + 1;
    pixels_per_audioframe = (vi.width / num_visible_audioframes) + 1;
    /*
     * Work out how many audioframe buffers we want.  To simplify caching,
     * audioframe n (corresponding to video frame n) is always cached in
     * audioframe buffer (n % num_audioframe_buffers).  By choosing
     * num_audioframe_buffers to be the next biggest power of 2 after
     * num_visible_audioframes, we can replace the modulus with a logical AND,
     * gaining performance.  The audioframe buffers are allocated as one big
     * chunk of WORDs.
     */
    num_audioframe_buffers = 1;
    while (num_audioframe_buffers < num_visible_audioframes)
        num_audioframe_buffers <<= 1;
    audioframe_buffers = new WORD[pixels_per_audioframe * num_audioframe_buffers];
    /*
     * cache_lookup tells us which audioframe is currently stored in each
     * audioframe buffer.  Initialise this with an invalid value meaning
     * "empty", noting that we can get asked for negatively-numbered
     * audioframes.
     */
    cache_lookup = new int[num_audioframe_buffers];
    for (int i = 0; i < num_audioframe_buffers; i++)
        cache_lookup[i] = -num_audioframe_buffers;
    /*
     * We need a way to generate an audioframe from one video frame's worth of
     * raw audio data.  Doing this exactly involves dividing the audio data
     * into (pixels_per_audioframe) parts, averaging together all the samples
     * in each part, and scaling each result according to the height of the
     * video frame to get the Y pixel coordinate.  For example, if there were
     * 20 samples per frame and 3 pixels per audioframe, we should generate
     * pixel 0 from samples 0-6, pixel 1 from samples 7-12, and pixel 2 from
     * samples 13-19.
     *
     * The first problem is that calculating an average involves division.  So
     * we actually don't average together 6 or 7 samples as stated above, but
     * round up to the nearest power of 2 (8 samples).  This increases speed,
     * but costs some accuracy, since the sample ranges that contribute to
     * neighbouring pixels can overlap - in the above case, pixel 0 would be
     * generated from samples 0-7, pixel 1 from samples 6-13, pixel 2 from
     * samples 12-19.  log_samples_per_pixel is the log (base 2) of the number
     * of audio samples that contribute to each pixel.
     */
    log_samples_per_pixel = 0;
    while (1 << log_samples_per_pixel < (samples_per_frame / pixels_per_audioframe))
        log_samples_per_pixel++;
    log_mono_samples_per_pixel = log_samples_per_pixel;
    if (vi.stereo)
        log_mono_samples_per_pixel++;
    /*
     * The second problem is that calculating which pixel a given sample
     * contributes to also involves division.  This is easily solved by
     * calculating the first sample in each range once at startup, and storing
     * a pointer to it in the sample_ranges array which can then be indexed by
     * the X pixel coordinate.
     */
    sample_ranges = new BYTE*[pixels_per_audioframe];
    int start_of_last_sample_range = (samples_per_frame - (1 << log_samples_per_pixel));
    sample_ranges[0] = audio_buffer;
    // Note, pixels_per_audioframe must be at least 2 - this was checked above
    for (int x_pixel = 0; x_pixel < pixels_per_audioframe; x_pixel++)
        sample_ranges[x_pixel] =
            audio_buffer
            + (x_pixel * start_of_last_sample_range / (pixels_per_audioframe - 1))
            * bytes_per_sample;
}


/*
 * AudioGraph::~AudioGraph
 * 
 * Free allocated memory.
 */
AudioGraph::~AudioGraph()
{
    delete[] audio_buffer;
    delete[] audioframe_buffers;
    delete[] cache_lookup;
    delete[] sample_ranges;
}


/*
 * AudioGraph::FillAudioFrame8
 * 
 * Fill an audioframe buffer from the 8-bit audio data in audio_buffer.  If
 * the source is stereo, the channels are averaged.  The resulting Y pixel
 * coordinates are stored into the given audioframe buffer.
 * 
 * Parameters:
 *   audioframe_buffer     A pointer to the audioframe buffer to fill.
 */
void AudioGraph::FillAudioFrame8(WORD *audioframe_buffer)
{
    for (int x_pixel = 0; x_pixel < pixels_per_audioframe; x_pixel++)
    {
        BYTE *src = sample_ranges[x_pixel];
        int y_pixel = 0;
        int num_samples = 1 << log_mono_samples_per_pixel;
        while (num_samples--)
        {
            y_pixel += *(BYTE*)src - 128;
            src++;
        }
        y_pixel >>= log_mono_samples_per_pixel;
        y_pixel = (128 + y_pixel) * vi.height / 256;
        audioframe_buffer[x_pixel] = (WORD)y_pixel;
    }
}


/*
 * AudioGraph::FillAudioFrame16
 * 
 * Fill an audioframe buffer from the 16-bit audio data in audio_buffer.  If
 * the source is stereo, the channels are averaged.  The resulting Y pixel
 * coordinates are stored into the given audioframe buffer.
 * 
 * Parameters:
 *   audioframe_buffer     A pointer to the audioframe buffer to fill.
 */
void AudioGraph::FillAudioFrame16(WORD *audioframe_buffer)
{
    for (int x_pixel = 0; x_pixel < pixels_per_audioframe; x_pixel++)
    {
        BYTE *src = sample_ranges[x_pixel];
        int y_pixel = 0;
        int num_samples = 1 << log_mono_samples_per_pixel;
        while (num_samples--)
        {
            y_pixel += *(short*)src;
            src += 2;
        }
        y_pixel >>= log_mono_samples_per_pixel;
        y_pixel = (32768 + y_pixel) * vi.height / 65536;
        audioframe_buffer[x_pixel] = (WORD)y_pixel;
    }
}


/*
 * AudioGraph::GetAudioFrame
 * 
 * Get a pointer to the audioframe corresponding to the given video frame.  If
 * the audioframe already exists in the cache, it is returned immediately.
 * Otherwise it is generated from the raw audio data, stored in the cache, and
 * returned.
 *
 * Parameters:
 *   frame      The frame whose audioframe is needed.
 *   env        A pointer to the IScriptEnvironment.
 * 
 * Returns:
 *   A pointer to the audioframe buffer holding the requested audioframe.
 */
WORD *AudioGraph::GetAudioFrame(int frame, IScriptEnvironment* env)
{
    int audioframe_index;
    WORD *audioframe_buffer;

    audioframe_index = frame & (num_audioframe_buffers - 1);
    audioframe_buffer = &audioframe_buffers[audioframe_index * pixels_per_audioframe];
    if (cache_lookup[audioframe_index] != frame)
    {
        child->GetAudio(audio_buffer, vi.AudioSamplesFromFrames(frame), samples_per_frame, env);
        if (vi.sixteen_bit)
            FillAudioFrame16(audioframe_buffer);
        else
            FillAudioFrame8(audioframe_buffer);
        cache_lookup[audioframe_index] = frame;
    }
    return audioframe_buffer;
}


/*
 * AudioGraph::GetFrame
 * 
 * Standard GetFrame method - gets video frame n as processed by this filter.
 * 
 * Parameters:
 *   n          The frame to get.
 *   env        A pointer to the IScriptEnvironment.
 * 
 * Returns:
 *   The frame, with audio graph overlaid.
 */
PVideoFrame __stdcall AudioGraph::GetFrame(int n, IScriptEnvironment* env)
{
    /*
     * First create a copy of the child frame.
     */
    PVideoFrame src = child->GetFrame(n, env);
    PVideoFrame dst = env->NewVideoFrame(vi);
    const unsigned char* srcp = src->GetReadPtr();
    unsigned char* dstp = dst->GetWritePtr();
    int src_pitch = src->GetPitch();
    int dst_pitch = dst->GetPitch();
    int row_size = dst->GetRowSize();
    int height = dst->GetHeight();
    int bytes_per_pixel = vi.BytesFromPixels(1);
    int pixels_per_row = row_size / bytes_per_pixel;
    
    env->BitBlt(dstp, dst_pitch, srcp, src_pitch, row_size, height);

    int prev_y_pixel = height >> 1;
    dstp += prev_y_pixel * dst_pitch;
    int frame = n - frames_either_side;
    WORD *audioframe_buffer;
    int x_pixel = pixels_per_audioframe;
    int colour;

    /*
     * The entire drawing loop has been duplicated for each possible number of
     * bytes per pixel, in order to increase efficiency.  This is important as
     * the inner part of this loop is one of the most executed parts of the
     * filter.  Better to test the number of bytes per pixel once, rather than
     * each time we draw a pixel.
     *
     * If speed was really critical this could be rewritten in assembler.
     */
    if (vi.IsYUV())
    {
        while (pixels_per_row--)
        {
            if (x_pixel == pixels_per_audioframe)
            {
                audioframe_buffer = GetAudioFrame(frame, env);
                colour = (frame == n) ? 0x007F : 0x0000;
                frame++;
                x_pixel = 0;
            }
            int y_pixel = audioframe_buffer[x_pixel];
            while (prev_y_pixel < y_pixel)
            {
                *(WORD*)dstp = colour;
                dstp += dst_pitch;
                prev_y_pixel++;
            }
            while (prev_y_pixel > y_pixel)
            {
                *(WORD*)dstp = colour;
                dstp -= dst_pitch;
                prev_y_pixel--;
            }
            *(WORD*)dstp = colour;
            dstp += 2;
            x_pixel++;
        }
    }
    else if (vi.IsRGB24())
    {
        while (pixels_per_row--)
        {
            if (x_pixel == pixels_per_audioframe)
            {
                audioframe_buffer = GetAudioFrame(frame, env);
                colour = (frame == n) ? 0xFF : 0x88;
                frame++;
                x_pixel = 0;
            }
            int y_pixel = audioframe_buffer[x_pixel];
            while (prev_y_pixel < y_pixel)
            {
                *(BYTE*)(dstp+0) = 0;
                *(BYTE*)(dstp+1) = colour;
                *(BYTE*)(dstp+2) = 0;
                dstp += dst_pitch;
                prev_y_pixel++;
            }
            while (prev_y_pixel > y_pixel)
            {
                *(BYTE*)(dstp+0) = 0;
                *(BYTE*)(dstp+1) = colour;
                *(BYTE*)(dstp+2) = 0;
                dstp -= dst_pitch;
                prev_y_pixel--;
            }
            *(BYTE*)(dstp+0) = 0;
            *(BYTE*)(dstp+1) = colour;
            *(BYTE*)(dstp+2) = 0;
            dstp += 3;
            x_pixel++;
        }
    }
    else if (vi.IsRGB32())
    {
        while (pixels_per_row--)
        {
            if (x_pixel == pixels_per_audioframe)
            {
                audioframe_buffer = GetAudioFrame(frame, env);
                colour = (frame == n) ? 0x00FF00 : 0x008800;
                frame++;
                x_pixel = 0;
            }
            int y_pixel = audioframe_buffer[x_pixel];
            while (prev_y_pixel < y_pixel)
            {
                *(DWORD*)dstp = colour;
                dstp += dst_pitch;
                prev_y_pixel++;
            }
            while (prev_y_pixel > y_pixel)
            {
                *(DWORD*)dstp = colour;
                dstp -= dst_pitch;
                prev_y_pixel--;
            }
            *(DWORD*)dstp = colour;
            dstp += 4;
            x_pixel++;
        }
    }
    return dst;
}


/*
 * Create_AudioGraph
 * 
 * Called by AviSynth to create a new AudioGraph filter.
 * 
 * Parameters:
 *   args           The arguments to the filter, specified in the script.
 *   user_data      Our user data parameter; not used.
 *   env            A pointer to the IScriptEnvironment.
 * 
 * Returns:
 *   An AVSValue__cdecl which does something.
 */
AVSValue __cdecl Create_AudioGraph(AVSValue args, void* user_data, IScriptEnvironment* env)
{
    return new AudioGraph(args[0].AsClip(), args[1].AsInt(), env);
}


/*
 * AvisynthPluginInit
 * 
 * Called by AviSynth when the plugin is loaded.  It registers a new function
 * called "AudioGraph", which scripts can then call to invoke this filter.
 *
 * Parameters:
 *   env    A pointer to the IScriptEnvironment.
 * 
 * Returns:
 *   A short description of the plugin.
 */
extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit(IScriptEnvironment* env)
{
    env->AddFunction("AudioGraph", "ci", Create_AudioGraph, NULL);
    return "`AudioGraph' sample plugin";
}

