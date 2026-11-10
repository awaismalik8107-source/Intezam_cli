# Intezam CLI

**A command-line torrent downloader and video streaming tool powered by libtorrent and VLC.**

Intezam CLI aims to let users watch torrent video files while they are still downloading. Instead of waiting for the entire video to finish, the goal is to download the required video pieces, make them available to VLC, and start playback as early as possible.

## Features

### Current implementation
- Accepts magnet links from the terminal.
- Uses libtorrent as the BitTorrent engine.
- Enables DHT, LSD, UPnP, and NAT-PMP.
- Downloads torrent content into a `test` directory inside the current working directory.
- Displays download progress, download speed, connected peers, and seeds.
- Prints libtorrent alerts and status messages.

### Planned features
- Automatically detect video files inside a torrent.
- Prioritize video pieces to support playback before completion.
- Integrate VLC for video playback.
- Start streaming while the remaining pieces continue downloading.
- Improve error handling and download management.

## Requirements

- Linux
- C++ compiler (G++)
- C++17 support
- libtorrent-rasterbar
- pkg-config
- VLC (planned integration)

## Installation

On Arch Linux / CachyOS:

```bash
sudo pacman -S base-devel pkgconf libtorrent-rasterbar vlc
```

## Compilation

Compile the project using:

```fish
g++ -std=c++17 main.cpp (pkg-config --cflags --libs libtorrent-rasterbar | string split ' ') -pthread -o intezam
```

This command:

- `g++` — compiles the C++ source code.
- `-std=c++17` — enables C++17 features.
- `pkg-config --cflags --libs libtorrent-rasterbar` — supplies the required libtorrent compiler and linker flags.
- `string split ' '` — splits those flags for Fish shell.
- `-pthread` — enables thread support.
- `-o intezam` — names the compiled executable `intezam`.

## Running

```bash
./intezam
```

Paste a valid magnet link when prompted.

## Download Location

Intezam CLI is designed to save downloaded files in a `test` folder inside the directory from which the program is launched.

For example:

```text
Intezam_cli/
├── main.cpp
├── intezam
└── test/
    └── downloaded-video.mkv
```

The download directory is created automatically if it does not exist.

## How It Works

1. The user provides a magnet link.
2. Intezam parses the magnet URI using libtorrent.
3. A torrent session is initialized with DHT and other peer-discovery features enabled.
4. The torrent is added to the session.
5. Libtorrent manages peer connections and downloads torrent pieces.
6. Intezam displays live download statistics.
7. The planned streaming component will make the video available to VLC before the complete download finishes.

## Development Roadmap

| Stage | Objective | Status |
|---|---|---|
| 1 | Initialize libtorrent session | Done |
| 2 | Accept and parse magnet links | Done |
| 3 | Enable peer discovery | Done |
| 4 | Download torrent files | In progress |
| 5 | Display live download statistics | Done |
| 6 | Detect video files | Planned |
| 7 | Prioritize video pieces | Planned |
| 8 | Integrate VLC playback | Planned |
| 9 | Stream while downloading | Planned |

## Project Goal

The long-term goal of Intezam CLI is to combine torrent downloading and media playback into one simple command-line application, allowing users to start watching video content without waiting for the complete download.

## License

To be decided.
