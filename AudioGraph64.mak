CXX=@cl.exe
CXXFLAGS=/MT /EHa /O2 /Ob2 /D "NDEBUG" /D "WIN32" /D "_WINDOWS"

LINK32=@link.exe
LINK32FLAGS=/dll /machine:x64 /nologo

AudioGraph64.dll: AudioGraph.obj convertaudio.obj
	$(LINK32) $(LINK32FLAGS) AudioGraph.obj convertaudio.obj /out:AudioGraph64.dll

convertaudio.obj: convertaudio.cpp
	$(CXX) $(CXXFLAGS) convertaudio.cpp -c

AudioGraph.obj: AudioGraph.cpp
	$(CXX) $(CXXFLAGS) AudioGraph.cpp -c

