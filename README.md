# AERA Media

An isolated audio and video player for AERA Recovery Project. The plugin reads
media from internal storage, decodes it with a private GStreamer runtime, sends
video frames through a sealed shared-memory channel, and sends PCM audio only
to AERA's protected recovery audio bridge. The browser presents lazy video
preview thumbnails before playback and releases off-screen previews to keep
recovery memory use bounded.

Supported initial formats include MP3, FLAC, WAV, AAC/M4A, Ogg/Opus, MP4,
Matroska and WebM, subject to the codecs present in each file.
