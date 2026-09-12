"""Zip the staged dist folder into the release archive.

    py -3 package.py

Run build.bat first. One archive comes out of dist:

  GlintSpotter-<version>.zip   the plugin, the ini, the README and the licence,
                               which is everything anyone needs to install it
                               by hand or through a mod manager.

The ini is regenerated here from the plugin's own WriteDefaults rather than
copied from whatever is lying in dist. The plugin writes that file itself on
first run, so a shipped ini that disagrees with it is a shipped lie, and the
two drifted twice before this script existed.

It prints the SHA-256 of the archive and of the plugin when it is done, in the
order the release notes list them.

Master Looter signs its plugin before packaging and refuses to build an
unsigned archive. This mod does not sign, so there is no such check here. If
signing is ever added, mod/scripts/sign.ps1 in that project is the pattern.
"""
import hashlib
import io
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
ROOT = os.path.dirname(MOD)
DIST = os.path.join(MOD, "dist")
FULL = ["GlintSpotter.asi", "GlintSpotter.ini", "README.md", "LICENSE"]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def version():
    header = os.path.join(MOD, "src", "version.h")
    text = io.open(header, encoding="utf-8").read()
    m = re.search(r'#define\s+GS_VERSION_STRING\s+"([^"]+)"', text)
    if not m:
        sys.exit("no GS_VERSION_STRING in %s" % header)
    return m.group(1)


def write_ini():
    """Rebuild dist/GlintSpotter.ini out of the fputs calls in WriteDefaults.

    Reading the source rather than running the plugin keeps this to one file
    and no game. The pattern only has to survive the shape those calls are
    written in, which has not changed in seventy builds."""
    src = io.open(os.path.join(MOD, "src", "core", "settings.cpp"), encoding="utf-8").read()
    start = src.index("void WriteDefaults")
    body = src[start:src.index("fclose(f);", start)]
    parts = [m.group(1).encode().decode("unicode_escape")
             for m in re.finditer(r'fputs\("((?:[^"\\]|\\.)*)"\s*,\s*f\)', body)]
    if not parts:
        sys.exit("no fputs lines found in WriteDefaults; the pattern needs updating")
    out = os.path.join(DIST, "GlintSpotter.ini")
    io.open(out, "w", encoding="utf-8", newline="\r\n").write("".join(parts))
    print("wrote %s (%d lines from WriteDefaults)" % (out, len(parts)))


def write_zip(name, files):
    out = os.path.join(DIST, name)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            path = os.path.join(DIST, f)
            if not os.path.exists(path):
                path = os.path.join(ROOT, f)      # README and LICENSE live at the top
            if not os.path.exists(path):
                sys.exit("missing %s; run build.bat first" % f)
            z.write(path, f)
    print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))
    return out


def main():
    v = version()
    write_ini()
    archive = write_zip("GlintSpotter-%s.zip" % v, FULL)
    print("\nSHA-256 for %s:" % v)
    for path in (archive, os.path.join(DIST, "GlintSpotter.asi")):
        print("%s  %s" % (sha256(path), os.path.basename(path)))


if __name__ == "__main__":
    main()
