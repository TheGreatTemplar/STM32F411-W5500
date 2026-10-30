"""Independently verify build identity and the actual embedded HTML bytes."""
from pathlib import Path
import hashlib
import json
import re
import zlib
import gzip

root = Path(__file__).resolve().parents[1]
macros = dict(re.findall(r'#define (FW_\w+) "([^"]*)"',
                         (root / 'src/build_info.h').read_text(encoding='utf-8')))
paths = []
for directory in ['src', 'third_party', 'firmware', 'scripts']:
    paths.extend(path for path in (root / directory).rglob('*') if path.is_file()
                 and path.name not in ['build_info.h', 'web_page.h'] and path.suffix != '.pyc')
paths.append(root / 'adc_web_f411.uvprojx')
manifest = ''.join(path.relative_to(root).as_posix() + ':' +
                   hashlib.sha256(path.read_bytes()).hexdigest() + '\n'
                   for path in sorted(paths, key=str))
assert hashlib.sha256(manifest.encode()).hexdigest() == macros['FW_SOURCE_SHA256']
html = (root / 'src/web/index.html').read_bytes()
assert zlib.crc32(html) == int(macros['FW_WEB_CRC32'], 16)
embedded = bytes(int(value, 16) for value in re.findall(
    r'0x([0-9A-F]{2})', (root / 'src/web_page.h').read_text(encoding='utf-8')))
assert embedded[-1:] == b'\0'
archive = embedded[:-1]
assert archive[:3] == b'\x1f\x8b\x08'
assert archive[4:8] == bytes(4)  # Reproducible build: no generation timestamp.
assert gzip.decompress(archive) == html
assert int.from_bytes(archive[-8:-4], 'little') == int(macros['FW_WEB_CRC32'], 16)
assert int.from_bytes(archive[-4:], 'little') == len(html)
version = json.loads((root / 'firmware/version.json').read_text(encoding='utf-8'))
assert macros['FW_VERSION'] == version['version']
assert macros['FW_BASE_COMMIT'] == version['base_commit']
print('Build identity: source SHA-256, HTML CRC32, embedded page and version verified')
