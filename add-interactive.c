#include "cache.h"
#include "add-interactive.h"
#include "diffcore.h"
#include "revision.h"
#include "refs.h"

struct item {
	const char *name;
};

struct list_options {
	const char *header;
	void (*print_item)(int i, struct item *item, void *print_item_data);
	void *print_item_data;
};

static void list(struct item **list, size_t nr, struct list_options *opts)
{
	int i;

	if (!nr)
		return;

	if (opts->header)
		printf("%s\n", opts->header);

	for (i = 0; i < nr; i++) {
		opts->print_item(i, list[i], opts->print_item_data);
		putchar('\n');
	}
}

struct adddel {
	uintmax_t add, del;
	unsigned seen:1, binary:1;
};

struct file_list {
	struct file_item {
		struct item item;
		struct adddel index, worktree;
	} **file;
	size_t nr, alloc;
};

static void add_file_item(struct file_list *list, const char *name)
{
	struct file_item *item;

	FLEXPTR_ALLOC_STR(item, item.name, name);

	ALLOC_GROW(list->file, list->nr + 1, list->alloc);
	list->file[list->nr++] = item;
}

static void reset_file_list(struct file_list *list)
{
	size_t i;

	for (i = 0; i < list->nr; i++)
		free(list->file[i]);
	list->nr = 0;
}

static void release_file_list(struct file_list *list)
{
	reset_file_list(list);
	FREE_AND_NULL(list->file);
	list->alloc = 0;
}

static int file_item_cmp(const void *a, const void *b)
{
	const struct file_item * const *f1 = a;
	const struct file_item * const *f2 = b;

	return strcmp((*f1)->item.name, (*f2)->item.name);
}

struct pathname_entry {
	struct hashmap_entry ent;
	size_t index;
	char pathname[FLEX_ARRAY];
};

static int pathname_entry_cmp(const void *unused_cmp_data,
			      const void *entry, const void *entry_or_key,
			      const void *pathname)
{
	const struct pathname_entry *e1 = entry, *e2 = entry_or_key;

	return strcmp(e1->pathname,
		      pathname ? (const char *)pathname : e2->pathname);
}

struct collection_status {
	enum { FROM_WORKTREE = 0, FROM_INDEX = 1 } phase;

	const char *reference;

	struct file_list *list;
	struct hashmap file_map;
};

static void collect_changes_cb(struct diff_queue_struct *q,
			       struct diff_options *options,
			       void *data)
{
	struct collection_status *s = data;
	struct diffstat_t stat = { 0 };
	int i;

	if (!q->nr)
		return;

	compute_diffstat(options, &stat, q);

	for (i = 0; i < stat.nr; i++) {
		const char *name = stat.files[i]->name;
		int hash = strhash(name);
		struct pathname_entry *entry;
		size_t file_index;
		struct file_item *file;
		struct adddel *adddel;

		entry = hashmap_get_from_hash(&s->file_map, hash, name);
		if (entry)
			file_index = entry->index;
		else {
			FLEX_ALLOC_STR(entry, pathname, name);
			hashmap_entry_init(&entry->ent, hash);
			entry->index = file_index = s->list->nr;
			hashmap_add(&s->file_map, &entry->ent);

			add_file_item(s->list, name);
		}
		file = s->list->file[file_index];

		adddel = s->phase == FROM_INDEX ? &file->index : &file->worktree;
		adddel->seen = 1;
		adddel->add = stat.files[i]->added;
		adddel->del = stat.files[i]->deleted;
		if (stat.files[i]->is_binary)
			adddel->binary = 1;
	}
	free_diffstat_info(&stat);
}

static int get_modified_files(struct repository *r, struct file_list *list,
			      const struct pathspec *ps)
{
	struct object_id head_oid;
	int is_initial = !resolve_ref_unsafe("HEAD", RESOLVE_REF_READING,
					     &head_oid, NULL);
	struct collection_status s = { FROM_WORKTREE };

	if (repo_read_index_preload(r, ps, 0) < 0)
		return error(_("could not read index"));

	s.list = list;
	hashmap_init(&s.file_map, pathname_entry_cmp, NULL, 0);

	for (s.phase = FROM_WORKTREE; s.phase <= FROM_INDEX; s.phase++) {
		struct rev_info rev;
		struct setup_revision_opt opt = { 0 };

		opt.def = is_initial ?
			empty_tree_oid_hex() : oid_to_hex(&head_oid);

		init_revisions(&rev, NULL);
		setup_revisions(0, NULL, &rev, &opt);

		rev.diffopt.output_format = DIFF_FORMAT_CALLBACK;
		rev.diffopt.format_callback = collect_changes_cb;
		rev.diffopt.format_callback_data = &s;

		if (ps)
			copy_pathspec(&rev.prune_data, ps);

		if (s.phase == FROM_INDEX)
			run_diff_index(&rev, 1);
		else {
			rev.diffopt.flags.ignore_dirty_submodules = 1;
			run_diff_files(&rev, 0);
		}
	}
	hashmap_free(&s.file_map, 1);

	/* While the diffs are ordered already, we ran *two* diffs... */
	QSORT(list->file, list->nr, file_item_cmp);

	return 0;
}

static void populate_wi_changes(struct strbuf *buf,
				struct adddel *ad, const char *no_changes)
{
	if (ad->binary)
		strbuf_addstr(buf, _("binary"));
	else if (ad->seen)
		strbuf_addf(buf, "+%"PRIuMAX"/-%"PRIuMAX,
			    (uintmax_t)ad->add, (uintmax_t)ad->del);
	else
		strbuf_addstr(buf, no_changes);
}

struct print_file_item_data {
	const char *modified_fmt;
	struct strbuf buf, index, worktree;
};

static void print_file_item(int i, struct item *item,
			    void *print_file_item_data)
{
	struct file_item *c = (struct file_item *)item;
	struct print_file_item_data *d = print_file_item_data;

	strbuf_reset(&d->index);
	strbuf_reset(&d->worktree);
	strbuf_reset(&d->buf);

	populate_wi_changes(&d->worktree, &c->worktree, _("nothing"));
	populate_wi_changes(&d->index, &c->index, _("unchanged"));
	strbuf_addf(&d->buf, d->modified_fmt,
		    d->index.buf, d->worktree.buf, item->name);

	printf(" %2d: %s", i + 1, d->buf.buf);
}

static int run_status(struct repository *r, const struct pathspec *ps,
		      struct file_list *files, struct list_options *opts)
{
	reset_file_list(files);

	if (get_modified_files(r, files, ps) < 0)
		return -1;

	if (files->nr)
		list((struct item **)files->file, files->nr, opts);
	putchar('\n');

	return 0;
}

int run_add_i(struct repository *r, const struct pathspec *ps)
{
	struct print_file_item_data print_file_item_data = {
		"%12s %12s %s", STRBUF_INIT, STRBUF_INIT, STRBUF_INIT
	};
	struct list_options opts = {
		NULL, print_file_item, &print_file_item_data
	};
	struct strbuf header = STRBUF_INIT;
	struct file_list files = { NULL };
	int res = 0;

	strbuf_addstr(&header, "      ");
	strbuf_addf(&header, print_file_item_data.modified_fmt,
		    _("staged"), _("unstaged"), _("path"));
	opts.header = header.buf;

	repo_refresh_and_write_index(r, REFRESH_QUIET, 1);
	if (run_status(r, ps, &files, &opts) < 0)
		res = -1;

	release_file_list(&files);
	strbuf_release(&print_file_item_data.buf);
	strbuf_release(&print_file_item_data.index);
	strbuf_release(&print_file_item_data.worktree);
	strbuf_release(&header);

	return res;
}
