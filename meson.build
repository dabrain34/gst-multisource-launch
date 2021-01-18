project('gst-multisource-launch', 'c', version : '1.19.0.1', license : 'LGPL')

cc = meson.get_compiler('c')

gst_dep = dependency('gstreamer-1.0',
    fallback : ['gstreamer', 'gst_dep'])

executable('gst-multisource-launch',
    'src/main.c',
    install: true,
    dependencies : [gst_dep]
  )
