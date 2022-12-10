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
	/* TODO: Your code goes here. */
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
		/* TODO: Create the page, fetch the initializer according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		struct page *page = calloc (1, sizeof(struct page));
		switch (VM_TYPE(type)) {
			case VM_ANON:
				uninit_new (page, upage, init, type, aux, anon_initializer);
				break;
			case VM_FILE:
				uninit_new (page, upage, init, type, aux, file_backed_initializer);
				break;
			default:
				printf("vm_alloc_page_with_initializer: 잉? 페이지 타입이 이상해요.\n");
				goto err;
		}

		page->writable = writable;

		/* TODO: Insert the page into the spt. */
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

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	/* TODO: Fill this function. */
	struct frame *frame = calloc (1, sizeof(struct frame));

	if ((frame->kva = palloc_get_page (PAL_USER | PAL_ZERO)) == NULL) {
		// PANIC ("TODO : if user pool memory is full, need to evict the frame.");
		thread_current()->exit_code = -1;
		thread_exit();
	}
	
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
		vm_stack_growth (addr);
		return true;
	}

	/* Check the address entry's existance in the SPT. */
	void *va = pg_round_down(addr);
	if ((page = spt_find_page (spt, va)) == NULL)
		return false;	/* Real Page Fault */

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
	/* TODO: Fill this function */
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

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	pml4_set_page (thread_current()->pml4, page->va, frame->kva, page->writable);

	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init (&spt->pages, page_hash, page_less, NULL);
	lock_init (&spt->spt_lock);
}

/* Copy supplemental page table from src to dst
   THIS function is called in CHILD process's context. */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {

	bool success = false;

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
				memcpy (dstp->frame->kva, srcp->frame->kva, PGSIZE);
			} else {
				printf("supplemental_page_table_copy: stack의 alloc 실패.\n");
				goto done;
			}
			continue;
		}

		/* Duplicate aux. */
		struct aux *dst_aux = calloc (1, sizeof(struct aux));
		struct aux *src_aux = srcp->uninit.aux;
		dst_aux->file = thread_current()->running_executable;
		dst_aux->ofs = src_aux->ofs;
		dst_aux->page_read_bytes = src_aux->page_read_bytes;
		dst_aux->page_zero_bytes = src_aux->page_zero_bytes;

		/* Make new uninit page.
		   This will insert new page to dst's SPT automatically. */
		if (!vm_alloc_page_with_initializer (srcp->uninit.type, srcp->va,
			srcp->writable, srcp->uninit.init, dst_aux))
			goto done;

		/* If src page is already initialized, do claim immediately. */
		switch (VM_TYPE(srcp->operations->type)) {
			case VM_UNINIT:
				break;
			case VM_ANON:
			case VM_FILE:
				dstp = spt_find_page(dst, srcp->va);
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
	success = true;
done:
	return success;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	struct hash_iterator i;
	struct hash *h = &spt->pages;

	hash_first (&i, h);
	while (hash_next (&i)) {
		struct page *p = hash_entry (hash_cur (&i), struct page, hash_elem);
		destroy (p);
	}
	hash_destroy (h, NULL);
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