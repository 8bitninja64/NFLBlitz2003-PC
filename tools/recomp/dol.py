"""DOL executable loader."""
import struct


class Dol:
    def __init__(self, path):
        d = open(path, 'rb').read()
        offs = struct.unpack('>18I', d[0:0x48])
        addrs = struct.unpack('>18I', d[0x48:0x90])
        sizes = struct.unpack('>18I', d[0x90:0xD8])
        self.bss_addr, self.bss_size, self.entry = struct.unpack('>3I', d[0xD8:0xE4])
        self.sections = []  # (name, addr, bytes, is_text)
        for i in range(18):
            if sizes[i]:
                text = i < 7
                name = f'.text{i}' if text else f'.data{i - 7}'
                self.sections.append((name, addrs[i], d[offs[i]:offs[i] + sizes[i]], text))

    def text_sections(self):
        return [s for s in self.sections if s[3]]

    def data_sections(self):
        return [s for s in self.sections if not s[3]]

    def section_of(self, addr):
        for s in self.sections:
            if s[1] <= addr < s[1] + len(s[2]):
                return s
        return None

    def is_text(self, addr):
        s = self.section_of(addr)
        return s is not None and s[3]

    def read32(self, addr):
        s = self.section_of(addr)
        if s is None:
            return None
        o = addr - s[1]
        if o + 4 > len(s[2]):
            return None
        return struct.unpack_from('>I', s[2], o)[0]
