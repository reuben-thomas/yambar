bar_backends = []

if backend_x11
  bar_x11 = declare_dependency(sources: ['xcb.c', 'xcb.h'], dependencies: [xcb_stuff])
  bar_backends += [bar_x11]
endif

if backend_wayland
  wayland_protocols = dependency('wayland-protocols')
  wayland_protocols_datadir = wayland_protocols.get_variable('pkgdatadir')

  wscanner = dependency('wayland-scanner', native: true)
  wscanner_prog = find_program(
    wscanner.get_variable('wayland_scanner'), native: true)

  wl_proto_headers = []
  wl_proto_src = []
  wl_proto_xml = [
    '../external/wlr-layer-shell-unstable-v1.xml',
    wayland_protocols_datadir + '/stable/xdg-shell/xdg-shell.xml',
    wayland_protocols_datadir + '/unstable/xdg-output/xdg-output-unstable-v1.xml']

    if wayland_protocols.version().version_compare('>=1.32')
      wl_proto_xml += [
        wayland_protocols_datadir + '/staging/cursor-shape/cursor-shape-v1.xml',
        wayland_protocols_datadir + '/unstable/tablet/tablet-unstable-v2.xml']
    endif

    foreach prot : wl_proto_xml
    wl_proto_headers += custom_target(
      prot.underscorify() + '-client-header',
      output: '@BASENAME@.h',
      input: prot,
      command: [wscanner_prog, 'client-header', '@INPUT@', '@OUTPUT@'])

    wl_proto_src += custom_target(
      prot.underscorify() + '-private-code',
      output: '@BASENAME@.c',
      input: prot,
      command: [wscanner_prog, 'private-code', '@INPUT@', '@OUTPUT@'])
  endforeach

  bar_wayland = declare_dependency(
    sources: ['wayland.c', 'wayland.h'] + wl_proto_src + wl_proto_headers,
    dependencies: [wayland_client, wayland_cursor, tllist])

  bar_backends += [bar_wayland]
endif

bar = declare_dependency(
  sources: ['bar.c', 'bar.h', 'private.h', 'backend.h'],
  dependencies: bar_backends + [threads])

install_headers('bar.h', subdir: 'yambar/bar')
