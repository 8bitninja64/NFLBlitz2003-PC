"""Helpers for tools/make_module.sh that need nothing but Python (minimal installs such as
openSUSE's lack xargs; tar may lack xz support):

  compile_module.py unpack <archive.tar.xz> <dir>    unpack the bundled compiler
  compile_module.py compile <source.cpp>...          compile in parallel into $OBJ

compile reads the compiler command from $CXX and its flags from $CXXFLAGS (shell words), the include
directories from $RT and $GEN, and writes <name>.o into $OBJ. It prints "# Compiling N files (J at a
time)" and one "compiled <file>" line per finished file (the first-start window's progress), and on
the first failure that file's compiler output, then exits 1.
"""
import os
import shlex
import subprocess
import sys
import tarfile
import threading
from concurrent.futures import ThreadPoolExecutor


def unpack(archive, dest):
    os.makedirs(dest, exist_ok=True)
    with tarfile.open(archive, 'r:xz') as t:
        if hasattr(tarfile, 'data_filter'):
            t.extractall(dest, filter='tar')  # keeps the symlinks and modes the release has
        else:
            t.extractall(dest)


def compile_all(sources):
    cxx = shlex.split(os.environ['CXX'])
    flags = shlex.split(os.environ.get('CXXFLAGS', ''))
    inc = ['-I' + os.environ['RT'], '-I' + os.environ['GEN']]
    obj = os.environ['OBJ']
    jobs = max(1, os.cpu_count() or 4)
    print(f'# Compiling {len(sources)} files ({jobs} at a time)', flush=True)
    failed = threading.Event()
    lock = threading.Lock()

    def one(src):
        if failed.is_set():
            return
        out = os.path.join(obj, os.path.splitext(os.path.basename(src))[0] + '.o')
        r = subprocess.run(cxx + flags + inc + ['-c', src, '-o', out],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with lock:
            if r.returncode:
                if not failed.is_set():
                    sys.stdout.write(r.stdout.decode(errors='replace'))
                    print(f'compiling {os.path.basename(src)} failed (exit {r.returncode})', flush=True)
                failed.set()
            else:
                print(f'compiled {os.path.basename(src)}', flush=True)

    with ThreadPoolExecutor(jobs) as pool:
        list(pool.map(one, sources))
    return 1 if failed.is_set() else 0


def main():
    if len(sys.argv) == 4 and sys.argv[1] == 'unpack':
        unpack(sys.argv[2], sys.argv[3])
        return 0
    if len(sys.argv) >= 3 and sys.argv[1] == 'compile':
        return compile_all(sys.argv[2:])
    print(__doc__, file=sys.stderr)
    return 2


sys.exit(main())
