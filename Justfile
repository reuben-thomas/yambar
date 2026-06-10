build_dir := "bld/release"
debug_dir := "bld/debug"

default:
    @just --list

setup:
    meson setup --buildtype=release {{build_dir}}

setup-debug:
    meson setup --buildtype=debug {{debug_dir}}

reconfigure:
    meson setup --reconfigure {{build_dir}}

build:
    ninja -C {{build_dir}}

build-debug:
    ninja -C {{debug_dir}}

clean:
    ninja -C {{build_dir}} clean

wipe:
    rm -rf {{build_dir}} {{debug_dir}}

run *ARGS: build
    {{build_dir}}/yambar {{ARGS}}

run-debug *ARGS: build-debug
    {{debug_dir}}/yambar {{ARGS}}

install: build
    ninja -C {{build_dir}} install

test: build
    meson test -C {{build_dir}}

format:
    clang-format -i *.c *.h modules/*.c modules/*.h particles/*.c decorations/*.c bar/*.c bar/*.h

format-check:
    clang-format --dry-run --Werror *.c *.h modules/*.c modules/*.h particles/*.c decorations/*.c bar/*.c bar/*.h
