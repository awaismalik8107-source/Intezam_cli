# Dependencies

Runtime/build library: libtorrent-rasterbar 2.1.0 or newer (tested: 2.1.2),
including its packaged Boost/OpenSSL dependencies.

Build tools on Arch Linux / CachyOS:

```bash
sudo pacman -S --needed base-devel pkgconf cmake libtorrent-rasterbar
```

The application requires C++17. CMake/CTest build and run the tests without a
third-party test framework. VLC is planned and is not required for metadata
detection, selection or simulated cache playback. See [README.md](README.md)
for build and test commands.
