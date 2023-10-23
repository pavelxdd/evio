project('evio', 'c',
    version: '0.1.0',
    license: 'MIT',
    default_options: [
        'buildtype=debugoptimized',
        'default_library=static',
        'c_std=c11',
        'werror=true',
        'warning_level=2',
    ]
)

project_version = meson.project_version()
project_version_split = project_version.split('.')
project_version_major = project_version_split[0]
project_version_minor = project_version_split[1]
project_version_patch = project_version_split[2]

project_source_root = meson.current_source_dir()
project_build_root = meson.current_build_dir()

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
    '-funwind-tables',
    '-fno-common',
    '-Wfatal-errors',
    '-Wundef',
    '-pedantic',
    '-pedantic-errors',
    '-Wstrict-prototypes',
    '-Wmissing-prototypes',
    '-Wmissing-declarations',
    '-Wno-missing-field-initializers',
    '-Wno-unused-parameter',
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
    '-Wc++-compat',
    '-Wformat=2',
    '-Wformat-overflow=2',
    '-Wformat-truncation=2',
    '-Wformat-signedness',
    '-Wno-format-pedantic',
    '-Wno-format-nonliteral',
    '-Wno-clobbered',
]

if cc.get_id() == 'clang'
    cflags_check += [
        '-Wno-gnu',
        '-Wno-builtin-requires-header',
        '-Wno-missing-braces',
    ]
endif

if cc.get_id() == 'gcc' and get_option('analyzer')
    cflags_check += '-fanalyzer'
    cflags_check += [
        '-Wno-analyzer-malloc-leak',
        '-Wno-analyzer-fd-leak',
    ]

    if cc.version().version_compare('<12')
        cflags_check += '-Wno-analyzer-mismatching-deallocation'
    endif
endif

add_project_arguments(
    cflags,
    cc.get_supported_arguments(cflags_check),
    language: 'c'
)

libevio_version = project_version_major + '.' + project_version_minor + '.0'

libevio = library('evio', 'src/evio.c',
    version: libevio_version,
    gnu_symbol_visibility: 'hidden',
    install: true
)

install_headers('src/evio.h')

pkg = import('pkgconfig')
pkg.generate(libevio, version: libevio_version, description: 'Simple epoll event loop')

astyle = find_program('astyle', required: false)
if astyle.found()
    custom_target('astyle',
        output: 'astyle',
        command: [
            astyle,
            '--options=@0@/.astylerc'.format(project_source_root),
            '-Q', '-n', '-r',
            project_source_root / 'src/*.c',
            project_source_root / 'src/*.h',
        ],
        console: true,
    )
endif
