# AAC mux fixtures

These three Media Foundation AAC streams exercise independent mixed, system
and microphone tracks. Each JSON file records packet offsets, timestamps,
sample rate, channel count and AudioSpecificConfig for its paired binary file.

The mux test rebases each stream onto a synthetic video origin. These fixtures
test container output; they do not establish live audio/video synchronization.
