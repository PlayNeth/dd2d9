# ddtrace: a separate ddraw.dll that logs a game's DirectDraw and Direct3D calls and passes
# them on (see README.md). Built next to dd2d9, as build/tools/ddtrace/ddraw.dll.
shared_library('ddraw', 'ddtrace.cpp',
  name_prefix : '',
  vs_module_defs : 'ddraw.def',
  dependencies : [cpp.find_library('dxguid'), cpp.find_library('uuid')],
  cpp_args : ['-Wno-cast-function-type'],
  link_args : static_link + ['-s'],
  install : false)
