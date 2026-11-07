project('dd2d9', 'cpp',
  version : '1.5.0',
  default_options : ['cpp_std=c++17', 'warning_level=2', 'buildtype=release', 'b_ndebug=if-release'])

cpp = meson.get_compiler('cpp')
if host_machine.system() != 'windows' or host_machine.cpu_family() != 'x86'
  error('DD2D9 targets 32-bit Windows; use --cross-file build-win32.txt')
endif

# -ffp-contract=off: no fused multiply-adds, even where -march allows them (the vertex
# pipeline's four-lane code gives exactly the scalar code's answers, operation by operation).
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', '-msse2', '-mfpmath=sse',
                      '-ffp-contract=off', language : 'cpp')

win_deps = []
foreach lib : ['d3d9', 'dxguid', 'uuid', 'gdi32', 'user32', 'ole32', 'winmm']
  win_deps += cpp.find_library(lib)
endforeach

static_link = ['-static', '-static-libgcc', '-static-libstdc++']

ddraw = shared_library('ddraw',
  'src/main.cpp', 'src/util.cpp', 'src/ddraw.cpp', 'src/surface.cpp', 'src/blit.cpp', 'src/presenter.cpp', 'src/gpu.cpp',
    'src/gpures.cpp', 'src/convert.cpp', 'src/dxt.cpp', 'src/d3d7.cpp', 'src/d3d6obj.cpp', 'src/d3d6vp.cpp',
    'src/d3d6dev.cpp', 'src/d3d6pipe.cpp', 'src/input.cpp', 'src/hooks.cpp', 'src/compat.cpp', 'src/video.cpp', 'src/overlay.cpp',
  name_prefix : '',
  vs_module_defs : 'src/ddraw.def',
  dependencies : win_deps,
  link_args : static_link + ['-Wl,--enable-stdcall-fixup', '-s'],
  install : true)

# Test program; links against the system ddraw.dll import library, so it runs
# with whichever ddraw.dll sits next to it.
executable('ddtest', 'tests/ddtest.cpp',
  dependencies : [cpp.find_library('ddraw'), cpp.find_library('dxguid'), cpp.find_library('gdi32'),
                  cpp.find_library('user32'), cpp.find_library('winmm'), cpp.find_library('vfw32')],
  link_args : static_link + ['-mwindows'],
  install : false)
# ddtest's stand-in for a Windows module calling a dd2d9 hook (copied into the Windows folder).
shared_library('wincaller', 'tests/wincaller.cpp',
  name_prefix : '',
  link_args : static_link + ['-Wl,--kill-at'],
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

# Focus test: an exclusive-mode flip chain switched away from and back ("paintfg" arg).
executable('focustest', 'tests/focustest.cpp',
  dependencies : [cpp.find_library('user32'), cpp.find_library('ddraw')],
  link_args : static_link + ['-mwindows'],
  install : false)

# Direct3D 7 test: a triangle through dd2d9's Direct3D 7, 2D on top, flipped ("3d", "gamma" args).
executable('d3d7test', 'tests/d3d7test.cpp',
  dependencies : [cpp.find_library('ddraw'), cpp.find_library('dxguid'), cpp.find_library('gdi32'),
                  cpp.find_library('user32')],
  link_args : static_link + ['-mwindows'],
  install : false)

# Shim test: dd2d9 under a stand-in for Windows' compatibility shims (fakeshim.dll hooks
# ddraw.dll's GetProcAddress import as it loads, before it starts, as the shim engine does).
fakeshim = shared_library('fakeshim', 'tests/fakeshim.cpp',
  name_prefix : '',
  link_args : static_link,
  install : false)
executable('shimtest', 'tests/shimtest.cpp',
  link_with : fakeshim,
  link_args : static_link + ['-mwindows'],
  install : false)

# Direct3D 3-6 check and benchmark: ProcessVertices, DrawIndexedPrimitive and execute buffers,
# hashed and timed (two builds compared by their hashes).
executable('d3dimbench', 'tests/d3dimbench.cpp',
  dependencies : [cpp.find_library('ddraw'), cpp.find_library('dxguid'), cpp.find_library('gdi32'),
                  cpp.find_library('user32')],
  link_args : static_link + ['-mwindows'],
  install : false)


# ddtrace: a separate logging ddraw.dll for developers (tools/ddtrace/README.md).
subdir('tools/ddtrace')
