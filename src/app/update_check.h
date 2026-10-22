// Updates from the project's GitHub releases (the F1 menu's Update button), after the NBA Hangtime
// port's updater. Check: one anonymous HTTPS GET to api.github.com for the latest release; its tag is
// compared with this build's version (BLITZ_VERSION). Install (only when the player chooses it):
// downloads this system's package (NFLBlitz2003-windows-x64.zip / NFLBlitz2003-linux-x64.tar.xz) from
// that release, checks its SHA-256 against the digest GitHub publishes (no checksum, no install),
// unpacks it with the system's tar and replaces the program files in place. Settings, saves and the
// compiled game module live in the user directory and are not touched; the module stamp
// (boot_args.cpp) rebuilds the game code when the new version's build inputs changed. Replaced files
// are renamed to *.old-update first (a running program's files can be renamed on both systems),
// restored if anything fails and removed at the next start. Then the program restarts.
#pragma once

#include <cstdint>
#include <string>

namespace update {

struct Asset {
  std::string name, url, sha256;
  uint64_t size = 0;
};

struct Release {
  enum State { Idle, Checking, UpToDate, Available, NoReleases, Failed } state = Idle;
  std::string latest;  // the release's tag, e.g. "v1.1"
  std::string error;   // Failed: what went wrong, for the player
  std::string page;    // the release's web page
  std::string notes;   // the release notes (Markdown as written)
  Asset asset;         // this system's package (name empty: the release has none)
};

struct Install {
  enum State { Idle, Downloading, Verifying, Extracting, Installing, Done, Failed } state = Idle;
  float fraction = 0;  // Downloading: 0..1
  std::string error;
};

const char* version();                    // this build's version
const char* repository();                 // "owner/name" (BLITZ_UPDATE_REPO overrides, for tests)
void checkStart();                        // background check (ignored while one runs)
Release checkStatus();
bool versionNewer(const std::string& latest, const std::string& current);
// Whether `release` can be installed in place here; `why` says why not (the menu then offers the
// release page).
bool canInstall(const Release& release, std::string& why);
void installStart(const Release& release);  // background install
Install installStatus();
// After a finished install: starts the new executable with this process's arguments (the caller
// then quits).
bool restart(int argc, char** argv);
// At startup: removes the files the last install replaced.
void cleanup();
// Opens a web page in the system's browser.
void openPage(const std::string& url);

}  // namespace update
