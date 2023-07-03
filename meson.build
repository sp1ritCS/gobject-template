project('gobject-template', 'c',
	version: '0.1.0',
	default_options: ['c_std=gnu2x', 'warning_level=3']
)

src = [
	'got.c'
]

bin_name = 'got'
templates_dir = get_option('datadir') / meson.project_name()

gio_unix = dependency('gio-unix-2.0', required: false)

conf_data = configuration_data()
conf_data.set('got_binary_name', bin_name)
conf_data.set('got_version', meson.project_version())
conf_data.set('got_templates_dir', get_option('prefix') / templates_dir)

compiler = meson.get_compiler('c')
conf_data.set('GOT_HAS_MMAP', compiler.has_function('mmap', prefix: '#include <sys/mman.h>'))
conf_data.set('GOT_HAS_GIO_UNIX', gio_unix.found())

config = configure_file(input: 'gotconfig.h.in', output: 'gotconfig.h', configuration: conf_data)

devenv = environment()
devenv.set('GOT_TEMPLATES_DIR', meson.project_source_root() / 'templates')
devenv.set('G_MESSAGES_DEBUG', 'all')
meson.add_devenv(devenv)

app = executable(bin_name, src, config,
	dependencies: [
		dependency('glib-2.0'),
		dependency('gio-2.0'),
		gio_unix
	],
	c_args: ['-DG_LOG_DOMAIN="GOT"'],
	install: true
)

install_subdir('templates', install_dir : templates_dir, strip_directory : true)

subdir('tests')
