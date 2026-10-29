project('dd2d9', 'cpp',
  version : '0.7.4',
  default_options : ['cpp_std=c++17', 'warning_level=2', 'buildtype=release', 'b_ndebug=if-release'])

cpp = meson.get_compiler('cpp')
if host_machine.system() != 'windows' or host_machine.cpu_family() != 'x86'
  error('DD2D9 targets 32-bit Windows; use --cross-file build-win32.txt')
endif

add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', '-msse2', language : 'cpp')

win_deps = []
foreach lib : ['d3d9', 'dxguid', 'uuid', 'gdi32', 'user32', 'ole32', 'winmm']
  win_deps += cpp.find_library(lib)
endforeach

static_link = ['-static', '-static-libgcc', '-static-libstdc++']

ddraw = shared_library('ddraw',
  'src/main.cpp', 'src/util.cpp', 'src/ddraw.cpp', 'src/surface.cpp', 'src/blit.cpp', 'src/presenter.cpp', 'src/d3dproxy.cpp',
  name_prefix : '',
  vs_module_defs : 'src/ddraw.def',
  dependencies : win_deps,
  link_args : static_link + ['-Wl,--enable-stdcall-fixup', '-s'],
  install : true)

# Test program; links against the system ddraw.dll import library, so it runs
# with whichever ddraw.dll sits next to it.
executable('ddtest', 'tests/ddtest.cpp',
  dependencies : [cpp.find_library('ddraw'), cpp.find_library('dxguid'), cpp.find_library('gdi32'),
                  cpp.find_library('user32')],
  link_args : static_link + ['-mwindows'],
  install : false)

# Performance benchmark (blitter throughput, flip loop, windowed updates).
executable('ddbench', 'tests/ddbench.cpp',
  dependencies : [cpp.find_library('ddraw'), cpp.find_library('dxguid'), cpp.find_library('gdi32'),
                  cpp.find_library('user32')],
  link_args : static_link + ['-mwindows'],
  install : false)

# Reload test: the DLL freed and loaded again while the window stays subclassed.
executable('reloadtest', 'tests/reloadtest.cpp',
  dependencies : [cpp.find_library('user32')],
  link_args : static_link + ['-mwindows'],
  install : false)

# Direct3D 7 test (a 3D game as D7VK sees it); needs a D3D7 implementation such as D7VK.
executable('d3d7test', 'tests/d3d7test.cpp',
  dependencies : [cpp.find_library('ddraw'), cpp.find_library('dxguid'), cpp.find_library('gdi32'),
                  cpp.find_library('user32')],
  link_args : static_link + ['-mwindows'],
  install : false)

