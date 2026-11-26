#!/usr/bin/env python3
"""
The Haskell package 'ugly' (libugly_hs.so and its interfaces) in a package
database of its own, so that module libraries can be built against it:
ghc -package-db <db> -package ugly.

  package.py build GHC_PKG DB CONF VERSION IMPORT_DIR LIBRARY_DIRS
      In the build tree: DB is made again from nothing; CONF is the package
      description written (the build's output).

  package.py install GHC_PKG BUILD_IMPORT_DIR HASKELL_DIR LIBRARY_DIRS VERSION
      At install time (meson.add_install_script, honouring DESTDIR): the
      interfaces are copied to HASKELL_DIR/import, and HASKELL_DIR/package.db
      describes the installed package.

LIBRARY_DIRS (colon-separated) have libugly_hs.so and libugly.so: a Haskell
program built against the package (the binding) links both.

The packages 'ugly' depends on are found by name, with GHC_PKG, in GHC's
global database.
"""
import os
import shutil
import subprocess
import sys

DEPENDS = ['base', 'bytestring', 'containers', 'deepseq', 'template-haskell', 'text']
EXPOSED = ['Ugly', 'Ugly.Export', 'Ugly.Types', 'Ugly.Value']
HIDDEN = ['Ugly.Marshal', 'Ugly.Native', 'Ugly.Plugin']


def description(ghc_pkg, version, import_dir, library_dirs):
    ids = [subprocess.run([ghc_pkg, 'field', p, 'id', '--simple-output'], check=True,
                          capture_output=True, text=True).stdout.split()[0] for p in DEPENDS]
    # libugly_hs.so is linked by its own name (extra-libraries), not as
    # libHSugly-ghc<version>.so (hs-libraries): the plugin links it too. A
    # program using the binding calls the core too: libugly.so.
    return '\n'.join([
        'name: ugly',
        'version: ' + version,
        'id: ugly',
        'key: ugly',
        'exposed: True',
        'exposed-modules: ' + ' '.join(EXPOSED),
        'hidden-modules: ' + ' '.join(HIDDEN),
        'import-dirs: ' + import_dir,
        'library-dirs: ' + ' '.join(library_dirs.split(':')),
        'dynamic-library-dirs: ' + ' '.join(library_dirs.split(':')),
        'extra-libraries: ugly_hs ugly',
        'depends: ' + ' '.join(ids),
        ''])


def build(ghc_pkg, db, conf, version, import_dir, library_dirs):
    with open(conf, 'w') as f:
        f.write(description(ghc_pkg, version, import_dir, library_dirs))
    shutil.rmtree(db, ignore_errors=True)
    subprocess.run([ghc_pkg, 'init', db], check=True)
    subprocess.run([ghc_pkg, '--package-db', db, 'register', '-v0', conf], check=True)


def install(ghc_pkg, build_import_dir, haskell_dir, library_dirs, version):
    destdir = os.environ.get('DESTDIR', '')
    staged = destdir + haskell_dir if destdir else haskell_dir
    import_dir = os.path.join(haskell_dir, 'import')
    staged_import = os.path.join(staged, 'import')
    shutil.rmtree(staged_import, ignore_errors=True)
    for root, _, files in os.walk(build_import_dir):
        for name in files:
            if name.endswith('.dyn_hi'):
                rel = os.path.relpath(os.path.join(root, name), build_import_dir)
                os.makedirs(os.path.dirname(os.path.join(staged_import, rel)), exist_ok=True)
                shutil.copy2(os.path.join(root, name), os.path.join(staged_import, rel))
    # The final paths don't exist yet under DESTDIR, and ghc-pkg register
    # would check them: write the description into the database, then recache.
    db = os.path.join(staged, 'package.db')
    shutil.rmtree(db, ignore_errors=True)
    os.makedirs(db)
    with open(os.path.join(db, 'ugly.conf'), 'w') as f:
        f.write(description(ghc_pkg, version, import_dir, library_dirs))
    subprocess.run([ghc_pkg, '--package-db', db, 'recache'], check=True)
    print('Haskell package ugly: ghc -package-db ' + os.path.join(haskell_dir, 'package.db') + ' -package ugly')


if __name__ == '__main__':
    {'build': build, 'install': install}[sys.argv[1]](*sys.argv[2:])
