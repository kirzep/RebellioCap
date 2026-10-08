`editor-sample.mp4` is a generated six-second H.264/AAC browser acceptance fixture (640x360, 30 fps), containing FFmpeg testsrc2 and a 440 Hz sine tone. It has no third-party footage or speech. Keeping it with the test removes the dependency on a staged native FFmpeg executable and Windows encoder availability in the frontend CI job.

Generated with the repository's staged FFmpeg using `-filter_complex "testsrc2=s=640x360:r=30:d=6[v];sine=frequency=440:duration=6[a]" -map "[v]" -map "[a]" -c:v h264_mf -hw_encoding 0 -c:a aac -movflags +faststart`.

The separate clip-playback test still requires an externally recorded multi-track fixture and native preparation; its skip remains explicit.
