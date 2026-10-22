// Updates (update_check.h). HTTPS through the operating system, no bundled TLS library: WinHTTP on
// Windows; on Linux the system's curl (present on SteamOS and common distributions; without it the
// check says so). Archives are unpacked with the system's tar (Windows 10 1803+ ships bsdtar, which
// reads .zip; on Linux, python3 unpacks when tar cannot read .xz).
// Test hook: BLITZ_UPDATE_API=http://127.0.0.1:PORT replaces https://api.github.com, and downloads are
// then accepted only from that server.
#include "update_check.h"

#include "log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef BLITZ_VERSION
#define BLITZ_VERSION "dev"
#endif

namespace fs = std::filesystem;

namespace update {

namespace {

// Never destroyed: a check still running when the program exits must not touch freed statics.
std::mutex& g_mutex = *new std::mutex;
Release& g_release = *new Release;
Install& g_install = *new Install;

#ifdef _WIN32
constexpr const char* kPackage = "NFLBlitz2003-windows-x64.zip";
constexpr const char* kExe = "NFLBlitz2003.exe";
#else
constexpr const char* kPackage = "NFLBlitz2003-linux-x64.tar.xz";
constexpr const char* kExe = "NFLBlitz2003";
#endif
constexpr const char* kBackup = ".old-update";

fs::path exeDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return fs::path(std::wstring(buf, n)).parent_path();
#else
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

std::string utf8(const fs::path& p) {
  auto u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool validRepo(const std::string& r) {  // owner/name; also keeps the curl command line literal
  if (r.empty() || r.size() > 200 || r.find('/') == std::string::npos || r.find('/') != r.rfind('/')) return false;
  for (char c : r)
    if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.' && c != '/') return false;
  return r.front() != '/' && r.back() != '/' && r.find("..") == std::string::npos;
}

bool validUrl(const std::string& u) {  // characters a release URL uses; nothing a shell would interpret
  if (u.size() > 2000) return false;
  for (char c : u)
    if (!isalnum((unsigned char)c) && !strchr(":/._~%-+=?&", c)) return false;
  return true;
}

std::string testApi() {
  const char* t = getenv("BLITZ_UPDATE_API");
  return t && *t ? t : "";
}
std::string apiBase() {
  std::string t = testApi();
  return t.empty() ? "https://api.github.com" : t;
}
std::string userAgent() { return std::string("NFLBlitz2003/") + BLITZ_VERSION; }

// SHA-256 (FIPS 180-4) of a buffer, lower-case hex.
std::string sha256Hex(const std::vector<uint8_t>& data) {
  static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
      0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
      0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
      0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  std::vector<uint8_t> m(data);
  uint64_t bits = (uint64_t)data.size() * 8;
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 7; i >= 0; i--) m.push_back((uint8_t)(bits >> (i * 8)));
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t)m[off + 4 * i] << 24 | (uint32_t)m[off + 4 * i + 1] << 16 | (uint32_t)m[off + 4 * i + 2] << 8 |
             m[off + 4 * i + 3];
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
  }
  char out[65];
  for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
  return out;
}

// GET url, the body streamed to `sink` (false: stop). Returns the HTTP status (0: no connection,
// `error` says why).
int httpGet(const std::string& url, bool api, const std::function<bool(const char*, size_t)>& sink, std::string& error) {
  if (!validUrl(url)) {
    error = "unexpected address " + url;
    return 0;
  }
#ifdef _WIN32
  const std::wstring wurl(url.begin(), url.end());  // ASCII (validUrl)
  wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = 2048;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    error = "unexpected address " + url;
    return 0;
  }
  const std::string ua8 = userAgent();
  const std::wstring ua(ua8.begin(), ua8.end());
  int code = 0;
  HINTERNET session = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session)
    session = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET connect = session ? WinHttpConnect(session, host, uc.nPort, 0) : nullptr;
  const std::wstring object = std::wstring(path) + extra;
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                              : nullptr;
  if (request) {
    WinHttpSetTimeouts(request, 10000, 10000, 15000, 30000);
    const wchar_t* headers =
        api ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (WinHttpSendRequest(request, headers, api ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
      DWORD sc = 0, len = sizeof(sc);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &sc, &len, WINHTTP_NO_HEADER_INDEX);
      code = (int)sc;
      std::vector<char> buf(1 << 16);
      for (DWORD got = 0;;) {
        if (!WinHttpReadData(request, buf.data(), (DWORD)buf.size(), &got)) {
          error = "the download was interrupted";
          code = 0;
          break;
        }
        if (!got || !sink(buf.data(), got)) break;
      }
    }
  }
  if (!code && error.empty())
    error = "no connection to " + std::string(host, host + wcslen(host)) + " (error " + std::to_string(GetLastError()) + ")";
  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  if (session) WinHttpCloseHandle(session);
  return code;
#else
  // API answers are small: the status code is appended on its own line. Downloads stream the body;
  // -f turns an HTTP error into curl's exit status 22. LD_PRELOAD (Steam's overlay) is dropped.
  const std::string cmd = std::string("env -u LD_PRELOAD curl -sL --connect-timeout 10 ") +
                          (api ? "--max-time 15 -w '\\n%{http_code}' " : "-f ") + "-A '" + userAgent() + "' " +
                          (api ? "-H 'Accept: application/vnd.github+json' -H 'X-GitHub-Api-Version: 2022-11-28' " : "") +
                          "'" + url + "' 2>/dev/null";
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) {
    error = "could not run curl";
    return 0;
  }
  std::string body;
  std::vector<char> buf(1 << 16);
  bool stopped = false;
  for (size_t n; (n = fread(buf.data(), 1, buf.size(), p)) > 0;) {
    if (api) {
      if (body.size() < (1u << 20)) body.append(buf.data(), n);
    } else if (!sink(buf.data(), n)) {
      stopped = true;
      break;
    }
  }
  const int rc = pclose(p);
  const int exitCode = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
  if (exitCode == 127) {
    error = "curl is not installed";
    return 0;
  }
  if (api) {
    const size_t nl = body.rfind('\n');
    const int code = nl == std::string::npos ? 0 : atoi(body.c_str() + nl + 1);
    if (nl != std::string::npos) body.resize(nl);
    if (!code) {
      error = "no connection to github.com";
      return 0;
    }
    sink(body.data(), body.size());
    return code;
  }
  if (stopped) return 200;
  if (exitCode == 22) {
    error = "the server refused the download";
    return 404;
  }
  if (exitCode != 0) {
    error = "the download failed (curl " + std::to_string(exitCode) + ")";
    return 0;
  }
  return 200;
#endif
}

int httpGetString(const std::string& url, bool api, std::string& body, std::string& error) {
  return httpGet(url, api, [&](const char* d, size_t n) {
    if (body.size() < (8u << 20)) body.append(d, n);
    return true;
  }, error);
}

std::vector<long> versionParts(const std::string& v) {
  size_t i = 0;
  if (i < v.size() && (v[i] == 'v' || v[i] == 'V')) ++i;
  std::vector<long> parts;
  while (i < v.size() && isdigit((unsigned char)v[i])) {
    long n = 0;
    while (i < v.size() && isdigit((unsigned char)v[i])) n = n * 10 + (v[i++] - '0');
    parts.push_back(n);
    if (i < v.size() && v[i] == '.') ++i;
    else break;
  }
  return parts;
}

// Downloads come only from the repository's own release files (GitHub then redirects to its storage
// host), or from the test server.
bool downloadAllowed(const std::string& url) {
  const std::string t = testApi();
  if (!t.empty()) return url.rfind(t + "/", 0) == 0;
  return url.rfind("https://github.com/" + std::string(repository()) + "/releases/download/", 0) == 0;
}

void setInstall(Install::State s, float fraction = 0, const std::string& error = {}) {
  std::lock_guard lk(g_mutex);
  g_install.state = s;
  g_install.fraction = fraction;
  g_install.error = error;
}

// The system's tar unpacks `archive` into `dir` (Linux: python3 when tar cannot read .xz).
bool extract(const fs::path& archive, const fs::path& dir, std::string& error) {
#ifdef _WIN32
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  const fs::path tar = fs::path(sys) / "tar.exe";
  if (!fs::exists(tar)) {
    error = "this Windows has no tar.exe (Windows 10 1803 or newer is needed)";
    return false;
  }
  std::wstring cmd = L"\"" + tar.wstring() + L"\" -xf \"" + archive.wstring() + L"\" -C \"" + dir.wstring() + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    error = "could not run tar.exe";
    return false;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code) {
    error = "the package could not be unpacked (tar " + std::to_string(code) + ")";
    return false;
  }
  return true;
#else
  auto run = [](const std::vector<std::string>& args) {
    std::vector<char*> argv;  // made before fork: the child only execs
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
      execvp(argv[0], argv.data());
      _exit(127);
    }
    int rc = 0;
    if (pid < 0 || waitpid(pid, &rc, 0) < 0) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
  };
  if (run({"tar", "-xJf", archive.string(), "-C", dir.string()}) == 0) return true;
  // Minimal installs: tar without xz. The installed package's own unpacker (Python's tarfile).
  if (run({"python3", "-I", (exeDir() / "tools" / "compile_module.py").string(), "unpack", archive.string(),
           dir.string()}) == 0)
    return true;
  error = "the package could not be unpacked (tar and python3 failed)";
  return false;
#endif
}

}  // namespace

const char* version() { return BLITZ_VERSION; }

const char* repository() {
  static const std::string repo = [] {
    const char* r = getenv("BLITZ_UPDATE_REPO");
    return std::string(r && *r ? r : "8bitninja64/NFLBlitz2003-PC");
  }();
  return repo.c_str();
}

bool versionNewer(const std::string& latest, const std::string& current) {
  std::vector<long> a = versionParts(latest), b = versionParts(current);
  if (a.empty()) return false;  // not a version tag
  a.resize(std::max(a.size(), b.size()));
  b.resize(a.size());
  return a > b;
}

static Release checkNow() {
  Release r;
  const std::string repo = repository();
  if (!validRepo(repo)) {
    r.state = Release::Failed;
    r.error = "invalid repository name";
    return r;
  }
  std::string body, error;
  const int code = httpGetString(apiBase() + "/repos/" + repo + "/releases/latest", true, body, error);
  if (code == 404) {
    r.state = Release::NoReleases;
    return r;
  }
  if (code != 200) {
    r.state = Release::Failed;
    r.error = code ? "GitHub answered " + std::to_string(code) + (code == 403 || code == 429 ? " (rate limit; try later)" : "")
                   : error;
    return r;
  }
  const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
  if (!j.is_object() || !j.contains("tag_name") || !j["tag_name"].is_string()) {
    r.state = Release::Failed;
    r.error = "unexpected answer from GitHub";
    return r;
  }
  auto str = [](const nlohmann::json& o, const char* k) {
    return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string();
  };
  r.latest = str(j, "tag_name");
  r.page = str(j, "html_url");
  if (r.page.rfind("https://", 0) != 0) r.page = "https://github.com/" + repo + "/releases/latest";
  r.notes = str(j, "body");
  if (j.contains("assets") && j["assets"].is_array())
    for (const auto& a : j["assets"]) {
      if (!a.is_object() || str(a, "name") != kPackage) continue;
      r.asset.name = kPackage;
      r.asset.url = str(a, "browser_download_url");
      r.asset.size = a.contains("size") && a["size"].is_number_unsigned() ? a["size"].get<uint64_t>() : 0;
      const std::string digest = str(a, "digest");  // "sha256:<hex>"
      if (digest.rfind("sha256:", 0) == 0) r.asset.sha256 = digest.substr(7);
    }
  r.state = versionNewer(r.latest, BLITZ_VERSION) ? Release::Available : Release::UpToDate;
  return r;
}

void checkStart() {
  {
    std::lock_guard lk(g_mutex);
    if (g_release.state == Release::Checking) return;
    g_release = {};
    g_release.state = Release::Checking;
  }
  std::thread([] {
    const Release r = checkNow();
    LOG_INFO("update check: state %d, latest %s, package %s %s", (int)r.state, r.latest.c_str(), r.asset.name.c_str(),
             r.error.c_str());
    std::lock_guard lk(g_mutex);
    g_release = r;
  }).detach();  // a slow network never delays the game; the result is dropped at exit
}

Release checkStatus() {
  std::lock_guard lk(g_mutex);
  return g_release;
}

bool canInstall(const Release& release, std::string& why) {
  if (release.state != Release::Available) {
    why = "no newer release";
    return false;
  }
  if (release.asset.name.empty()) {
    why = "the release has no package for this system";
    return false;
  }
  const fs::path dir = exeDir();
  std::error_code ec;
  if (!fs::exists(dir / "tools", ec) || !fs::exists(dir / kExe, ec)) {
    why = "this is not an installed release (a development build)";
    return false;
  }
  const fs::path probe = dir / ".update-probe";
  {
    std::ofstream f(probe);
    if (!f) {
      why = "the program folder is not writable";
      return false;
    }
  }
  fs::remove(probe, ec);
  return true;
}

static Install installNow(const Release& release) {
  auto fail = [](const std::string& e) {
    setInstall(Install::Failed, 0, e);
    LOG_ERROR("update failed: %s", e.c_str());
    return installStatus();
  };
  std::string why, error;
  if (!canInstall(release, why)) return fail(why);
  const Asset& asset = release.asset;
  if (!downloadAllowed(asset.url)) return fail("unexpected download address");
  std::string expect = asset.sha256;
  for (char& c : expect) c = (char)tolower((unsigned char)c);
  if (expect.size() != 64) return fail("the release publishes no SHA-256 checksum for " + asset.name);
  const fs::path dir = exeDir(), work = dir / ".update";
  std::error_code ec;
  fs::remove_all(work / "new", ec);
  fs::create_directories(work / "new", ec);
  const fs::path archive = work / asset.name;

  LOG_INFO("update: downloading %s (%llu bytes)", asset.url.c_str(), (unsigned long long)asset.size);
  setInstall(Install::Downloading, 0);
  std::vector<uint8_t> data;
  data.reserve((size_t)asset.size);
  const int code = httpGet(asset.url, false, [&](const char* d, size_t n) {
    data.insert(data.end(), d, d + n);
    if (asset.size) setInstall(Install::Downloading, std::min(1.0f, (float)data.size() / (float)asset.size));
    return data.size() <= ((size_t)1 << 30);
  }, error);
  if (code != 200) return fail(code ? "the download failed (HTTP " + std::to_string(code) + ")" : error);

  setInstall(Install::Verifying, 1);
  const std::string got = sha256Hex(data);
  if (got != expect)
    return fail("the download is damaged (SHA-256 " + got.substr(0, 12) + "... instead of " + expect.substr(0, 12) + "...)");
  {
    std::ofstream f(archive, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    if (!f) return fail("cannot write " + utf8(archive));
  }
  data.clear();
  data.shrink_to_fit();

  // The package is one folder (NFLBlitz2003/) holding the program.
  setInstall(Install::Extracting, 1);
  if (!extract(archive, work / "new", error)) return fail(error);
  fs::path root;
  // A regular file: on Linux the package's folder has the executable's name.
  if (fs::is_regular_file(work / "new" / kExe, ec)) root = work / "new";
  else
    for (auto& e : fs::directory_iterator(work / "new", ec))
      if (e.is_directory() && fs::is_regular_file(e.path() / kExe, ec)) {
        root = e.path();
        break;
      }
  if (root.empty()) return fail(std::string("the package does not contain ") + kExe);

  // Replace the files; every replaced file is kept as *.old-update until the next start.
  setInstall(Install::Installing, 1);
  std::vector<std::pair<fs::path, fs::path>> moved;  // (destination, its backup or empty)
  auto rollback = [&] {
    for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
      std::error_code e2;
      fs::remove(it->first, e2);
      if (!it->second.empty()) fs::rename(it->second, it->first, e2);
    }
  };
  std::vector<fs::path> files;
  for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it->is_symlink() || it->is_regular_file()) files.push_back(fs::relative(it->path(), root));
  }
  for (const fs::path& rel : files) {
    const fs::path src = root / rel, dest = dir / rel;
    fs::create_directories(dest.parent_path(), ec);
    fs::path backup;
    if (fs::exists(fs::symlink_status(dest, ec))) {
      backup = dest.native() + fs::path(kBackup).native();
      fs::remove(backup, ec);
      fs::rename(dest, backup, ec);
      if (ec) {
        rollback();
        return fail("cannot replace " + utf8(dest) + " (" + ec.message() + ")");
      }
    }
    fs::rename(src, dest, ec);
    if (ec) {
      if (!backup.empty()) {
        std::error_code e2;
        fs::rename(backup, dest, e2);
      }
      rollback();
      return fail("cannot install " + utf8(dest) + " (" + ec.message() + ")");
    }
    moved.emplace_back(dest, backup);
  }
  {
    std::ofstream f(work / "cleanup.txt", std::ios::app);
    for (const auto& [dest, backup] : moved)
      if (!backup.empty()) f << utf8(backup) << "\n";
  }
  fs::remove_all(work / "new", ec);
  fs::remove(archive, ec);
  LOG_INFO("update: installed %s (%zu files)", release.latest.c_str(), moved.size());
  setInstall(Install::Done, 1);
  return installStatus();
}

void installStart(const Release& release) {
  {
    std::lock_guard lk(g_mutex);
    if (g_install.state != Install::Idle && g_install.state != Install::Failed) return;
    g_install = {};
    g_install.state = Install::Downloading;
  }
  std::thread([release] { installNow(release); }).detach();
}

Install installStatus() {
  std::lock_guard lk(g_mutex);
  return g_install;
}

bool restart(int argc, char** argv) {
  const fs::path exe = exeDir() / kExe;
#ifdef _WIN32
  (void)argc;
  (void)argv;
  // The original command line (arguments included) with the new executable.
  std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  int n = 0;
  if (LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &n)) {
    for (int i = 1; i < n; i++) cmd += L" \"" + std::wstring(args[i]) + L"\"";
    LocalFree(args);
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, exeDir().c_str(), &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  std::string path = exe.string();
  std::vector<char*> args;  // made before fork: the child only execs
  args.push_back(const_cast<char*>(path.c_str()));
  for (int i = 1; i < argc; i++) args.push_back(argv[i]);
  args.push_back(nullptr);
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    usleep(500000);  // let this process release the window and audio device first
    execv(path.c_str(), args.data());
    _exit(127);
  }
  return pid > 0;
#endif
}

void cleanup() {
  const fs::path work = exeDir() / ".update", list = work / "cleanup.txt";
  std::error_code ec;
  if (!fs::exists(work, ec)) return;
  std::vector<std::string> left;
  {
    std::ifstream f(list);
    for (std::string line; std::getline(f, line);) {
      if (line.size() < strlen(kBackup) || line.compare(line.size() - strlen(kBackup), std::string::npos, kBackup) != 0)
        continue;  // only the backups this program made
      const fs::path p = fs::u8path(line);
      fs::remove(p, ec);
      std::error_code e2;
      if (ec && fs::exists(p, e2)) left.push_back(line);  // still in use (the previous process is exiting)
    }
  }
  if (left.empty()) {
    fs::remove_all(work, ec);
    return;
  }
  std::ofstream f(list, std::ios::trunc);
  for (const auto& l : left) f << l << "\n";
}

void openPage(const std::string& url) {
  if (url.rfind("https://", 0) != 0 || !validUrl(url)) return;
#ifdef _WIN32
  ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
  pid_t pid = fork();
  if (pid == 0) {
    execlp("xdg-open", "xdg-open", url.c_str(), (char*)nullptr);
    _exit(127);
  }
#endif
}

}  // namespace update
