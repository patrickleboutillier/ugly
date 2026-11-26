#!/usr/bin/env python3
"""
Registers the Haskell package 'ugly' (libugly_hs.so and its interfaces) in a
package database of its own, so that module libraries can be built against
it: ghc -package-db <db> -package ugly.

  package.py GHC_PKG DB CONF VERSION IMPORT_DIR LIBRARY_DIR

DB is made again from nothing; CONF is the package description written (the
build's output). The packages 'ugly' depends on are found by name, with
GHC_PKG, in GHC's global database.
"""
import shutil
import subprocess
import sys

DEPENDS = ['base', 'bytestring', 'containers', 'deepseq', 'template-haskell', 'text']
EXPOSED = ['Ugly', 'Ugly.Export', 'Ugly.Types', 'Ugly.Value']
HIDDEN = ['Ugly.Marshal', 'Ugly.Native', 'Ugly.Plugin']


def main():
    ghc_pkg, db, conf, version, import_dir, library_dir = sys.argv[1:]
    ids = [subprocess.run([ghc_pkg, 'field', p, 'id', '--simple-output'], check=True,
                          capture_output=True, text=True).stdout.split()[0] for p in DEPENDS]
    with open(conf, 'w') as f:
        # libugly_hs.so is linked by its own name (extra-libraries), not as
        # libHSugly-ghc<version>.so (hs-libraries): the plugin links it too.
        f.write('\n'.join([
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
            '']))
    shutil.rmtree(db, ignore_errors=True)
    subprocess.run([ghc_pkg, 'init', db], check=True)
    subprocess.run([ghc_pkg, '--package-db', db, 'register', '-v0', conf], check=True)


if __name__ == '__main__':
    main()
