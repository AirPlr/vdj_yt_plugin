# VirtualDJ YouTube Music Plugin

This repository contains **only the source code for the VirtualDJ YouTube Music Plugin (C++)**. Due to copyright and legal restrictions, **no backend, bridge, or third-party binaries/scripts are provided**.

## What is this?
This is a C++ plugin for VirtualDJ that allows integration with YouTube Music via a custom bridge (not included). The plugin communicates with a local HTTP backend (the "bridge") that you must implement yourself.

## Why is only the plugin source provided?
- The backend/bridge relies on third-party tools and APIs (e.g., yt-dlp, YouTube Music API) that may have copyright, licensing, or terms of service issues.
- Distributing such code or binaries could violate the terms of service of YouTube or other services.
- You are responsible for ensuring your use complies with all relevant laws and terms.

## How to use this plugin
1. **Build the plugin**
  - Open the project in Visual Studio (or your preferred C++ IDE).
  - Build the `YouTubeMusicPlugin.cpp` file as a VirtualDJ plugin (DLL).
  - Place the resulting DLL in your VirtualDJ plugins folder.

2. **Write your own bridge/backend**
  - The plugin expects a local HTTP API (the bridge) to provide search, playlist, and streaming endpoints.
  - You can write the bridge in any language (Python, Node.js, etc.).
  - The bridge should:
    - Accept search queries and return YouTube Music results (JSON).
    - Accept requests for streaming URLs and return a local file URI or stream.
    - Optionally, provide playlist browsing endpoints.
  - See below for a minimal API specification.

### Minimal Bridge API Specification
- `GET /` — Returns `{ "status": "online", "service": "VDJ Bridge" }` (for health check)
- `GET /search?q=QUERY` — Returns a list of tracks (JSON array)
- `GET /get_url?id=VIDEO_ID` — Returns `{ "videoId": ..., "streamUrl": ..., "title": ..., "ext": ... }`
- (Optional) `GET /playlists` and `GET /playlist_tracks?id=...` for playlist support

#### Example (Python FastAPI)
You can use [FastAPI](https://fastapi.tiangolo.com/) and [ytmusicapi](https://ytmusicapi.readthedocs.io/) to implement the bridge. See the comments in the plugin source for expected request/response formats.

**Note:** You must handle authentication, cookies, and API changes yourself. This repository does not provide support for the backend.

## Disclaimer
- This project is for educational purposes only.
- Use at your own risk. The author is not responsible for any misuse or legal issues.
- YouTube, YouTube Music, and VirtualDJ are trademarks of their respective owners.

---

For questions or contributions, open an issue or pull request.
---
