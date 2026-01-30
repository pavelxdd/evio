# evio justfile
# Run `just --list` to see all recipes.

set shell := ["bash", "-lc"]

default: docker-test

setup *ARGS:
    meson setup build {{ARGS}} || meson setup build --reconfigure {{ARGS}}

clean:
    rm -rf build

build:
    just setup
    meson compile -C build -v

test:
    just setup -Dtests=true
    meson test -C build -v

valgrind:
    just setup -Dtests=true -Dvalgrind=true -Dbuildtype=debug
    meson test -C build -v

sanitizers:
    just setup -Dtests=true -Db_sanitize=address,undefined -Dbuildtype=debug
    meson test -C build -v

analyzer:
    just setup -Dtests=true -Danalyzer=true -Dbuildtype=debug
    meson test -C build -v

examples:
    just setup -Dexamples=true
    meson compile -C build -v examples

format:
    set -euo pipefail
    { \
      git ls-files '*.c' '*.h' '*.h.in'; \
      git ls-files --others --exclude-standard '*.c' '*.h' '*.h.in'; \
    } | sort -u | xargs -r ./bin/astyle --options=.astylerc -Q -n

pre-commit:
    pre-commit run --all-files

docker-build:
    docker compose build --pull evio

docker-test:
    just clean
    just docker-build
    just docker-bash "just test"

docker-bash CMD="":
    if [ "{{CMD}}" = "" ]; then docker compose run --rm evio bash -l; else docker compose run --rm evio bash -lc "{{CMD}}"; fi

docker-zsh CMD="":
    if [ "{{CMD}}" = "" ]; then docker compose run --rm evio zsh -l; else docker compose run --rm evio zsh -lc "{{CMD}}"; fi

podman-build:
    podman-compose build --pull evio

podman-test:
    just clean
    just podman-build
    just podman-bash "just test"

podman-bash CMD="":
    if [ "{{CMD}}" = "" ]; then podman-compose run --rm evio bash -l; else podman-compose run --rm evio bash -lc "{{CMD}}"; fi

podman-zsh CMD="":
    if [ "{{CMD}}" = "" ]; then podman-compose run --rm evio zsh -l; else podman-compose run --rm evio zsh -lc "{{CMD}}"; fi
