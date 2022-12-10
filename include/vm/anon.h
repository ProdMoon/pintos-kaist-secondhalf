#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
struct page;
enum vm_type;

typedef bool vm_initializer (struct page *, void *aux);

struct anon_page {
	/* Need to remember uninit page elements
       because we will reuse them for page copy. */
	vm_initializer *init;
	enum vm_type type;
	void *aux;
	bool (*page_initializer) (struct page *, enum vm_type, void *kva);
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif
