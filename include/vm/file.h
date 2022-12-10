#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;

typedef bool vm_initializer (struct page *, void *aux);

struct file_page {
	/* Need to remember uninit page elements
       because we will reuse them for page copy. */
	vm_initializer *init;
	enum vm_type type;
	void *aux;
	bool (*page_initializer) (struct page *, enum vm_type, void *kva);
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif
