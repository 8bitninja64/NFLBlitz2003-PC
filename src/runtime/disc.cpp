#include "disc.h"

#include "log.h"

#include <nod.h>

#include <cstring>

Disc g_disc;

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

bool Disc::open(const std::string& path) {
  if (nod_disc_open(path.c_str(), nullptr, &m_nod) != NOD_RESULT_OK) {
    LOG_ERROR("nod: %s", nod_error_message());
    return false;
  }
  uint8_t hdr[0x440];
  if (!read(0, hdr, sizeof(hdr))) return false;
  m_gameId.assign(reinterpret_cast<char*>(hdr), 6);
  m_dolOff = be32(hdr + 0x420);
  m_fstOff = be32(hdr + 0x424);
  m_fstSize = be32(hdr + 0x428);
  std::vector<uint8_t> fst(m_fstSize);
  if (!read(m_fstOff, fst.data(), m_fstSize)) return false;
  uint32_t count = be32(fst.data() + 8);
  const char* names = reinterpret_cast<const char*>(fst.data() + count * 12);
  // Walk the FST; directories record their end index.
  std::vector<std::pair<uint32_t, std::string>> dirs;  // (end index, prefix)
  std::string prefix;
  for (uint32_t i = 1; i < count; i++) {
    while (!dirs.empty() && i >= dirs.back().first) {
      dirs.pop_back();
      prefix = dirs.empty() ? "" : dirs.back().second;
    }
    const uint8_t* e = fst.data() + i * 12;
    std::string name = names + (be32(e) & 0xFFFFFF);
    if (e[0]) {
      prefix += name + "/";
      dirs.emplace_back(be32(e + 8), prefix);
    } else {
      m_files.push_back({prefix + name, be32(e + 4), be32(e + 8)});
    }
  }
  LOG_INFO("disc %s: %zu files", m_gameId.c_str(), m_files.size());
  return true;
}

bool Disc::read(uint64_t offset, void* dst, uint32_t size) {
  std::lock_guard lk(m_mutex);
  if (nod_seek(m_nod, (int64_t)offset, 0) != (int64_t)offset) return false;
  auto* out = static_cast<uint8_t*>(dst);
  while (size) {
    int64_t n = nod_read(m_nod, out, size);
    if (n <= 0) return false;
    out += n;
    size -= (uint32_t)n;
  }
  return true;
}
