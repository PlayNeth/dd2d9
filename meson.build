project('dd2d9', 'cpp',
  version : '1.9.5',
  default_options : ['cpp_std=c++17', 'warning_level=2', 'buildtype=release', 'b_ndebug=if-release'])

cpp = meson.get_compiler('cpp')
if host_machine.system() != 'windows' or host_machine.cpu_family() != 'x86'
  error('DD2D9 targets 32-bit Windows; use --cross-file build-win32.txt')
endif

# -ffp-contract=off: no fused multiply-adds, even where -march allows them (the vertex
# pipeline's four-lane code gives exactly the scalar code's answers, operation by operation).
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', '-msse2', '-mfpmath=sse',
                      '-ffp-contract=off', language : 'cpp')
# The version the log's first line gives: the project's, above.
add_project_arguments('-DDD2D9_VERSION="' + meson.project_version() + '"', language : 'cpp')

win_deps = []
foreach lib : ['d3d9', 'dxguid', 'uuid', 'gdi32', 'user32', 'ole32', 'winmm']
  win_deps += cpp.find_library(lib)
endforeach

static_link = ['-static', '-static-libgcc', '-static-libstdc++']

ddraw = shared_library('ddraw',
  'src/main.cpp', 'src/attach.cpp', 'src/blit.cpp', 'src/blitcore.cpp', 'src/blitfourcc.cpp',
    'src/buffer.cpp', 'src/clipper.cpp', 'src/compat.cpp', 'src/compatdisplay.cpp', 'src/compatgdi.cpp',
    'src/compatsys.cpp', 'src/config.cpp', 'src/convert.cpp', 'src/d3d6dev.cpp', 'src/d3d6draw.cpp',
    'src/d3d6drv.cpp', 'src/d3d6exec.cpp', 'src/d3d6obj.cpp', 'src/d3d6vp.cpp', 'src/d3d6xform.cpp',
    'src/d3d7draw.cpp', 'src/d3d7obj.cpp', 'src/d3d7pipe.cpp', 'src/d3d7sb.cpp', 'src/d3d7state.cpp',
    'src/d3dxlate.cpp', 'src/ddcaps.cpp', 'src/ddcreate.cpp', 'src/ddgdi.cpp', 'src/ddhook.cpp',
    'src/ddmode.cpp', 'src/ddraw.cpp', 'src/ddscreen.cpp', 'src/ddtiming.cpp', 'src/ddvideoport.cpp',
    'src/ddwindow.cpp', 'src/format.cpp', 'src/fourcc.cpp', 'src/gpu.cpp', 'src/gpucontext.cpp',
    'src/gpudepth.cpp', 'src/gpudraw.cpp', 'src/gpuformat.cpp', 'src/gpusurf.cpp', 'src/gputex.cpp',
    'src/hooks.cpp', 'src/input.cpp', 'src/inputpaint.cpp', 'src/lock.cpp', 'src/log.cpp',
    'src/overlay.cpp', 'src/palette.cpp', 'src/presentconv.cpp', 'src/presenter.cpp', 'src/presentpass.cpp',
    'src/quadpass.cpp', 'src/surface.cpp', 'src/util.cpp', 'src/video.cpp',
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

# What a ddraw.dll and its driver answer (see the two files' comments). Built
# without SSE (the project's -msse2 undone): it is run on machines of the time, a Pentium II
# among them, where dd2d9 itself doesn't run.
executable('fmtprobe', 'tests/fmtprobe.cpp', 'tests/fmtprobe2.cpp',
  dependencies : [cpp.find_library('ddraw'), cpp.find_library('dxguid'), cpp.find_library('gdi32'),
                  cpp.find_library('advapi32')],
  cpp_args : ['-march=pentium', '-mno-sse2', '-mno-sse', '-mno-mmx', '-mfpmath=387'],
  link_args : static_link + ['-mwindows'],
  install : false)
# fmtprobe's gamma calibrator (tests/fmtcal.cpp), put beside it.
shared_library('fmtcal', 'tests/fmtcal.cpp',
  name_prefix : '',
  cpp_args : ['-march=pentium', '-mno-sse2', '-mno-sse', '-mno-mmx', '-mfpmath=387'],
  link_args : static_link + ['-Wl,--kill-at'],
  install : false)

# A DLL that reads the machine's memory (tests/hooktool.cpp), for ddtest's check of the
# modules dd2d9's hooks reach.
shared_library('hooktool', 'tests/hooktool.cpp',
  name_prefix : '',
  link_args : static_link + ['-Wl,--kill-at'],
  install : false)

# A Direct3D 9 with DF24 and DF16 but no INTZ (tests/dfshim.cpp), for dd2d9's Z buffers there.
shared_library('dfshim', 'tests/dfshim.cpp',
  name_prefix : '',
  link_args : static_link + ['-Wl,--kill-at'],
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
