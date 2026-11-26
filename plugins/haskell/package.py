#!/usr/bin/env python3
"""
The Haskell package 'ugly' (libugly_hs.so and its interfaces) in a package
database of its own, so that module libraries can be built against it:
ghc -package-db <db> -package ugly.

  package.py build GHC_PKG DB CONF VERSION IMPORT_DIR LIBRARY_DIR
      In the build tree: DB is made again from nothing; CONF is the package
      description written (the build's output).

  package.py install GHC_PKG BUILD_IMPORT_DIR HASKELL_DIR LIBRARY_DIR VERSION
      At install time (meson.add_install_script, honouring DESTDIR): the
      interfaces are copied to HASKELL_DIR/import, and HASKELL_DIR/package.db
      describes the installed package (libugly_hs.so in LIBRARY_DIR).

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


def description(ghc_pkg, version, import_dir, library_dir):
    ids = [subprocess.run([ghc_pkg, 'field', p, 'id', '--simple-output'], check=True,
                          capture_output=True, text=True).stdout.split()[0] for p in DEPENDS]
    # libugly_hs.so is linked by its own name (extra-libraries), not as
    # libHSugly-ghc<version>.so (hs-libraries): the plugin links it too.
    return '\n'.join([
        'name: ugly',
        'version: ' + version,
        'id: ugly',
        'key: ugly',
        'exposed: True',
        'exposed-modules: ' + ' '.join(EXPOSED),
        'hidden-modules: ' + ' '.join(HIDDEN),
        'import-dirs: ' + import_dir,
        'library-dirs: ' + library_dir,
        'dynamic-library-dirs: ' + library_dir,
        'extra-libraries: ugly_hs',
        'depends: ' + ' '.join(ids),
        ''])


def build(ghc_pkg, db, conf, version, import_dir, library_dir):
    with open(conf, 'w') as f:
        f.write(description(ghc_pkg, version, import_dir, library_dir))
    shutil.rmtree(db, ignore_errors=True)
    subprocess.run([ghc_pkg, 'init', db], check=True)
    subprocess.run([ghc_pkg, '--package-db', db, 'register', '-v0', conf], check=True)


def install(ghc_pkg, build_import_dir, haskell_dir, library_dir, version):
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
        f.write(description(ghc_pkg, version, import_dir, library_dir))
    subprocess.run([ghc_pkg, '--package-db', db, 'recache'], check=True)
    print('Haskell package ugly: ghc -package-db ' + os.path.join(haskell_dir, 'package.db') + ' -package ugly')


if __name__ == '__main__':
    {'build': build, 'install': install}[sys.argv[1]](*sys.argv[2:])
