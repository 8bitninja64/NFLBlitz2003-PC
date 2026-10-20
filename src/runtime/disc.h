// GameCube disc image access through nod: ISO/GCM, RVZ, WIA, CISO, GCZ, NFS, ...
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct DiscFile {
  std::string path;  // lowercase-insensitive lookups use the original names
  uint32_t offset;
  uint32_t size;
};

class Disc {
public:
  bool open(const std::string& path);
  bool read(uint64_t offset, void* dst, uint32_t size);
  const std::string& gameId() const { return m_gameId; }
  uint32_t dolOffset() const { return m_dolOff; }
  uint32_t fstOffset() const { return m_fstOff; }
  uint32_t fstSize() const { return m_fstSize; }
  const std::vector<DiscFile>& files() const { return m_files; }

private:
  struct NodHandle* m_nod = nullptr;
  std::mutex m_mutex;
  std::string m_gameId;
  uint32_t m_dolOff = 0, m_fstOff = 0, m_fstSize = 0;
  std::vector<DiscFile> m_files;
};

extern Disc g_disc;
