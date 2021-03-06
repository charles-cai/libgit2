#include "common.h"
#include "repository.h"
#include "filebuf.h"
#include "git2/blob.h"
#include "git2/tree.h"
#include <ctype.h>

const char *git_attr__true  = "[internal]__TRUE__";
const char *git_attr__false = "[internal]__FALSE__";
const char *git_attr__unset = "[internal]__UNSET__";

static int sort_by_hash_and_name(const void *a_raw, const void *b_raw);
static void git_attr_rule__clear(git_attr_rule *rule);

int git_attr_file__new(
	git_attr_file **attrs_ptr,
	git_attr_file_source from,
	const char *path,
	git_pool *pool)
{
	git_attr_file *attrs = NULL;

	attrs = git__calloc(1, sizeof(git_attr_file));
	GITERR_CHECK_ALLOC(attrs);

	if (pool)
		attrs->pool = pool;
	else {
		attrs->pool = git__calloc(1, sizeof(git_pool));
		if (!attrs->pool || git_pool_init(attrs->pool, 1, 0) < 0)
			goto fail;
		attrs->pool_is_allocated = true;
	}

	if (path) {
		size_t len = strlen(path);

		attrs->key = git_pool_malloc(attrs->pool, (uint32_t)len + 3);
		GITERR_CHECK_ALLOC(attrs->key);

		attrs->key[0] = '0' + from;
		attrs->key[1] = '#';
		memcpy(&attrs->key[2], path, len);
		attrs->key[len + 2] = '\0';
	}

	if (git_vector_init(&attrs->rules, 4, NULL) < 0)
		goto fail;

	*attrs_ptr = attrs;
	return 0;

fail:
	git_attr_file__free(attrs);
	attrs_ptr = NULL;
	return -1;
}

int git_attr_file__parse_buffer(
	git_repository *repo, const char *buffer, git_attr_file *attrs)
{
	int error = 0;
	const char *scan = NULL;
	char *context = NULL;
	git_attr_rule *rule = NULL;

	assert(buffer && attrs);

	scan = buffer;

	/* if subdir file path, convert context for file paths */
	if (attrs->key && git__suffixcmp(attrs->key, "/" GIT_ATTR_FILE) == 0) {
		context = attrs->key + 2;
		context[strlen(context) - strlen(GIT_ATTR_FILE)] = '\0';
	}

	while (!error && *scan) {
		/* allocate rule if needed */
		if (!rule && !(rule = git__calloc(1, sizeof(git_attr_rule)))) {
			error = -1;
			break;
		}

		/* parse the next "pattern attr attr attr" line */
		if (!(error = git_attr_fnmatch__parse(
				&rule->match, attrs->pool, context, &scan)) &&
			!(error = git_attr_assignment__parse(
				repo, attrs->pool, &rule->assigns, &scan)))
		{
			if (rule->match.flags & GIT_ATTR_FNMATCH_MACRO)
				/* should generate error/warning if this is coming from any
				 * file other than .gitattributes at repo root.
				 */
				error = git_attr_cache__insert_macro(repo, rule);
			else
				error = git_vector_insert(&attrs->rules, rule);
		}

		/* if the rule wasn't a pattern, on to the next */
		if (error < 0) {
			git_attr_rule__clear(rule); /* reset rule contents */
			if (error == GIT_ENOTFOUND)
				error = 0;
		} else {
			rule = NULL; /* vector now "owns" the rule */
		}
	}

	git_attr_rule__free(rule);

	/* restore file path used for context */
	if (context)
		context[strlen(context)] = '.'; /* first char of GIT_ATTR_FILE */

	return error;
}

int git_attr_file__new_and_load(
	git_attr_file **attrs_ptr,
	const char *path)
{
	int error;
	git_buf content = GIT_BUF_INIT;

	if ((error = git_attr_file__new(attrs_ptr, 0, path, NULL)) < 0)
		return error;

	if (!(error = git_futils_readbuffer(&content, path)))
		error = git_attr_file__parse_buffer(
			NULL, git_buf_cstr(&content), *attrs_ptr);

	git_buf_free(&content);

	if (error) {
		git_attr_file__free(*attrs_ptr);
		*attrs_ptr = NULL;
	}

	return error;
}

void git_attr_file__free(git_attr_file *file)
{
	unsigned int i;
	git_attr_rule *rule;

	if (!file)
		return;

	git_vector_foreach(&file->rules, i, rule)
		git_attr_rule__free(rule);

	git_vector_free(&file->rules);

	if (file->pool_is_allocated) {
		git_pool_clear(file->pool);
		git__free(file->pool);
	}
	file->pool = NULL;

	git__free(file);
}

uint32_t git_attr_file__name_hash(const char *name)
{
	uint32_t h = 5381;
	int c;
	assert(name);
	while ((c = (int)*name++) != 0)
		h = ((h << 5) + h) + c;
	return h;
}


int git_attr_file__lookup_one(
	git_attr_file *file,
	const git_attr_path *path,
	const char *attr,
	const char **value)
{
	unsigned int i;
	git_attr_name name;
	git_attr_rule *rule;

	*value = NULL;

	name.name = attr;
	name.name_hash = git_attr_file__name_hash(attr);

	git_attr_file__foreach_matching_rule(file, path, i, rule) {
		int pos = git_vector_bsearch(&rule->assigns, &name);

		if (pos >= 0) {
			*value = ((git_attr_assignment *)
					  git_vector_get(&rule->assigns, pos))->value;
			break;
		}
	}

	return 0;
}


bool git_attr_fnmatch__match(
	git_attr_fnmatch *match,
	const git_attr_path *path)
{
	int fnm;

	if (match->flags & GIT_ATTR_FNMATCH_DIRECTORY && !path->is_dir)
		return false;

	if (match->flags & GIT_ATTR_FNMATCH_FULLPATH)
		fnm = p_fnmatch(match->pattern, path->path, FNM_PATHNAME);
	else if (path->is_dir)
		fnm = p_fnmatch(match->pattern, path->basename, FNM_LEADING_DIR);
	else
		fnm = p_fnmatch(match->pattern, path->basename, 0);

	return (fnm == FNM_NOMATCH) ? false : true;
}

bool git_attr_rule__match(
	git_attr_rule *rule,
	const git_attr_path *path)
{
	bool matched = git_attr_fnmatch__match(&rule->match, path);

	if (rule->match.flags & GIT_ATTR_FNMATCH_NEGATIVE)
		matched = !matched;

	return matched;
}


git_attr_assignment *git_attr_rule__lookup_assignment(
	git_attr_rule *rule, const char *name)
{
	int pos;
	git_attr_name key;
	key.name = name;
	key.name_hash = git_attr_file__name_hash(name);

	pos = git_vector_bsearch(&rule->assigns, &key);

	return (pos >= 0) ? git_vector_get(&rule->assigns, pos) : NULL;
}

int git_attr_path__init(
	git_attr_path *info, const char *path, const char *base)
{
	/* build full path as best we can */
	git_buf_init(&info->full, 0);

	if (base != NULL && git_path_root(path) < 0) {
		if (git_buf_joinpath(&info->full, base, path) < 0)
			return -1;
		info->path = info->full.ptr + strlen(base);
	} else {
		if (git_buf_sets(&info->full, path) < 0)
			return -1;
		info->path = info->full.ptr;
	}

	/* remove trailing slashes */
	while (info->full.size > 0) {
		if (info->full.ptr[info->full.size - 1] != '/')
			break;
		info->full.size--;
	}
	info->full.ptr[info->full.size] = '\0';

	/* skip leading slashes in path */
	while (*info->path == '/')
		info->path++;

	/* find trailing basename component */
	info->basename = strrchr(info->path, '/');
	if (info->basename)
		info->basename++;
	if (!info->basename || !*info->basename)
		info->basename = info->path;

	info->is_dir = (int)git_path_isdir(info->full.ptr);

	return 0;
}

void git_attr_path__free(git_attr_path *info)
{
	git_buf_free(&info->full);
	info->path = NULL;
	info->basename = NULL;
}


/*
 * From gitattributes(5):
 *
 * Patterns have the following format:
 *
 * - A blank line matches no files, so it can serve as a separator for
 *   readability.
 *
 * - A line starting with # serves as a comment.
 *
 * - An optional prefix ! which negates the pattern; any matching file
 *   excluded by a previous pattern will become included again. If a negated
 *   pattern matches, this will override lower precedence patterns sources.
 *
 * - If the pattern ends with a slash, it is removed for the purpose of the
 *   following description, but it would only find a match with a directory. In
 *   other words, foo/ will match a directory foo and paths underneath it, but
 *   will not match a regular file or a symbolic link foo (this is consistent
 *   with the way how pathspec works in general in git).
 *
 * - If the pattern does not contain a slash /, git treats it as a shell glob
 *   pattern and checks for a match against the pathname without leading
 *   directories.
 *
 * - Otherwise, git treats the pattern as a shell glob suitable for consumption
 *   by fnmatch(3) with the FNM_PATHNAME flag: wildcards in the pattern will
 *   not match a / in the pathname. For example, "Documentation/\*.html" matches
 *   "Documentation/git.html" but not "Documentation/ppc/ppc.html". A leading
 *   slash matches the beginning of the pathname; for example, "/\*.c" matches
 *   "cat-file.c" but not "mozilla-sha1/sha1.c".
 */

/*
 * This will return 0 if the spec was filled out,
 * GIT_ENOTFOUND if the fnmatch does not require matching, or
 * another error code there was an actual problem.
 */
int git_attr_fnmatch__parse(
	git_attr_fnmatch *spec,
	git_pool *pool,
	const char *source,
	const char **base)
{
	const char *pattern, *scan;
	int slash_count;

	assert(spec && base && *base);

	pattern = *base;

	while (git__isspace(*pattern)) pattern++;
	if (!*pattern || *pattern == '#') {
		*base = git__next_line(pattern);
		return GIT_ENOTFOUND;
	}

	spec->flags = 0;

	if (*pattern == '[') {
		if (strncmp(pattern, "[attr]", 6) == 0) {
			spec->flags = spec->flags | GIT_ATTR_FNMATCH_MACRO;
			pattern += 6;
		}
		/* else a character range like [a-e]* which is accepted */
	}

	if (*pattern == '!') {
		spec->flags = spec->flags | GIT_ATTR_FNMATCH_NEGATIVE;
		pattern++;
	}

	slash_count = 0;
	for (scan = pattern; *scan != '\0'; ++scan) {
		/* scan until (non-escaped) white space */
		if (git__isspace(*scan) && *(scan - 1) != '\\')
			break;

		if (*scan == '/') {
			spec->flags = spec->flags | GIT_ATTR_FNMATCH_FULLPATH;
			slash_count++;
			if (pattern == scan)
				pattern++;
		}
		/* remember if we see an unescaped wildcard in pattern */
		else if (git__iswildcard(*scan) &&
			(scan == pattern || (*(scan - 1) != '\\')))
			spec->flags = spec->flags | GIT_ATTR_FNMATCH_HASWILD;
	}

	*base = scan;

	spec->length = scan - pattern;

	if (pattern[spec->length - 1] == '/') {
		spec->length--;
		spec->flags = spec->flags | GIT_ATTR_FNMATCH_DIRECTORY;
		if (--slash_count <= 0)
			spec->flags = spec->flags & ~GIT_ATTR_FNMATCH_FULLPATH;
	}

	if ((spec->flags & GIT_ATTR_FNMATCH_FULLPATH) != 0 &&
		source != NULL && git_path_root(pattern) < 0)
	{
		size_t sourcelen = strlen(source);
		/* given an unrooted fullpath match from a file inside a repo,
		 * prefix the pattern with the relative directory of the source file
		 */
		spec->pattern = git_pool_malloc(
			pool, (uint32_t)(sourcelen + spec->length + 1));
		if (spec->pattern) {
			memcpy(spec->pattern, source, sourcelen);
			memcpy(spec->pattern + sourcelen, pattern, spec->length);
			spec->length += sourcelen;
			spec->pattern[spec->length] = '\0';
		}
	} else {
		spec->pattern = git_pool_strndup(pool, pattern, spec->length);
	}

	if (!spec->pattern) {
		*base = git__next_line(pattern);
		return -1;
	} else {
		/* strip '\' that might have be used for internal whitespace */
		char *to = spec->pattern;
		for (scan = spec->pattern; *scan; to++, scan++) {
			if (*scan == '\\')
				scan++; /* skip '\' but include next char */
			if (to != scan)
				*to = *scan;
		}
		if (to != scan) {
			*to = '\0';
			spec->length = (to - spec->pattern);
		}
	}

	return 0;
}

static int sort_by_hash_and_name(const void *a_raw, const void *b_raw)
{
	const git_attr_name *a = a_raw;
	const git_attr_name *b = b_raw;

	if (b->name_hash < a->name_hash)
		return 1;
	else if (b->name_hash > a->name_hash)
		return -1;
	else
		return strcmp(b->name, a->name);
}

static void git_attr_assignment__free(git_attr_assignment *assign)
{
	/* name and value are stored in a git_pool associated with the
	 * git_attr_file, so they do not need to be freed here
	 */
	assign->name = NULL;
	assign->value = NULL;
	git__free(assign);
}

static int merge_assignments(void **old_raw, void *new_raw)
{
	git_attr_assignment **old = (git_attr_assignment **)old_raw;
	git_attr_assignment *new = (git_attr_assignment *)new_raw;

	GIT_REFCOUNT_DEC(*old, git_attr_assignment__free);
	*old = new;
	return GIT_EEXISTS;
}

int git_attr_assignment__parse(
	git_repository *repo,
	git_pool *pool,
	git_vector *assigns,
	const char **base)
{
	int error;
	const char *scan = *base;
	git_attr_assignment *assign = NULL;

	assert(assigns && !assigns->length);

	assigns->_cmp = sort_by_hash_and_name;

	while (*scan && *scan != '\n') {
		const char *name_start, *value_start;

		/* skip leading blanks */
		while (git__isspace(*scan) && *scan != '\n') scan++;

		/* allocate assign if needed */
		if (!assign) {
			assign = git__calloc(1, sizeof(git_attr_assignment));
			GITERR_CHECK_ALLOC(assign);
			GIT_REFCOUNT_INC(assign);
		}

		assign->name_hash = 5381;
		assign->value = git_attr__true;

		/* look for magic name prefixes */
		if (*scan == '-') {
			assign->value = git_attr__false;
			scan++;
		} else if (*scan == '!') {
			assign->value = git_attr__unset; /* explicit unspecified state */
			scan++;
		} else if (*scan == '#') /* comment rest of line */
			break;

		/* find the name */
		name_start = scan;
		while (*scan && !git__isspace(*scan) && *scan != '=') {
			assign->name_hash =
				((assign->name_hash << 5) + assign->name_hash) + *scan;
			scan++;
		}
		if (scan == name_start) {
			/* must have found lone prefix (" - ") or leading = ("=foo")
			 * or end of buffer -- advance until whitespace and continue
			 */
			while (*scan && !git__isspace(*scan)) scan++;
			continue;
		}

		/* allocate permanent storage for name */
		assign->name = git_pool_strndup(pool, name_start, scan - name_start);
		GITERR_CHECK_ALLOC(assign->name);

		/* if there is an equals sign, find the value */
		if (*scan == '=') {
			for (value_start = ++scan; *scan && !git__isspace(*scan); ++scan);

			/* if we found a value, allocate permanent storage for it */
			if (scan > value_start) {
				assign->value = git_pool_strndup(pool, value_start, scan - value_start);
				GITERR_CHECK_ALLOC(assign->value);
			}
		}

		/* expand macros (if given a repo with a macro cache) */
		if (repo != NULL && assign->value == git_attr__true) {
			git_attr_rule *macro =
				git_attr_cache__lookup_macro(repo, assign->name);

			if (macro != NULL) {
				unsigned int i;
				git_attr_assignment *massign;

				git_vector_foreach(&macro->assigns, i, massign) {
					GIT_REFCOUNT_INC(massign);

					error = git_vector_insert_sorted(
						assigns, massign, &merge_assignments);
					if (error < 0 && error != GIT_EEXISTS)
						return error;
				}
			}
		}

		/* insert allocated assign into vector */
		error = git_vector_insert_sorted(assigns, assign, &merge_assignments);
		if (error < 0 && error != GIT_EEXISTS)
			return error;

		/* clear assign since it is now "owned" by the vector */
		assign = NULL;
	}

	if (assign != NULL)
		git_attr_assignment__free(assign);

	*base = git__next_line(scan);

	return (assigns->length == 0) ? GIT_ENOTFOUND : 0;
}

static void git_attr_rule__clear(git_attr_rule *rule)
{
	unsigned int i;
	git_attr_assignment *assign;

	if (!rule)
		return;

	if (!(rule->match.flags & GIT_ATTR_FNMATCH_IGNORE)) {
		git_vector_foreach(&rule->assigns, i, assign)
			GIT_REFCOUNT_DEC(assign, git_attr_assignment__free);
		git_vector_free(&rule->assigns);
	}

	/* match.pattern is stored in a git_pool, so no need to free */
	rule->match.pattern = NULL;
	rule->match.length = 0;
}

void git_attr_rule__free(git_attr_rule *rule)
{
	git_attr_rule__clear(rule);
	git__free(rule);
}

