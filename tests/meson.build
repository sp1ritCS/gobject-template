testenv = environment()
testenv.set('GOT_PATH', app.full_path())
testenv.set('GOT_TEMPLATES_DIR', meson.project_source_root() / 'templates')

test('template expansion', find_program('./test_template_expansion.sh'),
	depends: app,
	env: testenv
)
