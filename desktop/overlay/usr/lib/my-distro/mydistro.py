"""mydistro: what my-update (system updates) and my-pkg (programs from the
Software app) share: settings files, versions, downloads and the signature
check.

Both trust only files signed by the maintainer of my-distro. The signature
is Ed25519, made with a private key that only the maintainer has, and checked
here with the public key /etc/my-distro/update-key.pem (openssl pkeyutl).
"""
import hashlib
import os
import subprocess
import sys
import urllib.request

VERSION_FILE = "/etc/my-distro/version"
PUBLIC_KEY = "/etc/my-distro/update-key.pem"


def say(*args):
    print(*args, flush=True)


def read_conf(path, defaults):
    """key = value lines (# starts a comment), on top of the defaults."""
    conf = dict(defaults)
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    key, value = line.split("=", 1)
                    conf[key.strip()] = value.strip()
    except OSError:
        pass
    return conf


def installed_version():
    try:
        with open(VERSION_FILE) as f:
            return f.read().strip()
    except OSError:
        return "0"


def version_tuple(v):
    """'v0.7.1' -> (0, 7, 1), to compare versions."""
    parts = []
    for p in v.strip().lstrip("vV").split("."):
        digits = "".join(c for c in p if c.isdigit())
        parts.append(int(digits) if digits else 0)
    return tuple(parts)


def is_live():
    """The live system (ISO) runs from RAM: nothing survives a restart."""
    try:
        with open("/proc/cmdline") as f:
            return "rdinit=" in f.read()
    except OSError:
        return False


def need_root(what, instead):
    """True when we are root; else says what to do instead."""
    if os.geteuid() == 0:
        return True
    say("%s needs root: run it as root, or use %s." % (what, instead))
    return False


def fetch(url, timeout=30, accept="application/vnd.github+json"):
    """A small download, into memory."""
    req = urllib.request.Request(url, headers={
        "User-Agent": "my-distro-updater",
        "Accept": accept,
    })
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def download(url, path, quiet=False):
    """A download into a file (path.part first, so a broken one never has the
    real name). Big files show their progress in steps of 10 %."""
    req = urllib.request.Request(url, headers={"User-Agent": "my-distro-updater"})
    with urllib.request.urlopen(req, timeout=60) as r, open(path + ".part", "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done, shown = 0, -10
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            if total > 1 << 20 and not quiet:   # progress only for big files
                pct = done * 100 // total
                if pct >= shown + 10:
                    say("  %3d %%  (%d of %d MB)" % (pct, done >> 20, total >> 20))
                    shown = pct
    os.replace(path + ".part", path)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def signature_ok(path, sigfile):
    """Was the file signed with the maintainer's key, and is it unchanged?"""
    r = subprocess.run(["openssl", "pkeyutl", "-verify", "-pubin", "-inkey", PUBLIC_KEY,
                        "-rawin", "-in", path, "-sigfile", sigfile],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return r.returncode == 0


def verify(package, sumfile, sigfile):
    """The signature of the sum file, then the package's sum. Raises if wrong."""
    if not signature_ok(sumfile, sigfile):
        raise RuntimeError("the signature is not valid: this update was not made by "
                           "the maintainer of my-distro, or it was changed")
    with open(sumfile) as f:
        expected = f.read().split()[0].lower()
    if sha256_of(package) != expected:
        raise RuntimeError("the download is damaged (SHA-256 sum does not match)")


if __name__ == "__main__":
    sys.exit("mydistro is a module for my-update and my-pkg, not a program.")
