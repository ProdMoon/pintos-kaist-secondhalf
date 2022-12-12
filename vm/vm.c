/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include <string.h>
#include "userprog/process.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */

	/* Init locks for global. */
	lock_init (&swap_lock);
	lock_init (&frame_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check whether the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* Create the page, fetch the initializer according to the VM type,
		   and then create "uninit" page struct by calling uninit_new. */
		struct page *page = calloc (1, sizeof(struct page));
		switch (VM_TYPE(type)) {
			case VM_ANON:
				uninit_new (page, upage, init, type, aux, anon_initializer);
				break;
			case VM_FILE:
				uninit_new (page, upage, init, type, aux, file_backed_initializer);
				break;
			default:
				printf("vm_alloc_page_with_initializer: Unexpected page type.\n");
				goto err;
		}

		/* Set some initial stuff. */
		page->writable = writable;
		page->page_cnt = 0;
		page->sec_no = -1;

		/* Insert the page into the spt. */
		if (!spt_insert_page (spt, page))
			goto err;

		/* Claim immediately if the page is the first stack page. */
		if (VM_IS_STACK(type))
			return vm_do_claim_page (page);

		return true;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	/* TODO: Fill this function. */
	struct hash *pages = &spt->pages;
	struct hash_elem *e;
	struct page sample;

	sample.va = va;
	e = hash_find (pages, &sample.hash_elem);

	return e != NULL ? hash_entry (e, struct page, hash_elem) : NULL;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED, struct page *page UNUSED) {
	return hash_insert (&spt->pages, &page->hash_elem) == NULL ? true : false;
}

/* Remove PAGE from spt. */
void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	hash_delete (&spt->pages, &page->hash_elem);
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	lock_acquire (&frame_lock);
	struct list_elem *e = list_pop_front (thread_current()->spt.frames);
	lock_release (&frame_lock);

	struct frame *victim = list_entry (e, struct frame, elem);

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	/* Swap out the victim and return the evicted frame. */
	struct frame *victim = vm_get_victim ();
	swap_out (victim->page);

	/* Clear page from pml4. */
	pml4_clear_page (thread_current()->pml4, victim->page->va);
	
	/* Unlink. */
	victim->page->frame = NULL;
	victim->page = NULL;

	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = calloc (1, sizeof(struct frame));

	if ((frame->kva = palloc_get_page (PAL_USER | PAL_ZERO)) == NULL) {
		/* No available page. Evict the page and get frame. */
		free (frame);
		if ((frame = vm_evict_frame ()) == NULL)
			PANIC ("BOTH MEMORY AND SWAP ARE FULL.");
	}

	/* Push back to frame table. */
	lock_acquire (&frame_lock);
	list_push_back (thread_current()->spt.frames, &frame->elem);
	lock_release (&frame_lock);
	
	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	void *va = pg_round_down (addr);

	ASSERT (VM_STACKSIZE_LIMIT <= va);

	while (spt_find_page (spt, va) == NULL) {
		vm_alloc_page (VM_ANON | VM_MARKER_0, va, true);
		va += PGSIZE;
	}
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page;

	/* Grow the stack if it's a valid stack growth case. */
	void *rsp = f->rsp;
	if (rsp-8 == addr ||
		((rsp <= addr) && (VM_STACKSIZE_LIMIT <= addr) && (addr < USER_STACK))) {
		
		/* Check that the page is evicted stack. */
		if (page = spt_find_page (spt, pg_round_down(addr)))
			vm_do_claim_page (page);
		else
			vm_stack_growth (addr);
			
		return true;
	}

	/* Check the address entry's existance in the SPT. */
	void *va = pg_round_down(addr);
	if ((page = spt_find_page (spt, va)) == NULL)
		return false;	/* Real Page Fault */

	/* Check that user tries to write on the unwritable. (i.e. code segment) */
	if (user && write && !(page->writable))
		return false;

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = calloc (1, sizeof(struct page));
	page->va = va;

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	ASSERT (page != NULL);

	struct frame *frame = vm_get_frame ();
	struct supplemental_page_table *spt = &thread_current()->spt;

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* Insert page table entry to map page's VA to frame's PA. */
	pml4_set_page (thread_current()->pml4, page->va, frame->kva, page->writable);

	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init (&spt->pages, page_hash, page_less, NULL);
	list_init (&spt->mmap_list);
}

/* Copy the swap and return the sec_no of copied. */
disk_sector_t
swap_copy (struct page *srcp) {
	struct disk *swap_disk = thread_current()->spt.swap_disk;
	struct list *swap_free = thread_current()->spt.swap_free;
	struct list *swap_used = thread_current()->spt.swap_used;

	lock_acquire (&swap_lock);

	/* Find the swap from used list. */
	struct list_elem *e;
	struct swap *src_swap;
	for (e = list_begin (swap_used); e != list_end (swap_used); e = list_next (e)) {
		src_swap = list_entry (e, struct swap, elem);
		if (src_swap->sec_no == srcp->sec_no)
			break;
	}
	ASSERT (e != list_end (swap_used));

	/* Get a free swap and push into used list. */
	struct swap *dst_swap = list_entry (list_pop_front (swap_free), struct swap, elem);
	list_push_front (swap_used, &dst_swap->elem);

	lock_release (&swap_lock);

	/* Make a new copy.
	   Iterate for 8 times (PGSIZE / DISK_SECTOR_SIZE). */
	char buf[DISK_SECTOR_SIZE];
	disk_sector_t dst_no = dst_swap->sec_no;
	disk_sector_t src_no = src_swap->sec_no;

	for (int i = 0; i < 8; i++) {
		disk_read (swap_disk, src_no, buf);
		disk_write (swap_disk, dst_no, buf);

		/* Advance. */
		dst_no++;
		src_no++;
	}

	return dst_no - 8;
}

/* Read from the src swap and write into dstp. */
void
swap_read_and_paste (struct page *dstp, struct page *srcp) {
	struct disk *swap_disk = thread_current()->spt.swap_disk;
	struct list *swap_free = thread_current()->spt.swap_free;
	struct list *swap_used = thread_current()->spt.swap_used;

	lock_acquire (&swap_lock);

	/* Find the swap from used list. */
	struct list_elem *e;
	struct swap *src_swap;
	for (e = list_begin (swap_used); e != list_end (swap_used); e = list_next (e)) {
		src_swap = list_entry (e, struct swap, elem);
		if (src_swap->sec_no == srcp->sec_no)
			break;
	}
	ASSERT (e != list_end (swap_used));

	lock_release (&swap_lock);

	/* Read from swap and paste into dstp address.
	   Iterate for 8 times (PGSIZE / DISK_SECTOR_SIZE). */
	char buf[DISK_SECTOR_SIZE];
	void *addr = dstp->va;
	disk_sector_t src_no = src_swap->sec_no;

	while (addr < dstp->va + PGSIZE) {
		disk_read (swap_disk, src_no, buf);
		memcpy (addr, buf, DISK_SECTOR_SIZE);

		/* Advance. */
		src_no++;
		addr += DISK_SECTOR_SIZE;
	}
}

/* Copy supplemental page table from src to dst
   THIS function is called in CHILD process's context. */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {

	bool success = false;

	/* Copy globals. */
	dst->frames = src->frames;
	dst->swap_free = src->swap_free;
	dst->swap_used = src->swap_used;
	dst->swap_disk = src->swap_disk;

	/* Duplicate hash table. */
	struct hash_iterator i;
	struct hash *h = &src->pages;

	hash_first (&i, h);
	while (hash_next (&i)) {
		struct page *srcp = hash_entry (hash_cur (&i), struct page, hash_elem);
		struct page *dstp = NULL;

		/* Stack page is special. */
		if (VM_IS_STACK(srcp->uninit.type)) {
			if (vm_alloc_page (srcp->uninit.type, srcp->va, srcp->writable)) {
				dstp = spt_find_page(dst, srcp->va);

				/* If stack is in swap, read it from the swap. */
				if (srcp->sec_no > -1) {
					swap_read_and_paste (dstp, srcp);
				}
				else {
					memcpy (dstp->frame->kva, srcp->frame->kva, PGSIZE);
					ASSERT (srcp->sec_no == -1);
					dstp->sec_no = -1;
				}
			}
			else {
				printf("supplemental_page_table_copy: stack의 alloc 실패.\n");
				goto done;
			}
			continue;
		}

		/* Duplicate aux. */
		struct aux *dst_aux = calloc (1, sizeof(struct aux));
		struct aux *src_aux = srcp->uninit.aux;
		dst_aux->file = VM_TYPE(srcp->uninit.type) == VM_FILE ?
			file_duplicate (src_aux->file) : thread_current()->running_executable;
		dst_aux->ofs = src_aux->ofs;
		dst_aux->page_read_bytes = src_aux->page_read_bytes;
		dst_aux->page_zero_bytes = src_aux->page_zero_bytes;
		
		/* Make new uninit page.
		   This will insert new page to dst's SPT automatically. */
		if (!vm_alloc_page_with_initializer (srcp->uninit.type, srcp->va,
			srcp->writable, srcp->uninit.init, dst_aux))
			goto done;

		dstp = spt_find_page(dst, srcp->va);

		/* Copy stuff for mmap pages. */
		dstp->page_cnt = srcp->page_cnt;

		/* If page is in swap, duplicate it and continue. */
		if (srcp->sec_no > -1) {
			dstp->sec_no = swap_copy (srcp);
			continue;
		}
		
		/* If src page is already initialized, do claim immediately. */
		switch (VM_TYPE(srcp->operations->type)) {
			case VM_UNINIT:
				break;
			case VM_ANON:
			case VM_FILE:
				if (vm_do_claim_page (dstp)) {
					memcpy (dstp->frame->kva, srcp->frame->kva, PGSIZE);
				} else {
					printf("supplemental_page_table_copy: vm_do_claim_page is failed.\n");
					goto done;
				}
				break;
			default:
				printf("supplemental_page_table_copy: Unexpected page type.\n");
				goto done;
		}
	}

	/* Duplicate mmap list. */
	struct list_elem *e;
	struct list *src_list = &src->mmap_list;
	struct list *dst_list = &dst->mmap_list;
	for (e = list_begin (src_list); e != list_end (src_list); e = list_next (e)) {
		struct page *srcp = list_entry (e, struct page, mmap_elem);
		struct page *dstp = spt_find_page (dst, srcp->va);
		ASSERT (srcp->page_cnt != 0);
		list_push_back(dst_list, &dstp->mmap_elem);
	}

	success = true;
done:
	return success;
}

void
hash_destroy_helper (struct hash_elem *e, void *aux) {
	struct page *p = hash_entry (e, struct page, hash_elem);
	vm_dealloc_page (p);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */

	/* Unmap file pages. Writeback will also operated here. */
	struct list *mmap_list = &spt->mmap_list;
	while ( ! list_empty (mmap_list)) {
		struct page *page = list_entry (list_pop_front (mmap_list), struct page, mmap_elem);
		ASSERT (page->page_cnt != 0);
		do_munmap (page->va);
	}

	/* Destroy and re-init hash table. */
	struct hash *h = &spt->pages;

	hash_destroy (h, hash_destroy_helper);
	hash_init (h, page_hash, page_less, NULL);
}

/* Returns a hash value for page p. */
unsigned
page_hash (const struct hash_elem *p_, void *aux UNUSED) {
	const struct page *p = hash_entry (p_, struct page, hash_elem);
	return hash_bytes (&p->va, sizeof p->va);
}

/* Returns true if page a precedes page b.
   페이지 a가 b보다 주소가 앞선다면 true를 리턴함. */
bool
page_less (const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED) {
	const struct page *a = hash_entry (a_, struct page, hash_elem);
	const struct page *b = hash_entry (b_, struct page, hash_elem);

	return a->va < b->va;
}