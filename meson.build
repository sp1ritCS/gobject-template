project('gobject-template', 'c',
	version: '0.1.0',
	default_options: ['c_std=gnu2x', 'warning_level=3']
)

src = [
	'got.c'
]

app = executable('got', src,
	dependencies: [
		dependency('glib-2.0'),
		dependency('gio-2.0'),
		dependency('gio-unix-2.0')
	],
	c_args: ['-DG_LOG_DOMAIN="GOT"'],
	install: true
)
