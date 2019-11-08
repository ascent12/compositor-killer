project('compositor-killer', 'c',
  version : '0.1',
  default_options : ['warning_level=2'])

add_project_arguments('-Wno-unused-parameter', language: 'c')

wl = dependency('wayland-client')
wl_egl = dependency('wayland-egl')
egl = dependency('egl')
gles = dependency('glesv2')

scanner = dependency('wayland-scanner')
scanner = scanner.get_variable(pkgconfig: 'wayland_scanner')
scanner = find_program(scanner, native: true)

protos = dependency('wayland-protocols')
protos = protos.get_variable(pkgconfig: 'pkgdatadir')

xdg_shell_c = custom_target('xdg-shell.c',
  input: protos / 'stable/xdg-shell/xdg-shell.xml',
  output: 'xdg-shell-protocol.c',
  command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

xdg_shell_h = custom_target('xdg-shell.h',
  input: protos / 'stable/xdg-shell/xdg-shell.xml',
  output: 'xdg-shell-protocol.h',
  command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

exe = executable('compositor-killer', 'main.c', xdg_shell_c, xdg_shell_h,
  dependencies: [wl, wl_egl, egl, gles])
