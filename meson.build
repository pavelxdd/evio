project('evio', 'c',
    version: '0.9.0',
    license: 'MIT',
    meson_version: '>=0.56.0',
    default_options: [
        'buildtype=debugoptimized',
        'default_library=static',
        'c_std=c11',
        'werror=true',
        'warning_level=2',
    ]
)

current_source_dir = meson.current_source_dir()

cc = meson.get_compiler('c')

cflags = [
    '-D_GNU_SOURCE',
    '-D_LARGE_FILES',
    '-D_LARGEFILE_SOURCE',
    '-D_FILE_OFFSET_BITS=64',
    '-D_TIME_BITS=64',
]

cflags_check = [
    '-pipe',
    '-fno-common',
    '-Wfatal-errors',
    '-Wundef',
    '-pedantic',
    '-pedantic-errors',
    '-Wstrict-prototypes',
    '-Wmissing-braces',
    '-Wmissing-prototypes',
    '-Wmissing-declarations',
    '-Wduplicated-branches',
    '-Wduplicated-cond',
    '-Wimplicit-fallthrough',
    '-Wdouble-promotion',
    '-Wpointer-arith',
    '-Wfloat-equal',
    '-Wlogical-op',
    '-Wshadow',
    '-Wvla',
    '-Walloca',
    '-Wformat=2',
    '-Wwrite-strings',
    '-Wstrict-overflow=4',
    '-Wno-missing-field-initializers',
    '-Wno-strict-aliasing',
    '-Wno-clobbered',
]

if cc.get_id() == 'clang'
    cflags_check += [
        '-Wno-gnu',
        '-Wno-language-extension-token',
        '-Wno-builtin-requires-header',
        '-Wno-format-pedantic',
    ]
endif

if get_option('analyzer')
    if cc.get_id() == 'gcc'
        cflags_check += '-fanalyzer'
        cflags_check += [
            '-Wno-analyzer-malloc-leak',
            '-Wno-analyzer-fd-leak',
        ]
        if cc.version().version_compare('<12')
            cflags_check += '-Wno-analyzer-mismatching-deallocation'
        endif
    else
        warning('GCC analyzer requested, but compiler is not GCC. Ignoring.')
    endif
endif

add_project_arguments(
    cflags,
    cc.get_supported_arguments(cflags_check),
    language: 'c'
)

sources = files(
    'src/evio_utils.c',
    'src/evio_alloc.c',
    'src/evio_core.c',
    'src/evio_heap.c',
    'src/evio_list.c',
    'src/evio_loop.c',
    'src/evio_poll.c',
    'src/evio_timer.c',
    'src/evio_signal.c',
    'src/evio_async.c',
    'src/evio_idle.c',
    'src/evio_prepare.c',
    'src/evio_check.c',
    'src/evio_cleanup.c',
    'src/evio_once.c',
    'src/evio_eventfd.c',
)

if cc.has_header('linux/io_uring.h')
    sources += 'src/evio_uring.c'
    add_project_arguments('-DEVIO_IO_URING', language: 'c')
endif

headers = files(
    'src/evio.h',
    'src/evio_utils.h',
    'src/evio_alloc.h',
    'src/evio_loop.h',
    'src/evio_poll.h',
    'src/evio_timer.h',
    'src/evio_signal.h',
    'src/evio_async.h',
    'src/evio_idle.h',
    'src/evio_prepare.h',
    'src/evio_check.h',
    'src/evio_cleanup.h',
    'src/evio_once.h',
)

libevio_version_arr = meson.project_version().split('.')
libevio_version = '@0@.@1@.0'.format(libevio_version_arr[0], libevio_version_arr[1])

libevio = library('evio', sources,
    version: libevio_version,
    gnu_symbol_visibility: 'hidden',
    install: true
)

install_headers(headers, subdir: 'evio')

pkg = import('pkgconfig')
pkg.generate(libevio,
    name: 'evio',
    version: libevio_version,
    description: 'epoll event loop library'
)

if get_option('tests')
    evio_test = executable('evio_test', files('src/evio.c'),
        link_with: libevio,
        dependencies: dependency('threads', required: true),
        install: false,
    )

    if get_option('valgrind')
        valgrind = find_program('valgrind', required: true)
        valgrind_args = [
            '--tool=memcheck',
            '--leak-check=full',
            '--show-leak-kinds=all',
            '--show-reachable=no',
            '--track-origins=yes',
            '--trace-children=yes',
            '--error-exitcode=1',
        ]
        test('evio_test (valgrind)', valgrind, args: valgrind_args + [evio_test])
    else
        test('evio_test', evio_test)
    endif
endif

astyle = find_program('astyle', required: false)
if astyle.found()
    custom_target('astyle',
        output: 'astyle',
        command: [
            astyle,
            '--options=@0@/.astylerc'.format(current_source_dir),
            '-Q', '-n', '-r',
            current_source_dir / 'src/*.c',
            current_source_dir / 'src/*.h',
        ],
        console: true,
        build_by_default: false
    )
endif
