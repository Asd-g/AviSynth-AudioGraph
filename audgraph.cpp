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
 *	 _graph_scale			The vertical scale factor. Set to 0 to enable auto-scale
 *	 _middle_colour			The graph colour for the current frame
 *	 _side_colour			The graph colour for the frames on either side of the current
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
 *   return AudioGraph(AudioDub(BlankClip(1000), audio), 20, 0, $8a9dff, $fcb5db)
 * 
 * TO DO:
 * -----
 * - Allow separate graphing of left or right channels of stereo audio (using
 *   different colours).
 * - Fix the "feature" that the current frame's audio is not always centred
 *   on the display.  It can be offset quite far to the right, depending on
 *   the relationship between video frame width and frames_either_side.
 */

#include "windows.h"
#include "crtdbg.h"
#include "avisynth.h"
//#include "audio.h"

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
	AudioGraph(PClip _child, int _frames_either_side, int _graph_scale, int _middle_colour, int _side_colour, IScriptEnvironment* _env);
	virtual ~AudioGraph();
	int GetGraphAutoScale(IScriptEnvironment* _env);
	void FillAudioFrame8(WORD *audioframe_buffer);
	void FillAudioFrame16(WORD *audioframe_buffer);
	WORD *GetAudioFrame(int frame, IScriptEnvironment* env);
	PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env);
private:
	IScriptEnvironment* m_env;
	size_t  m_audio_buffer_size;
	BYTE*   m_audio_buffer;
	size_t  m_cache_lookup_size;
	int*    m_cache_lookup;
	size_t  m_audioframe_buffers_size;
	WORD*   m_audioframe_buffers;
	size_t  m_sample_ranges_size;
	BYTE**  m_sample_ranges;
	int samples_per_frame;
	int num_audioframe_buffers;
	int frames_either_side;
	int pixels_per_audioframe;
	int log_samples_per_pixel;
	int log_mono_samples_per_pixel;
	int middle_colour, side_colour;
	int graph_scale;
};


/*
 * AudioGraph::AudioGraph
 * 
 * Parameters:
 *   _child                 The PClip which whose audio is to be graphed.
 *   _frames_either_side    The number of frames either side of the current
 *							frame, whose audio should be graphed.
 *	 _graph_scale			The vertical scale factor
 *	 _middle_colour			The graph colour for the current frame
 *	 _side_colour			The graph colour for the frames on either side of the current
 */
AudioGraph::AudioGraph(PClip _child, int _frames_either_side, int _graph_scale, int _middle_colour, int _side_colour, IScriptEnvironment* _env) :
	GenericVideoFilter(ConvertAudio::Create(_child,SAMPLE_INT16|SAMPLE_INT8,SAMPLE_INT16)),
	m_env(_env),
	m_audio_buffer_size(0),
	m_audio_buffer(NULL),
	m_cache_lookup_size(0),
	m_cache_lookup(NULL),
	m_audioframe_buffers_size(0),
	m_audioframe_buffers(NULL),
	m_sample_ranges_size(0),
	m_sample_ranges(NULL),
	frames_either_side(_frames_either_side),
	middle_colour(_middle_colour),
	side_colour(_side_colour)
{
	int num_visible_audioframes;
	int bytes_per_sample;

	if (vi.IsYUY2()) 
	  child = _env->Invoke("Greyscale", child).AsClip();

	if (vi.IsYV12()) 
	  _env->ThrowError("AudioGraph:  YV12 mode not supported.");

	if (! vi.HasAudio())
		_env->ThrowError("AudioGraph: clip has no audio");

	if (_frames_either_side < 0)
		_env->ThrowError("AudioGraph: negative parameter not allowed");
	/*
	 * Allocate the buffer for raw audio data.  We only ever read raw audio
	 * data for one frame at a time.
	 */
	bytes_per_sample = vi.BytesPerAudioSample();
	int audio_channels_count = vi.AudioChannels();
	samples_per_frame = (int)vi.AudioSamplesFromFrames(1);
	m_audio_buffer_size = bytes_per_sample * samples_per_frame * audio_channels_count;
	m_audio_buffer = new BYTE[m_audio_buffer_size];
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
	m_audioframe_buffers_size = pixels_per_audioframe * num_audioframe_buffers;
	m_audioframe_buffers = new WORD[m_audioframe_buffers_size];
	/*
	 * cache_lookup tells us which audioframe is currently stored in each
	 * audioframe buffer.  Initialise this with an invalid value meaning
	 * "empty", noting that we can get asked for negatively-numbered
	 * audioframes.
	 */
	m_cache_lookup_size = num_audioframe_buffers;
	m_cache_lookup = new int[m_cache_lookup_size];
	for (int i = 0; i < num_audioframe_buffers; i++)
		m_cache_lookup[i] = -num_audioframe_buffers;
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
//    if (vi.AudioChannels)
		log_mono_samples_per_pixel+=vi.AudioChannels()-1;
	/*
	 * The second problem is that calculating which pixel a given sample
	 * contributes to also involves division.  This is easily solved by
	 * calculating the first sample in each range once at startup, and storing
	 * a pointer to it in the sample_ranges array which can then be indexed by
	 * the X pixel coordinate.
	 */
	int start_of_last_sample_range = (samples_per_frame - (1 << log_samples_per_pixel));
	if ((int)m_audio_buffer_size <= (pixels_per_audioframe * start_of_last_sample_range / (pixels_per_audioframe - 1)) * bytes_per_sample)
		_env->ThrowError("AudioGraph: invalid audio buffer size");

	m_sample_ranges_size = pixels_per_audioframe;
	m_sample_ranges = new BYTE*[m_sample_ranges_size];
	m_sample_ranges[0] = m_audio_buffer;
	// Note, pixels_per_audioframe must be at least 2 - this was checked above
	for (int x_pixel = 1; x_pixel < pixels_per_audioframe; x_pixel++)
		m_sample_ranges[x_pixel] = m_audio_buffer	+ (x_pixel * start_of_last_sample_range / (pixels_per_audioframe - 1)) * bytes_per_sample;

	/*
	*	Set the vertical scale factor.
	*/
	if (_graph_scale == 0)
	{
		graph_scale = 1;
		graph_scale = GetGraphAutoScale(_env);
	}
	else
		graph_scale = _graph_scale;
}


/*
 * AudioGraph::~AudioGraph
 * 
 * Free allocated memory.
 */
AudioGraph::~AudioGraph()
{
	delete[] m_audio_buffer;
	delete[] m_audioframe_buffers;
	delete[] m_cache_lookup;
	delete[] m_sample_ranges;
}


int AudioGraph::GetGraphAutoScale(IScriptEnvironment* _env)
{
	int max_graph_y_pixel = 0, y_pixel;
	int height2 = vi.height>>1;
	WORD* audioframe_buffer;

	for (int fi = 0; fi < vi.num_frames; ++fi)
	{
		audioframe_buffer = GetAudioFrame(fi, _env);
		for (int x_pixel = 0; x_pixel < pixels_per_audioframe; ++x_pixel)
		{
			y_pixel = abs(int(audioframe_buffer[x_pixel])-height2);
			if (max_graph_y_pixel < y_pixel)
				max_graph_y_pixel = y_pixel;
		}
	}
	int result = height2/max_graph_y_pixel;
	return !result ? 1 : result;
}


template<class T>
inline T Clamp(const T& _x, const T& _a, const T& _b)
{
	if (_x < _a)
		return _a;
	else if (_x > _b)
		return _b;
	return _x;
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
	int height2 = vi.height>>1;
	for (int x_pixel = 0; x_pixel < pixels_per_audioframe; x_pixel++)
	{
		if (x_pixel >= (int)m_sample_ranges_size)
			m_env->ThrowError("AudioGraph: x pixel 8");
		BYTE *src = m_sample_ranges[x_pixel];
		int y_pixel = 0;
		int num_samples = 1 << log_mono_samples_per_pixel;
		while (num_samples--)
		{
			y_pixel += *(BYTE*)src - 128;
			src++;
		}
		y_pixel >>= log_mono_samples_per_pixel;
		y_pixel = Clamp((y_pixel * vi.height / 256) * graph_scale, -height2, height2);
		audioframe_buffer[x_pixel] = (WORD)(height2 + y_pixel);
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
	int height2 = vi.height>>1;
	for (int x_pixel = 0; x_pixel < pixels_per_audioframe; x_pixel++)
	{
		if (x_pixel >= (int)m_sample_ranges_size)
			m_env->ThrowError("AudioGraph: x pixel 16");
		BYTE *src = m_sample_ranges[x_pixel];
		int y_pixel = 0;
		int num_samples = 1 << log_mono_samples_per_pixel;
		while (num_samples--)
		{
			y_pixel += *(short*)src;
			src += 2;
		}
		y_pixel >>= log_mono_samples_per_pixel;
		y_pixel = Clamp((y_pixel * vi.height / 65536) * graph_scale, -height2, height2);
		audioframe_buffer[x_pixel] = (WORD)(height2 + y_pixel);
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
	int audioframe_buffer_index, audioframe_index;
	WORD *audioframe_buffer;

	audioframe_index = frame & (num_audioframe_buffers - 1);
	audioframe_buffer_index = audioframe_index * pixels_per_audioframe;
	if (audioframe_buffer_index >= (int)m_audioframe_buffers_size)
		m_env->ThrowError("AudioGraph: audioframe buffer index");
	audioframe_buffer = &m_audioframe_buffers[audioframe_buffer_index];
	if (audioframe_index >= (int)m_cache_lookup_size)
		m_env->ThrowError("AudioGraph: audioframe index");
	if (m_cache_lookup[audioframe_index] != frame)
	{
		if (vi.SampleType() == SAMPLE_INT16) {
			if (samples_per_frame * 2 > (int) m_audio_buffer_size)
				m_env->ThrowError("AudGraph: invalid buf size 16");
		} else if (vi.SampleType() == SAMPLE_INT8) {
			if (samples_per_frame > (int) m_audio_buffer_size)
				m_env->ThrowError("AudGraph: invalid buf size 8");
		} else {
			m_env->ThrowError("AudGraph: invalid sample type");
		}
		__int64 start = vi.AudioSamplesFromFrames(frame);
		try {
			child->GetAudio(m_audio_buffer, start, samples_per_frame, env);
		} catch (AvisynthError err) {
			memset(m_audio_buffer, 0, m_audio_buffer_size);
		}
		if (vi.sample_type == SAMPLE_INT16)
			FillAudioFrame16(audioframe_buffer);
		else if (vi.sample_type == SAMPLE_INT8)
			FillAudioFrame8(audioframe_buffer);
		else
			m_env->ThrowError("AudioGraph: invalid sample type");
		m_cache_lookup[audioframe_index] = frame;
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
	if (vi.IsYUY2())
	{
		bool col = false;
		while (pixels_per_row--)
		{   
			if (x_pixel == pixels_per_audioframe)
			{
				audioframe_buffer = GetAudioFrame(frame, env);
				col = (frame==n);
				frame++;
				x_pixel = 0;
			}

			int y_pixel = audioframe_buffer[x_pixel];

			while (prev_y_pixel < y_pixel)
			{

			  dstp[0] = dstp[2] = 235;
			  dstp[1] = dstp[3] = (col) ? 15 : 225;

			  dstp += dst_pitch;
			  prev_y_pixel++;
			}
			while (prev_y_pixel > y_pixel)
			{

			  dstp[0] = dstp[2] = 235;
			  dstp[1] = dstp[3] = (col) ? 15 : 225;

			  dstp -= dst_pitch;
			  prev_y_pixel--;
			}

			dstp[0] = dstp[2] = 235;
			dstp[1] = dstp[3] = (col) ? 15 : 225;

			if (x_pixel&1)
			  dstp += 4;

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
				colour = (frame == n || frame == n+1) ? middle_colour : side_colour;
				x_pixel = 0;

				unsigned char* vlinep = dst->GetWritePtr()+(frame-n+frames_either_side)*pixels_per_audioframe*bytes_per_pixel;
				for (int y = 0; y < height; ++y)
				{
					*(BYTE*)(vlinep+0) = *((BYTE*)&colour+0);
					*(BYTE*)(vlinep+1) = *((BYTE*)&colour+1);
					*(BYTE*)(vlinep+2) = *((BYTE*)&colour+2);
					vlinep += row_size;
				}

				colour = (frame == n) ? middle_colour : side_colour;
				frame++;
			}
			int y_pixel = audioframe_buffer[x_pixel];
			while (prev_y_pixel < y_pixel)
			{
				*(BYTE*)(dstp+0) = *((BYTE*)&colour+0);
				*(BYTE*)(dstp+1) = *((BYTE*)&colour+1);
				*(BYTE*)(dstp+2) = *((BYTE*)&colour+2);
				dstp += dst_pitch;
				prev_y_pixel++;
			}
			while (prev_y_pixel > y_pixel)
			{
				*(BYTE*)(dstp+0) = *((BYTE*)&colour+0);
				*(BYTE*)(dstp+1) = *((BYTE*)&colour+1);
				*(BYTE*)(dstp+2) = *((BYTE*)&colour+2);
				dstp -= dst_pitch;
				prev_y_pixel--;
			}
			*(BYTE*)(dstp+0) = *((BYTE*)&colour+0);
			*(BYTE*)(dstp+1) = *((BYTE*)&colour+1);
			*(BYTE*)(dstp+2) = *((BYTE*)&colour+2);
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
				colour = (frame == n || frame == n+1) ? middle_colour : side_colour;
				x_pixel = 0;

				unsigned char* vlinep = dst->GetWritePtr()+(frame-n+frames_either_side)*pixels_per_audioframe*bytes_per_pixel;
				for (int y = 0; y < height; ++y)
				{
					*(DWORD*)vlinep = colour;
					vlinep += row_size;
				}

				colour = (frame == n) ? middle_colour : side_colour;
				frame++;
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
	else if (vi.IsYV12()) {

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
	return new AudioGraph(args[0].AsClip(), args[1].AsInt(), args[2].AsInt(), args[3].AsInt(), args[4].AsInt(), env);
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
extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit2(IScriptEnvironment* env)
{
	env->AddFunction("AudioGraph", "ciiii", Create_AudioGraph, NULL);
	return "'AudioGraph' sample plugin";
}

