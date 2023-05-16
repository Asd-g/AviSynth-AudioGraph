audio = WavSource("test.wav")
return AudioGraph(AudioDub(BlankClip(10000), audio), 20, 0, $8a9dff, $fcb5db)
