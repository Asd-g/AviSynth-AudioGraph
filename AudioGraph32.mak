CXX=@cl.exe
CXXFLAGS=/MT /EHa /O2 /Ob2 /D "NDEBUG" /D "WIN32" /D "_WINDOWS"

LINK32=@link.exe
LINK32FLAGS=/dll /machine:x86 /nologo

AudioGraph32.dll: AudioGraph.obj convertaudio.obj
	$(LINK32) $(LINK32FLAGS) AudioGraph.obj convertaudio.obj /out:AudioGraph32.dll

convertaudio.obj: convertaudio.cpp
	$(CXX) $(CXXFLAGS) convertaudio.cpp -c

AudioGraph.obj: AudioGraph.cpp
	$(CXX) $(CXXFLAGS) AudioGraph.cpp -c

