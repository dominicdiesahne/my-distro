#!/usr/bin/env python3
"""make-packages.py: the build machine's half of my-pkg (run by `make packages`).

The extra programs (desktop/packages.fragment) are built in the same
Buildroot tree as the image, so they use exactly the image's libraries. This
script then cuts them apart again:

  check-config OUT WORK FRAGMENT
      Compares the build config with the image's (OUT/.config.last). A program
      may bring new packages, but must not change an option of one that is in
      the image: that package would not be rebuilt, and installed systems have
      the image's version. Options of build tools (host-...) may change: their
      names are printed, so the Makefile can build them again. Also writes the
      list of extra packages (OUT/.extra-packages) for "clean".

  cut OUT WORK BUILDROOT UPDATE CATALOG DEST
      One .tar.xz per extra package that has files, with only the files that
      are not in the image (UPDATE = the image's root filesystem as a tar),
      plus the package list my-pkg-index.json. Dependencies on other extra
      packages become the list's dependencies. Buildroot says which package
      installed which file (build/packages-file-list.txt) and what each
      package needs (make show-info, in WORK/info.json).

  clean OUT [UPDATE]
      Takes the extra packages out of the build tree again (target and staging
      files, install stamps), so the next `make desktop` builds the image
      without them and the next `make packages` installs them anew. Prints the
      image packages whose files an extra package had overwritten (the
      Makefile installs those again). UPDATE (the image) helps to tell which
      files are the image's when a package list misses them.
"""
import configparser
import glob
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time

ALLOWED = {"BR2_PACKAGE_BUSYBOX_SHOW_OTHERS"}   # only shows packages in the menu
LIBRARIES = "Libraries"


def die(msg):
    sys.exit("make-packages: " + msg)


def symbol(name):
    """Buildroot package name -> its config symbol (sdl_mixer -> BR2_PACKAGE_SDL_MIXER)"""
    return "BR2_PACKAGE_" + name.upper().replace("-", "_")


def read_config(path):
    """symbol -> value; "n" for '# X is not set'"""
    conf = {}
    with open(path) as f:
        for line in f:
            m = re.match(r"# (BR2_\w+) is not set", line)
            if m:
                conf[m.group(1)] = "n"
            elif line.startswith("BR2_") and "=" in line:
                k, v = line.rstrip("\n").split("=", 1)
                conf[k] = v
    return conf


def load_info(path):
    with open(path) as f:
        return json.load(f)


def extra_packages(work):
    """The target packages the package config builds and the image's doesn't."""
    base, info = load_info(os.path.join(work, "base-info.json")), \
        load_info(os.path.join(work, "info.json"))
    return {n: i for n, i in info.items() if i.get("type") == "target" and n not in base}


# --- check-config ---------------------------------------------------------------------

def check_config(out, work, fragment):
    base = read_config(os.path.join(out, ".config.last"))
    pkg = read_config(os.path.join(out, ".config"))
    last_path = os.path.join(out, ".config.packages-last")
    last = read_config(last_path) if os.path.exists(last_path) else base
    extras = extra_packages(work)
    info = load_info(os.path.join(work, "info.json"))
    new_symbols = {symbol(n) for n in extras}
    errors, host_changed = [], set()

    # What packages.fragment asks for must really be on
    with open(fragment) as f:
        for line in f:
            if re.match(r"BR2_\w+=y\s*$", line):
                k = line.split("=")[0]
                if pkg.get(k) != "y":
                    errors.append("%s can't be turned on (a dependency is missing; see "
                                  "make menuconfig)" % k)

    for k, v in pkg.items():
        b = base.get(k)
        if b is None or b == v or k in new_symbols or k in ALLOWED:
            continue
        if k.startswith("BR2_PACKAGE_HOST_"):
            if last.get(k, b) != v:
                host_changed.add(k)
            continue
        errors.append("%s: %s in the image, %s for the packages" % (k, b, v))
    for k, b in base.items():
        if b != "n" and k not in pkg:
            errors.append("%s (on in the image) is gone in the package config" % k)
    if errors:
        die("the packages change the image's configuration:\n  " + "\n  ".join(errors) +
            "\nPut such options into desktop/buildroot.fragment (the image) instead.")

    # Which build tools to build again: the host package with the longest
    # name the changed symbol starts with
    hosts = sorted((n for n, i in info.items() if i.get("type") == "host"),
                   key=len, reverse=True)
    rebuild = set()
    for k in host_changed:
        for n in hosts:
            if k == symbol(n) or k.startswith(symbol(n) + "_"):
                rebuild.add(n)
                break
    with open(os.path.join(out, ".extra-packages"), "w") as f:
        for n, i in sorted(extras.items()):
            f.write("%s %s\n" % (n, i.get("stamp_dir") or "-"))
    print(" ".join(sorted(rebuild)))


# --- cut --------------------------------------------------------------------------------

def file_lists(out, which=""):
    """package -> [paths] (relative, no ./) from the per-package lists.

    Buildroot writes a package's list when it installs it, as the difference
    to a snapshot taken when it was configured. "clean" makes the extra
    packages install again without configuring again: then there is no
    snapshot and the new list is empty. So "clean" keeps each list as
    .files-list*.my-pkg, and an empty list means: the kept one."""
    lists = {}
    for path in glob.glob(os.path.join(out, "build", "*", ".files-list%s.txt" % which)):
        kept = path[:-4] + ".my-pkg"
        if os.path.getsize(path) == 0 and os.path.exists(kept):
            path = kept
        with open(path, errors="replace") as f:
            for line in f:
                pkg, _, p = line.rstrip("\n").partition(",")
                if p.startswith("./"):
                    p = p[2:]
                if p:
                    lists.setdefault(pkg, []).append(p)
    return lists


def base_listing(update):
    """Files and folders in the image (the update package)"""
    out = subprocess.run(["tar", "-tJf", update], stdout=subprocess.PIPE, check=True,
                         text=True, errors="replace").stdout
    files, dirs = set(), set()
    for line in out.splitlines():
        p = line[2:] if line.startswith("./") else line
        if p.endswith("/"):
            dirs.add(p.rstrip("/"))
        elif p:
            files.add(p)
    return files, dirs


def help_text(buildroot, name):
    """The first paragraph of the help in Buildroot's Config.in"""
    path = os.path.join(buildroot, "package", name, "Config.in")
    try:
        with open(path) as f:
            lines = f.read().split("\n")
    except OSError:
        return ""
    sym = symbol(name)
    for i, line in enumerate(lines):
        if line.strip() in ("config " + sym, "menuconfig " + sym):
            for j in range(i + 1, len(lines)):
                if lines[j].strip() in ("help", "---help---"):
                    words = []
                    for t in lines[j + 1:]:
                        t = t.strip()
                        if not t or t.startswith("http"):
                            break
                        words.append(t)
                    return " ".join(words)
                if lines[j].startswith("config ") or lines[j].startswith("menuconfig "):
                    break
    return ""


def desktop_entry(name, entry):
    """A .desktop file, so the file manager can open files with the program"""
    run = entry.get("run")
    return ("[Desktop Entry]\nType=Application\nName=%s\nComment=%s\nExec=%s\n"
            "Icon=%s\nTerminal=%s\nCategories=%s;\n%s" % (
                entry.get("title", name), entry.get("title", name), run,
                entry.get("icon", "application-x-executable"),
                "true" if entry.get("terminal") == "yes" else "false",
                entry.get("category", "Utility"),
                "MimeType=%s;\n" % entry["mime"].rstrip(";") if entry.get("mime") else ""))


def menu_command(entry):
    """The Start menu's command: in a terminal, or asking for the file first"""
    run, title = entry["run"], entry.get("title", "")
    if "%f" in run:
        prog = run.replace("%f", "").strip()
        mime = " ".join("--mime %s" % m for m in entry.get("mime", "").split(";") if m)
        return "my-pick-file --title '%s' %s %s" % (title.replace("'", ""), mime, prog)
    if entry.get("terminal") == "yes":
        hold = "-hold " if entry.get("hold") == "yes" else ""
        return "xterm %s-T '%s' -e %s" % (hold, title.replace("'", ""), run)
    return run


def safe(v):
    return re.sub(r"[^A-Za-z0-9._-]", "_", v)[:40]


def tarinfo(name, st, kind, link=""):
    ti = tarfile.TarInfo(name)
    ti.type = kind
    ti.mode = st.st_mode & 0o7777
    ti.mtime = int(st.st_mtime)
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = "root"
    ti.linkname = link
    return ti


def cut(out, work, buildroot, update, catalog_path, dest):
    target = os.path.join(out, "target")
    with open(os.path.join(target, "etc", "my-distro", "version")) as f:
        system = f.read().strip()
    extras = extra_packages(work)
    catalog = configparser.ConfigParser(interpolation=None)
    catalog.read(catalog_path)
    for n in catalog.sections():
        if n not in extras:
            die("%s is in the catalog but not built (not in packages.fragment?)" % n)

    print("Reading the image's file list...", flush=True)
    base_files, base_dirs = base_listing(update)
    lists = file_lists(out)

    # Each extra package's files: those not in the image. Finalizing the
    # target removed some (headers, manuals) and turned .py into .pyc.
    owned, files_of = {}, {}
    for n in sorted(extras):
        mine = []
        for p in lists.get(n, []):
            if not os.path.lexists(os.path.join(target, p)):
                if p.endswith(".py") and os.path.exists(os.path.join(target, p + "c")):
                    p += "c"
                else:
                    continue
            if p in base_files:
                print("  note: %s overwrote %s of the image; not packed" % (n, p))
                continue
            if p in owned:
                print("  note: %s is in %s and %s; packed with %s" % (p, owned[p], n, owned[p]))
                continue
            owned[p] = n
            mine.append(p)
        files_of[n] = sorted(mine)

    # Dependencies: the extra packages with files that Buildroot built first
    # (through packages without files, e.g. virtual ones), plus "needs"
    def deps_of(n, seen):
        found = set()
        for d in extras[n].get("dependencies", []):
            if d not in extras or d in seen:
                continue
            if files_of.get(d):
                found.add(d)
            else:
                found |= deps_of(d, seen | {d})
        return found

    os.makedirs(dest, exist_ok=True)
    for old in glob.glob(os.path.join(dest, "pkg-*.tar.xz")):
        os.remove(old)
    packages = []
    for n in sorted(extras):
        entry = dict(catalog[n]) if catalog.has_section(n) else {}
        files = list(files_of[n])
        extra_files = {}
        if entry.get("run"):
            extra_files["usr/share/applications/my-pkg-%s.desktop" % n] = \
                desktop_entry(n, entry).encode()
        # A package without files of its own (a virtual one like nodejs, whose
        # files come from nodejs-src) is only listed when the catalog names
        # it: then it is an empty package that brings its dependencies.
        if not files and not extra_files and not entry:
            continue
        if not files and entry and not extras[n].get("virtual"):
            die("%s has no files in %s (not installed there?)" % (n, target))
        deps = deps_of(n, {n}) | set(entry.get("needs", "").split())
        for d in deps:
            if d not in extras:
                die("%s needs %s, which is not an extra package" % (n, d))

        # Folders the image doesn't have
        dirs = set()
        for p in files + list(extra_files):
            d = os.path.dirname(p)
            while d and d not in base_dirs:
                dirs.add(d)
                d = os.path.dirname(d)

        # a virtual package has no version: its provider's
        version = extras[n].get("version") or \
            "-".join(sorted(extras[d].get("version") or "" for d in deps if d in extras)) or "1"
        fname = "pkg-%s-%s.tar.xz" % (n, safe(version))
        path = os.path.join(dest, fname)
        size = 0
        with tarfile.open(path, "w:xz") as tar:
            for d in sorted(dirs):
                st = os.stat(os.path.join(target, d)) if os.path.isdir(os.path.join(target, d)) \
                    else os.stat(target)
                tar.addfile(tarinfo(d, st, tarfile.DIRTYPE))
            for p in files:
                full = os.path.join(target, p)
                st = os.lstat(full)
                if os.path.islink(full):
                    tar.addfile(tarinfo(p, st, tarfile.SYMTYPE, os.readlink(full)))
                elif os.path.isfile(full):
                    ti = tarinfo(p, st, tarfile.REGTYPE)
                    ti.size = st.st_size
                    size += st.st_size
                    with open(full, "rb") as f:
                        tar.addfile(ti, f)
            for p, data in extra_files.items():
                st = os.stat(target)
                ti = tarinfo(p, st, tarfile.REGTYPE)
                ti.mode, ti.size, ti.mtime = 0o644, len(data), int(time.time())
                tar.addfile(ti, io.BytesIO(data))
                size += len(data)
        with open(path, "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        menu = []
        if entry.get("run"):
            menu.append([entry.get("title", n), menu_command(entry),
                         entry.get("icon", "application-x-executable")])
        packages.append({
            "name": n,
            "version": version,
            "title": entry.get("title", n),
            "category": entry.get("category", LIBRARIES),
            "description": " ".join((entry.get("description") or help_text(buildroot, n)
                                     or n).split()),
            "size": os.path.getsize(path),
            "installed_size": size,
            "dependencies": sorted(deps),
            "file": fname,
            "sha256": sha,
            "menu": menu,
        })
        print("  %-20s %-16s %6d files %8.1f MB  %s" % (
            n, version[:16], len(files), os.path.getsize(path) / 1e6,
            ", ".join(sorted(deps))), flush=True)

    index = {"format": 1, "system": system,
             "made": time.strftime("%Y-%m-%d %H:%M"), "packages": packages}
    with open(os.path.join(dest, "my-pkg-index.json"), "w") as f:
        json.dump(index, f, indent=1)
        f.write("\n")
    for stale in ("my-pkg-index.json.sig",):
        if os.path.exists(os.path.join(dest, stale)):
            os.remove(os.path.join(dest, stale))
    print("%d packages for my-distro %s in %s" % (len(packages), system, dest))


# --- clean ------------------------------------------------------------------------------

def clean(out, update=None):
    path = os.path.join(out, ".extra-packages")
    if not os.path.exists(path):
        return
    with open(path) as f:
        extras = dict((line.split() + [""])[:2] for line in f if line.strip())
    # Keep every package's file list (see file_lists): the extra packages and
    # the image packages installed again below come back with empty ones
    for lst in glob.glob(os.path.join(out, "build", "*", ".files-list*.txt")):
        if os.path.getsize(lst) > 0:
            shutil.copyfile(lst, lst[:-4] + ".my-pkg")
    image = base_listing(update)[0] if update and os.path.exists(update) else set()
    reinstall = set()
    for which, folder in (("", "target"), ("-staging", "staging")):
        lists = file_lists(out, which)
        base = {}
        for n, paths in lists.items():
            if n not in extras:
                for p in paths:
                    base[p] = n
        root = os.path.realpath(os.path.join(out, folder))
        for n in extras:
            for p in lists.get(n, []):
                full = os.path.join(root, p)
                owner = base.get(p)
                if owner is None and folder == "target" and p in image:
                    # In the image, but no image package lists it: BusyBox
                    # installs its commands only where no file is yet (vi,
                    # xxd), so when vim came first, its list misses them.
                    owner = "busybox"
                if owner:
                    # an image file the package overwrote: put the original back
                    if folder == "target" and os.path.lexists(full):
                        os.remove(full)
                        reinstall.add(owner)
                    continue
                for f in (full, full + "c"):
                    if os.path.lexists(f) and not os.path.isdir(f):
                        os.remove(f)
                # The folders that are empty now go too (else the image gets
                # empty /usr/share/vim & co), up to the top-level ones
                d = os.path.dirname(p)
                while d.count("/") >= 1:
                    try:
                        os.rmdir(os.path.join(root, d))
                    except OSError:
                        break                    # not empty (or gone already)
                    d = os.path.dirname(d)
    for n, stamp_dir in extras.items():
        if stamp_dir == "-":
            continue
        d = os.path.join(out, stamp_dir)        # relative to OUT, or absolute
        for stamp in (".stamp_installed", ".stamp_target_installed",
                      ".stamp_staging_installed", ".stamp_images_installed"):
            if os.path.exists(os.path.join(d, stamp)):
                os.remove(os.path.join(d, stamp))
    os.remove(path)
    print(" ".join(sorted(reinstall)))


def main():
    a = sys.argv[1:]
    if a[:1] == ["check-config"] and len(a) == 4:
        check_config(*a[1:])
    elif a[:1] == ["cut"] and len(a) == 7:
        cut(*a[1:])
    elif a[:1] == ["clean"] and len(a) in (2, 3):
        clean(*a[1:])
    else:
        die("usage: check-config OUT WORK FRAGMENT | cut OUT WORK BUILDROOT UPDATE "
            "CATALOG DEST | clean OUT [UPDATE]")


if __name__ == "__main__":
    main()
