/*
 * Virtual DJ YouTube Music Plugin
 * Version: v1.1.0 (With Visual Feedback)
 *
 * -------------------------------------------------------------------------
 *  HOW TO USE THIS PLUGIN (Bridge Guide)
 * -------------------------------------------------------------------------
 *
 * This plugin requires a local backend ("bridge") that exposes a REST API.
 * The backend is NOT included. You must implement it yourself (see README).
 *
 * 1. Write or obtain a backend that exposes at least these endpoints:
 *    - GET /                 → returns { "status": "online", "service": "VDJ Bridge" }
 *    - GET /search?q=QUERY   → returns a JSON array of tracks
 *    - GET /get_url?id=ID    → returns { "videoId": ..., "streamUrl": ..., ... }
 *    - (optional) /playlists and /playlist_tracks?id=... for playlist support
 *
 * 2. Set the backend path:
 *    - Edit the GetBackendPath() function below to return the folder path
 *      where your backend (e.g. main.py) is located.
 *    - Example: return "C:/Users/YourName/Desktop/vdj_plugin_ytmusic/bridge/";
 *
 * 3. The plugin will attempt to auto-start the backend if not running.
 *    - It expects to find a file called main.py in the bridge path.
 *    - The backend must listen on http://127.0.0.1:8000
 *
 * 4. For backend implementation examples, see the README or use FastAPI + ytmusicapi.
 *
 * 5. This plugin does NOT provide or distribute any backend code, scripts, or third-party binaries.
 *    You are responsible for your own backend and for complying with all terms of service.
 *
 * -------------------------------------------------------------------------
 * Features:
 * - Search YouTube Music
 * - Browse user playlists (with authentication)
 * - Stream audio directly (no downloads)
 * - Auto-start Python backend on load
 * - Visual feedback overlay during stream URL fetching
 */

std::string BPath = "your/Path/to/bridge"; // Path to your backend bridge


#define _CRT_SECURE_NO_WARNINGS
#include "../sdk/vdjPlugin8.h"
#include "../sdk/vdjOnlineSource.h"
#include "../sdk/vdjDsp8.h"      // per GUID DSP/Buffer (host può interrogarli)
#include "../sdk/vdjVideo8.h"    // per GUID VideoFx/Transition

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <ctime>

#ifdef VDJ_WIN
#include <windows.h>
#include <Unknwn.h>
#include <winhttp.h>
#include <shellapi.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#else
#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif




// JSON parsing - using simple manual parsing to avoid dependencies
// For production, consider using nlohmann/json or rapidjson

//////////////////////////////////////////////////////////////////////////
// Forward declarations
inline std::string GetBackendPath();

//////////////////////////////////////////////////////////////////////////
// FeedbackOverlay - Toast-style visual feedback window
#ifdef VDJ_WIN
class FeedbackOverlay {
private:
    std::thread uiThread;
    std::atomic<bool> running;
    std::string message;
    HWND hWnd;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT rect;
            GetClientRect(hwnd, &rect);

            // Dark background, light text (VDJ style)
            HBRUSH bgBrush = CreateSolidBrush(RGB(40, 40, 40));
            FillRect(hdc, &rect, bgBrush);
            DeleteObject(bgBrush);

            // Blue border accent
            HBRUSH borderBrush = CreateSolidBrush(RGB(0, 120, 215));
            FrameRect(hdc, &rect, borderBrush);
            DeleteObject(borderBrush);

            // Text rendering
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            
            HFONT hFont = CreateFontA(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, 
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            SelectObject(hdc, hFont);

            LONG_PTR ptr = GetWindowLongPtr(hwnd, GWLP_USERDATA);
            if (ptr) {
                std::string* msgPtr = (std::string*)ptr;
                RECT textRect = rect;
                textRect.left += 10;
                DrawTextA(hdc, msgPtr->c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }

            DeleteObject(hFont);
            EndPaint(hwnd, &ps);
        }
        break;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        return 0;
    }

    void ThreadLoop() {
        WNDCLASSA wc = { 0 };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "VDJ_YTMusic_Feedback";
        wc.hCursor = LoadCursor(NULL, IDC_WAIT);
        RegisterClassA(&wc);

        POINT pt;
        GetCursorPos(&pt);
        int x = pt.x + 15;
        int y = pt.y + 15;
        int w = 250;
        int h = 40;

        hWnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            wc.lpszClassName,
            "Loading",
            WS_POPUP | WS_VISIBLE | WS_BORDER,
            x, y, w, h,
            NULL, NULL, wc.hInstance, NULL
        );

        if (hWnd) {
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)&message);
            
            MSG msg;
            while (running && GetMessage(&msg, NULL, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        
        if (hWnd && IsWindow(hWnd)) DestroyWindow(hWnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
    }

public:
    FeedbackOverlay() : running(false), hWnd(NULL) {}

    ~FeedbackOverlay() {
        Stop();
    }

    void Show(const std::string& msg) {
        Stop();
        message = msg;
        running = true;
        uiThread = std::thread(&FeedbackOverlay::ThreadLoop, this);
    }

    void Stop() {
        running = false;
        if (hWnd && IsWindow(hWnd)) {
            PostMessage(hWnd, WM_CLOSE, 0, 0);
        }
        if (uiThread.joinable()) {
            uiThread.join();
        }
        hWnd = NULL;
    }
};
#else
// Stub for non-Windows platforms
class FeedbackOverlay {
public:
    void Show(const std::string& msg) {}
    void Stop() {}
};
#endif

//////////////////////////////////////////////////////////////////////////
// Logging helper
class Logger {
private:
    static std::string GetLogPath() {
        std::string backendPath = GetBackendPath();
        return backendPath + "\\plugin.log";
    }

    static std::string GetTimestamp() {
        time_t now = time(0);
        struct tm tstruct;
        char buf[80];
        localtime_s(&tstruct, &now);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tstruct);
        return buf;
    }

public:
    static void Log(const std::string& message) {
        std::string logMsg = "[" + GetTimestamp() + "] " + message;
        
#ifdef VDJ_WIN
        // Output to debugger
        OutputDebugStringA(("[YTMusic] " + logMsg + "\n").c_str());
#endif
        
        // Write to log file
        std::ofstream logFile(GetLogPath(), std::ios::app);
        if (logFile.is_open()) {
            logFile << logMsg << std::endl;
            logFile.close();
        }
    }

    static void Error(const std::string& message) {
        Log("ERROR: " + message);
#ifdef VDJ_WIN
        MessageBoxA(NULL, message.c_str(), "YouTube Music Plugin Error", MB_OK | MB_ICONERROR);
#endif
    }
};

//////////////////////////////////////////////////////////////////////////
// Helper class for HTTP requests
// Get backend directory path - points to Desktop/vdj_plugin_ytmusic/bridge
// Returns the path to your backend bridge. Edit this to match your backend location.
inline std::string GetBackendPath() {
    return BPath;
}

//////////////////////////////////////////////////////////////////////////
// Simple JSON parser (minimal implementation)
class SimpleJSON {
public:
    static std::string ExtractString(const std::string& json, const std::string& key) {
        std::string searchKey = "\"" + key + "\":\"";
        size_t pos = json.find(searchKey);
        if (pos == std::string::npos) {
            searchKey = "\"" + key + "\": \"";
            pos = json.find(searchKey);
        }
        if (pos == std::string::npos) return "";
        
        pos += searchKey.length();
        size_t endPos = json.find("\"", pos);
        if (endPos == std::string::npos) return "";
        
        return json.substr(pos, endPos - pos);
    }

    static int ExtractInt(const std::string& json, const std::string& key) {
        std::string searchKey = "\"" + key + "\":";
        size_t pos = json.find(searchKey);
        if (pos == std::string::npos) {
            searchKey = "\"" + key + "\": ";
            pos = json.find(searchKey);
        }
        if (pos == std::string::npos) return 0;
        
        pos += searchKey.length();
        size_t endPos = json.find_first_of(",}\n", pos);
        if (endPos == std::string::npos) return 0;
        
        std::string numStr = json.substr(pos, endPos - pos);
        try {
            return std::stoi(numStr);
        } catch (...) {
            return 0;
        }
    }

    static bool ExtractBool(const std::string& json, const std::string& key) {
        // Handle both JSON booleans and quoted "true"/"false" values
        std::string searchKey = "\"" + key + "\":";
        size_t pos = json.find(searchKey);
        if (pos != std::string::npos) {
            pos += searchKey.length();
            // Skip optional whitespace
            while (pos < json.size() && isspace(static_cast<unsigned char>(json[pos]))) {
                ++pos;
            }
            if (json.compare(pos, 4, "true") == 0) return true;
            if (json.compare(pos, 5, "false") == 0) return false;
        }

        std::string value = ExtractString(json, key);
        if (!value.empty()) {
            return value == "true" || value == "True";
        }
        return false;
    }

    static std::vector<std::string> ExtractArray(const std::string& json) {
        std::vector<std::string> items;
        
        size_t start = json.find('[');
        size_t end = json.rfind(']');
        if (start == std::string::npos || end == std::string::npos) return items;
        
        int braceLevel = 0;
        size_t itemStart = start + 1;
        
        for (size_t i = start + 1; i < end; i++) {
            if (json[i] == '{') braceLevel++;
            else if (json[i] == '}') {
                braceLevel--;
                if (braceLevel == 0) {
                    items.push_back(json.substr(itemStart, i - itemStart + 1));
                    itemStart = i + 2; // Skip comma and space
                }
            }
        }
        
        return items;
    }
};

//////////////////////////////////////////////////////////////////////////
// Helper class for HTTP requests
class HttpClient {
private:
    std::string baseUrl;
    
#ifdef VDJ_WIN
    HINTERNET hSession;
    HINTERNET hConnect;
#else
    CURL* curl;
#endif

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

public:
    HttpClient() : baseUrl("http://127.0.0.1:8000") {
#ifdef VDJ_WIN
        hSession = WinHttpOpen(L"VDJ-YTMusic/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (hSession) {
            hConnect = WinHttpConnect(hSession, L"127.0.0.1", 8000, 0);
        }
#else
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = curl_easy_init();
#endif
    }

    ~HttpClient() {
#ifdef VDJ_WIN
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);
#else
        if (curl) curl_easy_cleanup(curl);
        curl_global_cleanup();
#endif
    }

    std::string Get(const std::string& endpoint) {
        std::string response;
        
#ifdef VDJ_WIN
        if (!hConnect) return "";
        
        std::wstring wEndpoint(endpoint.begin(), endpoint.end());
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
            wEndpoint.c_str(), NULL, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, NULL)) {
                
                DWORD dwSize = 0;
                DWORD dwDownloaded = 0;
                do {
                    dwSize = 0;
                    if (WinHttpQueryDataAvailable(hRequest, &dwSize) && dwSize > 0) {
                        char* buffer = new char[dwSize + 1];
                        ZeroMemory(buffer, dwSize + 1);
                        
                        if (WinHttpReadData(hRequest, buffer, dwSize, &dwDownloaded)) {
                            response.append(buffer, dwDownloaded);
                        }
                        delete[] buffer;
                    }
                } while (dwSize > 0);
            }
            WinHttpCloseHandle(hRequest);
        }
#else
        if (!curl) return "";
        
        std::string url = baseUrl + endpoint;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            return "";
        }
#endif
        
        return response;
    }

    bool IsServerAlive() {
        std::string response = Get("/");
        bool alive = !response.empty() && response.find("\"status\"") != std::string::npos;
        if (alive) {
            Logger::Log("HttpClient: Server is alive");
        } else {
            Logger::Log("HttpClient: Server not responding");
        }
        return alive;
    }

    bool IsAuthenticated() {
        std::string response = Get("/auth_status");
        if (response.empty()) return false;
        bool auth = SimpleJSON::ExtractBool(response, "authenticated");
        Logger::Log(std::string("HttpClient: Authenticated = ") + (auth ? "true" : "false"));
        return auth;
    }
};

//////////////////////////////////////////////////////////////////////////
// Track data structure
struct Track {
    std::string videoId;
    std::string title;
    std::string artist;
    std::string album;
    float duration;
    std::string thumbnail;
    bool isVideo;
};

//////////////////////////////////////////////////////////////////////////
// Playlist data structure
struct Playlist {
    std::string playlistId;
    std::string title;
    int count;
    std::string thumbnail;
};

//////////////////////////////////////////////////////////////////////////
// Main plugin class
class YouTubeMusicPlugin : public IVdjPluginOnlineSource {
private:
    HttpClient httpClient;
    FeedbackOverlay feedback;
    std::vector<Track> searchResults;
    std::vector<Playlist> userPlaylists;
    std::vector<Track> currentPlaylistTracks;
    std::string currentFolder;
    bool backendRunning;
    HANDLE pythonProcess;
    std::mutex dataMutex;
    bool authPromptShown = false;

    void OpenConfigPageIfNeeded() {
    if (authPromptShown) return;

    // Only trigger once per session
    authPromptShown = true;
    std::string url = "http://127.0.0.1:8000/config";
    Logger::Log("Opening config page: " + url);

#ifdef VDJ_WIN
    ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open '" + url + "'";
    system(cmd.c_str());
#else
    std::string cmd = "xdg-open '" + url + "'";
    system(cmd.c_str());
#endif
    }

    // URL encoding helper
    std::string UrlEncode(const std::string& value) {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (char c : value) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << c;
            } else if (c == ' ') {
                escaped << '+';
            } else {
                escaped << '%' << std::setw(2) << std::hex << std::uppercase << int((unsigned char)c);
            }
        }

        return escaped.str();
    }

    // Start Python backend if not running
    bool EnsureBackendRunning() {
        Logger::Log("EnsureBackendRunning: Checking backend status...");
        
        if (backendRunning && httpClient.IsServerAlive()) {
            Logger::Log("EnsureBackendRunning: Backend already running");
            return true;
        }

        Logger::Log("EnsureBackendRunning: Backend not running, attempting to start...");
        std::string backendPath = GetBackendPath();
        Logger::Log("EnsureBackendRunning: Backend path = " + backendPath);

#ifdef VDJ_WIN
        // Build command to start Python backend
        std::string pythonScript = backendPath + "\\main.py";
        Logger::Log("EnsureBackendRunning: Python script = " + pythonScript);
        
        // Check if Python script exists
        DWORD fileAttr = GetFileAttributesA(pythonScript.c_str());
        if (fileAttr == INVALID_FILE_ATTRIBUTES) {
            Logger::Error("Python backend not installed at: " + pythonScript);
            return false;
        }
        
        Logger::Log("EnsureBackendRunning: Python script found, starting backend...");

        // Try to start Python backend using pythonw (no console window)
        std::string command = "cmd.exe /c cd /d \"" + backendPath + "\" && start /B pythonw main.py";
        Logger::Log("EnsureBackendRunning: Command = " + command);

        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        if (CreateProcessA(NULL, (LPSTR)command.c_str(), NULL, NULL, FALSE,
            CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, backendPath.c_str(), &si, &pi)) {
            pythonProcess = pi.hProcess;
            CloseHandle(pi.hThread);
            
            Logger::Log("EnsureBackendRunning: Process started, waiting 4 seconds...");

            // Wait for server to start
            Sleep(4000);

            backendRunning = httpClient.IsServerAlive();
            if (backendRunning) {
                Logger::Log("EnsureBackendRunning: Backend started successfully!");
            } else {
                Logger::Error("Backend process started but server not responding");
            }
            return backendRunning;
        } else {
            DWORD error = GetLastError();
            Logger::Error("Failed to start backend process. Error code: " + std::to_string(error));
        }
#elif defined(__APPLE__) || defined(__linux__)
        // macOS/Linux
        std::string pythonScript = backendPath + "/main.py";
        
        // Check if Python script exists
        struct stat buffer;
        if (stat(pythonScript.c_str(), &buffer) != 0) {
            return false;
        }

        // Start Python backend in background
        std::string command = "cd \"" + backendPath + "\" && python3 main.py &";
        int result = system(command.c_str());
        
        if (result == 0) {
            sleep(4);
            backendRunning = httpClient.IsServerAlive();
            return backendRunning;
        }
#endif
        return false;
    }

    // Check authentication and open config page if not authenticated
    void EnsureAuthUI() {
        bool auth = httpClient.IsAuthenticated();
        if (!auth) {
            Logger::Log("Auth not configured, launching config page...");
            OpenConfigPageIfNeeded();
        }
    }

    // Parse tracks from JSON array
    std::vector<Track> ParseTracks(const std::string& json) {
        std::vector<Track> tracks;
        std::vector<std::string> items = SimpleJSON::ExtractArray(json);

        for (const auto& item : items) {
            Track track;
            track.videoId = SimpleJSON::ExtractString(item, "videoId");
            track.title = SimpleJSON::ExtractString(item, "title");
            track.artist = SimpleJSON::ExtractString(item, "artist");
            track.album = SimpleJSON::ExtractString(item, "album");
            track.duration = (float)SimpleJSON::ExtractInt(item, "duration");
            track.thumbnail = SimpleJSON::ExtractString(item, "thumbnail");
            track.isVideo = SimpleJSON::ExtractBool(item, "isVideo");

            if (!track.videoId.empty() && !track.title.empty()) {
                tracks.push_back(track);
            }
        }

        return tracks;
    }

    // Parse playlists from JSON array
    std::vector<Playlist> ParsePlaylists(const std::string& json) {
        std::vector<Playlist> playlists;
        std::vector<std::string> items = SimpleJSON::ExtractArray(json);

        for (const auto& item : items) {
            Playlist playlist;
            playlist.playlistId = SimpleJSON::ExtractString(item, "playlistId");
            playlist.title = SimpleJSON::ExtractString(item, "title");
            playlist.count = SimpleJSON::ExtractInt(item, "count");
            playlist.thumbnail = SimpleJSON::ExtractString(item, "thumbnail");

            if (!playlist.playlistId.empty() && !playlist.title.empty()) {
                playlists.push_back(playlist);
            }
        }

        return playlists;
    }

public:
    YouTubeMusicPlugin() : backendRunning(false), pythonProcess(NULL) {}

    ~YouTubeMusicPlugin() {
#ifdef VDJ_WIN
        if (pythonProcess) {
            TerminateProcess(pythonProcess, 0);
            CloseHandle(pythonProcess);
        }
#endif
    }

    //////////////////////////////////////////////////////////////////////////
    // IVdjPlugin8 interface

    HRESULT VDJ_API OnLoad() {
        Logger::Log("=== YouTube Music Plugin Loading ===");
        Logger::Log("OnLoad: Plugin initialized");
        
        // Try to start backend
        bool started = EnsureBackendRunning();
        if (started) {
            Logger::Log("OnLoad: Backend started successfully");
            EnsureAuthUI();
        } else {
            Logger::Error("OnLoad: Failed to start backend");
        }
        
        return S_OK;
    }

    HRESULT VDJ_API OnGetPluginInfo(TVdjPluginInfo8* infos) {
        infos->PluginName = "YouTube Music";
        infos->Author = "VDJ Bridge";
        infos->Description = "Stream music from YouTube Music";
        infos->Version = "v1.0.0";
        infos->Flags = 0;
        infos->Bitmap = NULL;
        return S_OK;
    }

    //////////////////////////////////////////////////////////////////////////
    // IVdjPluginOnlineSource interface

    HRESULT VDJ_API OnSearch(const char* search, IVdjTracksList* tracksList) {
        Logger::Log("=== OnSearch called ===");
        Logger::Log("OnSearch: Query = '" + std::string(search) + "'");
        
        if (!EnsureBackendRunning()) {
            Logger::Error("OnSearch: Backend not available");
            return E_FAIL;
        }

        std::string endpoint = "/search?q=" + UrlEncode(search);
        Logger::Log("OnSearch: Endpoint = " + endpoint);
        Logger::Log("OnSearch: Making HTTP request...");
        
        std::string response = httpClient.Get(endpoint);

        if (response.empty()) {
            Logger::Error("OnSearch: Empty response from backend");
            return E_FAIL;
        }
        
        Logger::Log("OnSearch: Response received (" + std::to_string(response.length()) + " bytes)");
        Logger::Log("OnSearch: Response preview: " + response.substr(0, 200));

        std::lock_guard<std::mutex> lock(dataMutex);
        searchResults = ParseTracks(response);
        
        Logger::Log("OnSearch: Parsed " + std::to_string(searchResults.size()) + " tracks");

        for (const auto& track : searchResults) {
            Logger::Log("OnSearch: Adding track: " + track.title + " by " + track.artist);
            tracksList->add(
                track.videoId.c_str(),
                track.title.c_str(),
                track.artist.c_str(),
                nullptr, // remix
                nullptr, // genre
                nullptr, // label
                track.album.c_str(), // comment (using album)
                track.thumbnail.c_str(), // coverUrl
                nullptr, // streamUrl (will provide later via GetStreamUrl)
                track.duration,
                0.0f, // bpm
                0, // key
                0, // year
                track.isVideo,
                false // isKaraoke
            );
        }

        Logger::Log("OnSearch: Search completed successfully");
        return S_OK;
    }

    HRESULT VDJ_API OnSearchCancel() {
        Logger::Log("=== OnSearchCancel called ===");
        // Cancel any ongoing search operations if needed
        return S_OK;
    }

    HRESULT VDJ_API GetStreamUrl(const char* uniqueId, IVdjString& url, IVdjString& errorMessage) {
        Logger::Log("=== GetStreamUrl called ===");
        Logger::Log("GetStreamUrl: Video ID = " + std::string(uniqueId));
        
        // Show visual feedback overlay
        feedback.Show("Downloading from YouTube...");
        
        if (!EnsureBackendRunning()) {
            feedback.Stop();
            Logger::Error("GetStreamUrl: Backend not available");
            errorMessage = "Backend not available";
            return E_FAIL;
        }

        std::string endpoint = "/get_url?id=" + std::string(uniqueId);
        Logger::Log("GetStreamUrl: Requesting " + endpoint);
        
        // This is a blocking call that takes 3-5 seconds
        // The overlay will remain visible thanks to the separate thread
        std::string response = httpClient.Get(endpoint);
        
        // Hide visual feedback
        feedback.Stop();

        if (response.empty()) {
            feedback.Stop();
            Logger::Error("GetStreamUrl: Empty response from backend");
            errorMessage = "Failed to get stream URL";
            return E_FAIL;
        }
        
        Logger::Log("GetStreamUrl: Response received (" + std::to_string(response.length()) + " bytes)");
        Logger::Log("GetStreamUrl: Response preview: " + response.substr(0, 400));

        std::string streamUrl = SimpleJSON::ExtractString(response, "streamUrl");
        if (streamUrl.empty()) {
            // Fallback: some responses might use "url" or return "detail" on error
            streamUrl = SimpleJSON::ExtractString(response, "url");
        }
        
        if (streamUrl.empty()) {
            std::string detail = SimpleJSON::ExtractString(response, "detail");
            Logger::Error("GetStreamUrl: No streamUrl in response. Detail: " + detail + "; Raw: " + response.substr(0, 400));
            errorMessage = detail.empty() ? "Stream URL not available" : detail.c_str();
            return E_FAIL;
        }

        Logger::Log("GetStreamUrl: Stream URL = " + streamUrl.substr(0, 100) + "...");
        url = streamUrl.c_str();
        return S_OK;
    }

    HRESULT VDJ_API GetFolderList(IVdjSubfoldersList* subfoldersList) {
        // No subfolders
        return S_OK;
    }

    HRESULT VDJ_API GetFolder(const char* folderUniqueId, IVdjTracksList* tracksList) {
        if (!EnsureBackendRunning()) {
            return E_FAIL;
        }

        std::string folderId(folderUniqueId);

        if (folderId == "search") {
            // Search folder - return cached search results
            std::lock_guard<std::mutex> lock(dataMutex);
            for (const auto& track : searchResults) {
                tracksList->add(
                    track.videoId.c_str(),
                    track.title.c_str(),
                    track.artist.c_str(),
                    nullptr, nullptr, nullptr,
                    track.album.c_str(),
                    track.thumbnail.c_str(),
                    nullptr,
                    track.duration,
                    0.0f, 0, 0,
                    track.isVideo,
                    false
                );
            }
            return S_OK;
        }
        else if (folderId == "playlists") {
            // Load user playlists
            std::string response = httpClient.Get("/playlists");
            if (response.empty()) {
                return E_FAIL;
            }

            std::lock_guard<std::mutex> lock(dataMutex);
            userPlaylists = ParsePlaylists(response);

            // Add playlists as "folders"
            // Note: VDJ doesn't support nested folders in OnlineSource
            // We'll return empty here and handle in context menu
            return S_OK;
        }
        else {
            // Specific playlist
            std::string endpoint = "/playlist_tracks?id=" + folderId;
            std::string response = httpClient.Get(endpoint);

            if (response.empty()) {
                return E_FAIL;
            }

            std::vector<Track> tracks = ParseTracks(response);

            for (const auto& track : tracks) {
                tracksList->add(
                    track.videoId.c_str(),
                    track.title.c_str(),
                    track.artist.c_str(),
                    nullptr, nullptr, nullptr,
                    track.album.c_str(),
                    track.thumbnail.c_str(),
                    nullptr,
                    track.duration,
                    0.0f, 0, 0,
                    track.isVideo,
                    false
                );
            }

            return S_OK;
        }
    }
};

//////////////////////////////////////////////////////////////////////////
// DLL Export

#define VDJINTERFACE8 8

// Helper function to convert GUID to string for logging
std::string GuidToString(const GUID& guid) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return std::string(buf);
}

extern "C" {
    VDJ_EXPORT HRESULT VDJ_API DllGetClassObject(const GUID& rclsid, const GUID& riid, void** ppObject) {
        Logger::Log("=== DllGetClassObject called ===");
        Logger::Log("DllGetClassObject: rclsid = " + GuidToString(rclsid));
        Logger::Log("DllGetClassObject: riid = " + GuidToString(riid));
        Logger::Log("DllGetClassObject: CLSID_VdjPlugin8 = " + GuidToString(CLSID_VdjPlugin8));
        Logger::Log("DllGetClassObject: IID_IVdjPluginOnlineSource = " + GuidToString(IID_IVdjPluginOnlineSource));
        Logger::Log("DllGetClassObject: IID_IVdjPluginBasic8 = " + GuidToString(IID_IVdjPluginBasic8));
        Logger::Log("DllGetClassObject: IID_IVdjPluginDsp8 = " + GuidToString(IID_IVdjPluginDsp8));
        Logger::Log("DllGetClassObject: IID_IVdjPluginBuffer8 = " + GuidToString(IID_IVdjPluginBuffer8));
        Logger::Log("DllGetClassObject: IID_IVdjPluginVideoFx8 = " + GuidToString(IID_IVdjPluginVideoFx8));
        Logger::Log("DllGetClassObject: IID_IVdjPluginVideoTransition8 = " + GuidToString(IID_IVdjPluginVideoTransition8));
        Logger::Log("DllGetClassObject: IID_IVdjPluginVideoTransitionMultiDeck8 = " + GuidToString(IID_IVdjPluginVideoTransitionMultiDeck8));
#ifdef _WIN32
    Logger::Log("DllGetClassObject: IID_IUnknown = " + GuidToString(IID_IUnknown));
#endif
        
    bool clsidPlugin   = memcmp(&rclsid, &CLSID_VdjPlugin8, sizeof(GUID)) == 0;
    bool clsidOnline   = memcmp(&rclsid, &IID_IVdjPluginOnlineSource, sizeof(GUID)) == 0; // alcuni host passano l'IIDs come CLSID
    bool riidOnline    = memcmp(&riid, &IID_IVdjPluginOnlineSource, sizeof(GUID)) == 0;
    bool riidBasic     = memcmp(&riid, &IID_IVdjPluginBasic8, sizeof(GUID)) == 0;
    bool riidDsp       = memcmp(&riid, &IID_IVdjPluginDsp8, sizeof(GUID)) == 0;
    bool riidBuffer    = memcmp(&riid, &IID_IVdjPluginBuffer8, sizeof(GUID)) == 0;
    bool riidVideoFx   = memcmp(&riid, &IID_IVdjPluginVideoFx8, sizeof(GUID)) == 0;
    bool riidVideoTr   = memcmp(&riid, &IID_IVdjPluginVideoTransition8, sizeof(GUID)) == 0;
    bool riidVideoTrMd = memcmp(&riid, &IID_IVdjPluginVideoTransitionMultiDeck8, sizeof(GUID)) == 0;
#ifdef _WIN32
    bool riidUnknown   = memcmp(&riid, &IID_IUnknown, sizeof(GUID)) == 0;
#else
    bool riidUnknown   = false;
#endif

    if ((clsidPlugin || clsidOnline) && (riidOnline || riidBasic || riidDsp || riidBuffer || riidVideoFx || riidVideoTr || riidVideoTrMd || riidUnknown)) {
        Logger::Log("DllGetClassObject: GUIDs accepted, creating YouTubeMusicPlugin instance");
        *ppObject = new YouTubeMusicPlugin();
        return NO_ERROR;
    }
        
        Logger::Error("DllGetClassObject: GUID mismatch - plugin not loaded");
        return CLASS_E_CLASSNOTAVAILABLE;
    }
}
