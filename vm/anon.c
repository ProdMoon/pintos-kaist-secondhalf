/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/malloc.h"
#include <string.h>

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	struct supplemental_page_table *spt = &thread_current()->spt;

	/* Init global lists for frames and swaps. */
	spt->frames = calloc (1, sizeof(struct list));
	spt->swap_free = calloc (1, sizeof(struct list));
	spt->swap_used = calloc (1, sizeof(struct list));
	list_init (spt->frames);
	list_init (spt->swap_free);
	list_init (spt->swap_used);

	/* Set up the swap_disk. */
	swap_disk = disk_get (1, 1);
	spt->swap_disk = swap_disk;

	int available_pages = (disk_size (swap_disk) * DISK_SECTOR_SIZE) / PGSIZE;
	disk_sector_t sec_no = 0;
	while (available_pages > 0) {
		struct swap *swap = calloc (1, sizeof(struct swap));
		swap->sec_no = sec_no;
		list_push_back (spt->swap_free, &swap->elem);

		/* Advance. */
		available_pages -= 1;
		sec_no += (PGSIZE / DISK_SECTOR_SIZE);
	}
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;
	
	/* Remember uninit page elements for page copy. */
	anon_page->init = page->uninit.init;
	anon_page->type = page->uninit.type;
	anon_page->aux = page->uninit.aux;
	anon_page->page_initializer = page->uninit.page_initializer;

	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	struct disk *swap_disk = thread_current()->spt.swap_disk;
	struct list *swap_free = thread_current()->spt.swap_free;
	struct list *swap_used = thread_current()->spt.swap_used;

	/* Find the swap from used list and push into free list. */
	struct list_elem *e;
	struct swap *swap;
	lock_acquire (&swap_lock);

	for (e = list_begin (swap_used); e != list_end (swap_used); e = list_next (e)) {
		swap = list_entry (e, struct swap, elem);
		if (swap->sec_no == page->sec_no)
			break;
	}
	ASSERT (e != list_end (swap_used));
	list_remove (e);
	list_push_front (swap_free, &swap->elem);

	lock_release (&swap_lock);

	/* Unlink page from swap. */
	page->sec_no = -1;

	/* Copy from disk to va.
	   Iterate for 8 times (PGSIZE / DISK_SECTOR_SIZE). */
	char buf[DISK_SECTOR_SIZE];
	void *addr = page->va;
	disk_sector_t sec_no = swap->sec_no;

	while (addr < page->va + PGSIZE) {
		disk_read (swap_disk, sec_no, buf);
		memcpy (addr, buf, DISK_SECTOR_SIZE);

		/* Advance. */
		sec_no++;
		addr += DISK_SECTOR_SIZE;
	}

	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	struct disk *swap_disk = thread_current()->spt.swap_disk;
	struct list *swap_free = thread_current()->spt.swap_free;
	struct list *swap_used = thread_current()->spt.swap_used;

	/* Get a free swap and push into used list. */
	lock_acquire (&swap_lock);
	struct swap *swap = list_entry (list_pop_front (swap_free), struct swap, elem);
	list_push_front (swap_used, &swap->elem);
	lock_release (&swap_lock);

	/* Make a link for identification. */
	page->sec_no = swap->sec_no;

	/* Copy and write each sector from va to disk.
	   Iterate for 8 times (PGSIZE / DISK_SECTOR_SIZE). */
	char buf[DISK_SECTOR_SIZE];
	void *addr = page->va;
	disk_sector_t sec_no = swap->sec_no;

	while (addr < page->va + PGSIZE) {
		memcpy (buf, addr, DISK_SECTOR_SIZE);
		disk_write (swap_disk, sec_no, buf);

		/* Advance. */
		sec_no++;
		addr += DISK_SECTOR_SIZE;
	}

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	if (page->sec_no > -1) {
		/* Remove page from swap disk. */
		struct disk *swap_disk = thread_current()->spt.swap_disk;
		struct list *swap_free = thread_current()->spt.swap_free;
		struct list *swap_used = thread_current()->spt.swap_used;
		struct list_elem *e;
		struct swap *swap;
		
		lock_acquire (&swap_lock);
		for (e = list_begin (swap_used); e != list_end (swap_used); e = list_next (e)) {
			swap = list_entry (e, struct swap, elem);
			if (swap->sec_no == page->sec_no)
				break;
		}
		list_remove (e);
		list_push_front (swap_free, &swap->elem);
		lock_release (&swap_lock);
		
		page->sec_no = -1;
	}
	else {
		/* Remove the frame from the frame list. */
		struct list_elem *e;
		struct list *frames = thread_current()->spt.frames;
		struct frame *f;

		lock_acquire (&frame_lock);
		for (e = list_begin (frames); e != list_end (frames); e = list_next (e)) {
			f = list_entry (e, struct frame, elem);
			if (f->kva == page->frame->kva)
				break;
		}
		list_remove (e);
		lock_release (&frame_lock);

		/* Free the struct frame. */
		ASSERT (page->frame->kva == f->kva);
		free (page->frame);
		page->frame = NULL;
	}

	/* Free the aux. */
	free (anon_page->aux);
	anon_page->aux = NULL;
}
