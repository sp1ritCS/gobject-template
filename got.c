#include "gotconfig.h"

#include <glib.h>
#include <gio/gio.h>
#ifdef GOT_HAS_GIO_UNIX
#include <gio/gfiledescriptorbased.h>
#endif

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static gboolean got_replacement_write_stream(gpointer write_data, const gchar* data, gsize data_len) {
	GError* err = NULL;
	if (g_output_stream_write(G_OUTPUT_STREAM(write_data), data, data_len, NULL, &err) == -1) {
		g_critical("Failed writing to stream: %s", err->message);
		g_error_free(err);
		return FALSE;
	}
	return TRUE;
}

static gboolean got_replacement_write_memory(gpointer write_data, const gchar* data, gsize data_len) {
	*(GByteArray**)write_data = g_byte_array_append(*(GByteArray**)write_data, (const guchar*)data, data_len);
	return TRUE;
}

static gboolean got_do_template_replacement(
		GHashTable* templates,
		const gchar* data,
		gsize data_len,
		gboolean(*write_fn)(gpointer write_data, const gchar* data, gsize data_len),
		gpointer write_data
	) {
	for (gsize cursor = 0; cursor < data_len; cursor++) {
		if (data[cursor] == '{') {
			for (gsize altcursor = cursor; altcursor < data_len; altcursor++) {
				if (data[altcursor] == '}') {
					gsize keylen = (altcursor - 1) - (cursor);
					g_autofree gchar* key = g_malloc(keylen + 1);
					memcpy(key, &data[cursor + 1], keylen);
					key[keylen] = '\0';

					const gchar* replacement;
					if ((replacement = g_hash_table_lookup(templates, key))) {
						if (!write_fn(write_data, replacement, strlen(replacement)))
							return FALSE;

						cursor = altcursor;
						goto next_iter;
					} else {
						break;
					}
				}
			}
		}

		if (!write_fn(write_data, &data[cursor], 1))
			return FALSE;
next_iter:
	}
	return TRUE;
}

#define GOT_FILE_KEY_STEAM "got-steam"

static gssize got_read_file(GFile* file, gchar** data) {
	GError* err = NULL;
#if defined(GOT_HAS_MMAP) && defined(GOT_HAS_GIO_UNIX)
	GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, &err);
	if (!info) {
		g_critical("Failure reading file attributes: %s", err->message);
		g_error_free(err);
		return -1;
	}
	gsize file_size = g_file_info_get_size(info);
	g_object_unref(info);

	GFileInputStream* stream = g_file_read(file, NULL, &err);
	if (!stream) {
		g_critical("Failure reading file: %s", err->message);
		g_error_free(err);
		return -1;
	}

	errno = 0;
	*data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, g_file_descriptor_based_get_fd(G_FILE_DESCRIPTOR_BASED(stream)), 0);
	if (errno != 0) {
		g_critical("Failed mapping file: %s", strerror(errno));
		g_object_unref(stream);
		return -1;
	}

	g_object_set_data(G_OBJECT(file), GOT_FILE_KEY_STEAM, stream);
#else
	gsize file_size;
	if (!g_file_load_contents(file, NULL, data, &file_size, NULL, &err)) {
		g_critical("Failed reading file: %s", err->message);
		g_error_free(err);
		return -1;
	}
#endif

	return file_size;
}

static void got_close_file(__attribute__((unused)) GFile* file, gchar* data, __attribute__((unused)) gsize data_len) {
#if defined(GOT_HAS_MMAP) && defined(GOT_HAS_GIO_UNIX)
	if (munmap(data, data_len) != 0)
		g_critical("Failed unmapping file: %s", strerror(errno));
	g_object_unref(g_object_get_data(G_OBJECT(file), GOT_FILE_KEY_STEAM));
#else
	g_free(data);
#endif
}

static const gsize GOT_CONST_ZERO = 0;
static GPtrArray* got_parse_case_name(const gchar* name) {
	g_warn_if_fail(g_str_is_ascii(name));
	gsize len = strlen(name);

	g_autoptr(GArray) element_indieces = g_array_new(FALSE, FALSE, sizeof(gsize));
	if (len > 0)
		g_array_append_val(element_indieces, GOT_CONST_ZERO);
	for (gsize i = 1; i < len; i++) {
		if (g_ascii_isupper(name[i]))
			g_array_append_val(element_indieces, i);
	}

	GPtrArray* element_tokens = g_ptr_array_new_with_free_func(g_free);
	for (gsize i = 0; i < element_indieces->len; i++) {
		gsize this = g_array_index(element_indieces, gsize, i);
		gsize next;
		if ((i + 1) < element_indieces->len)
			next = g_array_index(element_indieces, gsize, i + 1);
		else
			next = len;

		gsize token_len = next - this;
		gchar* token = g_malloc(token_len + 1);
		memcpy(token, &name[this], token_len);
		token[token_len] = '\0';

		g_ptr_array_add(element_tokens, token);
	}

	return element_tokens;
}

typedef struct {
	gchar* lower;
	gchar* upper;
	gchar* caps;
} GotTokenVariations;

static GotTokenVariations got_get_token_variations_from_token(const gchar* token) {
	GotTokenVariations variations;

	gsize len = strlen(token);

	variations.lower = g_malloc(len + 1);
	for (gsize i = 0; i < len; i++)
		variations.lower[i] = g_ascii_tolower(token[i]);
	variations.lower[len] = '\0';

	variations.upper = g_malloc(len + 1);
	for (gsize i = 0; i < len; i++)
		variations.upper[i] = i == 0 ? g_ascii_toupper(token[i]) : g_ascii_tolower(token[i]);
	variations.upper[len] = '\0';

	variations.caps = g_malloc(len + 1);
	for (gsize i = 0; i < len; i++)
		variations.caps[i] = g_ascii_toupper(token[i]);
	variations.caps[len] = '\0';

	return variations;
}

typedef struct {
	gchar* namespace_lower;
	gchar* namespace_upper;
	gchar* namespace_caps;
	gchar* name_lower;
	gchar* name_lower_condensed;
	gchar* name_upper;
	gchar* name_caps;
	gchar* name_caps_condensed;
} GotElementSection;

static void got_element_section_free_inner(GotElementSection* section) {
	g_free(section->namespace_lower);
	g_free(section->namespace_upper);
	g_free(section->namespace_caps);
	g_free(section->name_lower);
	g_free(section->name_lower_condensed);
	g_free(section->name_upper);
	g_free(section->name_caps);
	g_free(section->name_caps_condensed);
}

static GotElementSection got_get_element_section_from_name(const gchar* name) {
	GotElementSection element;

	g_autoptr(GPtrArray) tokens = got_parse_case_name(name);
	if (tokens->len <= 0) {
		element.namespace_lower = NULL;
		element.namespace_upper = NULL;
		element.namespace_caps = NULL;
	} else {
		GotTokenVariations variations = got_get_token_variations_from_token(tokens->pdata[0]);
		element.namespace_lower = variations.lower;
		element.namespace_upper = variations.upper;
		element.namespace_caps = variations.caps;
	}

	if (tokens->len <= 1) {
		element.name_lower = NULL;
		element.name_lower_condensed = NULL;
		element.name_upper = NULL;
		element.name_caps = NULL;
		element.name_caps_condensed = NULL;
	} else {
		GString* name_lower = g_string_new(NULL);
		GString* name_lower_condensed = g_string_new(NULL);
		GString* name_upper = g_string_new(NULL);
		GString* name_caps = g_string_new(NULL);
		GString* name_caps_condensed = g_string_new(NULL);
		for (guint i = 1; i < tokens->len; i++) {
			GotTokenVariations variations = got_get_token_variations_from_token(tokens->pdata[i]);
			g_string_append(name_lower, variations.lower);
			g_string_append(name_lower_condensed, variations.lower);
			g_string_append(name_upper, variations.upper);
			g_string_append(name_caps, variations.caps);
			g_string_append(name_caps_condensed, variations.caps);
			if ((i + 1) < tokens->len) {
				g_string_append(name_lower, "_");
				g_string_append(name_caps, "_");
			}

			g_free(variations.lower);
			g_free(variations.upper);
			g_free(variations.caps);
		}

		element.name_lower = g_string_free(name_lower, FALSE);
		element.name_lower_condensed = g_string_free(name_lower_condensed, FALSE);
		element.name_upper = g_string_free(name_upper, FALSE);
		element.name_caps = g_string_free(name_caps, FALSE);
		element.name_caps_condensed = g_string_free(name_caps_condensed, FALSE);
	}

	return element;
}

typedef struct {
	gchar* parent;
	gchar* parent_type;
} GotParentSection;

typedef struct {
	GotElementSection element;
	GotParentSection parent;
} GotReplacements;

static void got_replacements_free_inner(GotReplacements* replacements) {
	got_element_section_free_inner(&replacements->element);
	g_free(replacements->parent.parent);
	g_free(replacements->parent.parent_type);
}

static GotReplacements got_generate_replacements_from_name(const gchar* name, const gchar* parent) {
	GotReplacements replacements;

	replacements.element = got_get_element_section_from_name(name);
	GotElementSection parent_element = got_get_element_section_from_name(parent == NULL ? "GObject" : parent);
	replacements.parent.parent = g_strdup_printf("%s%s", parent_element.namespace_upper, parent_element.name_upper);
	replacements.parent.parent_type = g_strdup_printf("%s_TYPE_%s", parent_element.namespace_caps, parent_element.name_caps);
	got_element_section_free_inner(&parent_element);

	return replacements;
}

static GHashTable* got_new_replacement_table_from_replacements(GotReplacements* replacements) {
	GHashTable* replacement_table = g_hash_table_new(g_str_hash, g_str_equal);

	g_hash_table_insert(replacement_table, "ns", replacements->element.namespace_lower);
	g_hash_table_insert(replacement_table, "Ns", replacements->element.namespace_upper);
	g_hash_table_insert(replacement_table, "NS", replacements->element.namespace_caps);

	g_hash_table_insert(replacement_table, "name_wide", replacements->element.name_lower);
	g_hash_table_insert(replacement_table, "name", replacements->element.name_lower_condensed);
	g_hash_table_insert(replacement_table, "Name", replacements->element.name_upper);
	g_hash_table_insert(replacement_table, "NAME_WIDE", replacements->element.name_caps);
	g_hash_table_insert(replacement_table, "NAME", replacements->element.name_caps_condensed);

	g_hash_table_insert(replacement_table, "Parent", replacements->parent.parent);
	g_hash_table_insert(replacement_table, "PARENT_TYPE", replacements->parent.parent_type);

	return replacement_table;
}

static const gchar* got_application_name = "got";

static void got_print_usage(void(*print_fn)(const gchar* template, ...)) {
	print_fn("\
Usage: %s [OPTIONS] <template> <ClassName> [Parent]\n\
  -o, --output=<OUTPUT> Set the directory where the resulting files will be created in.\n\
  -l, --list-templates  List all available templates that can be used.\n\
  -h, --help            Show the help page and exit.\n\
  -v, --version         Show version and licensing information and exit.\n", got_application_name);
}

static void got_print_help(void(*print_fn)(const gchar* template, ...)) {
	print_fn("GOT - GObject Template; Advanced boilerplate sourcecode generator for GObject.\n\n");
	got_print_usage(print_fn);
	print_fn("\n\
Exit status:\n\
  %d: Process exited as expected.\n\
  %d: An error occured during runtime of the software.\n\
  %d: The command-line invocation of %s was faulty.\n", 0, 1, 2, got_application_name);
}

static void got_print_version(void(*print_fn)(const gchar* template, ...)) {
	print_fn("\
%s (version %s)\n\
Copyright (c) 2023 Florian \"sp1rit\" <sp1rit@national.shitposting.agency>\n\
\n\
Licensed under the GNU Affero General Public License version 3 or later.\n\
  You should have received a copy of it along with this program.\n\
  If not, see <https://www.gnu.org/licenses/>.\n\
\n\
This is free software: you are free to change and redistribute it.\n\
This program comes with ABSOLUTELY NO WARRANTY, to the extent permitted by law.\n", got_application_name, GOT_VERSION);
}

static gint got_list_templates(const gchar* templates_dir) {
	g_autoptr(GFile) template = g_file_new_for_path(templates_dir);

	GError* err = NULL;
	gchar* query_attributes = g_strconcat(G_FILE_ATTRIBUTE_STANDARD_NAME, ",", G_FILE_ATTRIBUTE_STANDARD_TYPE, NULL);
	g_autoptr(GFileEnumerator) iter = g_file_enumerate_children(template, query_attributes, G_FILE_QUERY_INFO_NONE, NULL, &err);
	g_free(query_attributes);
	if (!iter) {
		g_critical("Failed reading templates dir: %s", err->message);
		g_error_free(err);
		return 1;
	}

	gint return_code = 0;
	while (TRUE) {
		GFileInfo* info;
		if (!g_file_enumerator_iterate(iter, &info, NULL, NULL, &err)) {
			g_critical("Failed querying file: %s", err->message);
			return_code = 1;
			g_error_free(err);
			err = NULL;
			continue;
		}
		if (!info)
			break;
		if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
			g_print("%s\n", g_file_info_get_name(info));
	}

	return return_code;
}

int main(int argc, char** argv) {
	if (argc > 0)
		got_application_name = argv[0];

	const gchar* templates_dir;
	if (!(templates_dir = g_getenv("GOT_TEMPLATES_DIR")))
		templates_dir = GOT_TEMPLATES_DIR;

	gint c;
	const gchar* template_name;
	const gchar* class_name;
	const gchar* parent = NULL;
	const gchar* output_dir = ".";

	const struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'v'},
		{"list-templates", no_argument, NULL, 'l'},
		{"output", required_argument, NULL, 'o'},
		{0, 0, 0, 0}
	};

	while ((c = getopt_long(argc, argv, "hlvo:", long_options, NULL)) != -1) {
		switch (c) {
			case 'h':
				got_print_help(g_print);
				return 0;
			case 'v':
				got_print_version(g_print);
				return 0;
			case 'l':
				got_list_templates(templates_dir);
				return 0;
			case 'o':
				output_dir = optarg;
				break;
			case '?':
			default:
				got_print_usage(g_printerr);
				return 2;
		}
	}

	if (optind + 1 < argc) {
		template_name = argv[optind++];
		class_name = argv[optind++];
		if (optind < argc) {
			parent = argv[optind++];
		}
	} else {
		got_print_usage(g_printerr);
		return 2;
	}

	g_autoptr(GFile) template = g_file_new_build_filename(templates_dir, template_name, NULL);

	GError* err = NULL;
	g_autoptr(GFileEnumerator) iter = g_file_enumerate_children(template, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE, NULL, &err);
	if (!iter) {
		g_critical("Failed reading template dir: %s", err->message);
		g_error_free(err);
		return 1;
	}

	GotReplacements replacements = got_generate_replacements_from_name(class_name, parent);
	GHashTable* replacement_table = got_new_replacement_table_from_replacements(&replacements);									

	while (TRUE) {
		GFileInfo* info;
		GFile* file;
		if (!g_file_enumerator_iterate(iter, &info, &file, NULL, &err)) {
			g_critical("Failed querying file: %s", err->message);
			g_error_free(err);
			err = NULL;
			continue;
		}
		if (!info)
			break;

		const gchar* filename = g_file_info_get_name(info);
		if (!g_str_has_suffix(filename, ".got"))
			continue;

		GByteArray* new_filename = g_byte_array_new();
		got_do_template_replacement(replacement_table, filename, strlen(filename), got_replacement_write_memory, &new_filename);
		new_filename = g_byte_array_remove_range(new_filename, new_filename->len - 4, 4);
		new_filename = g_byte_array_append(new_filename, (guchar*)"\0", 1);
		gchar* fns = (gchar*)g_byte_array_free(new_filename, FALSE);
		g_autoptr(GFile) output = g_file_new_build_filename(output_dir, fns, NULL);
		g_free(fns);
		GOutputStream* output_stream = G_OUTPUT_STREAM(g_file_create(output, G_FILE_CREATE_NONE, NULL, &err));
		if (output_stream == NULL) {
			g_critical("Failed creating file for writing: %s", err->message);
			g_error_free(err);
			err = NULL;
			continue;
		}

		gchar* data;
		gssize data_len = got_read_file(file, &data);
		if (data_len < 0)
			return 1;

		got_do_template_replacement(replacement_table, data, data_len, got_replacement_write_stream, output_stream);
		g_object_unref(output_stream);

		got_close_file(file, data, data_len);
	}

	g_hash_table_unref(replacement_table);
	got_replacements_free_inner(&replacements);

	return 0;
}
