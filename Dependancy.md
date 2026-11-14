# Dependencies

Runtime/build library: libtorrent-rasterbar 2.1.0 or newer (tested: 2.1.2),
including its packaged Boost/OpenSSL dependencies, plus LibVLC 3.x
(tested: 3.0.24) and VLC demuxer/codec/output plugins.

Build tools on Arch Linux / CachyOS:

```bash
sudo pacman -S --needed base-devel pkgconf cmake libtorrent-rasterbar libvlc vlc vlc-plugins-all
```

The application requires C++17. CMake/CTest build and run the tests without a
third-party test framework. FFmpeg is an optional test-fixture generator:
`sudo pacman -S --needed ffmpeg`. The application does not link FFmpeg directly.
LibVLC 4 is not supported by this lifecycle implementation. See [README.md](README.md)
for build, playback and test commands.
