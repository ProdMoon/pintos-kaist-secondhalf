#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "threads/synch.h"
#include "intrinsic.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "threads/malloc.h"
#include "threads/palloc.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	lock_init(&filesys_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	syscall_handler_func *handler;

	handler = syscall_handlers[f->R.rax];
	if (handler) {
		handler(f);		// handle system call.
	}
	else {
		intr_dump_frame (f);
		PANIC ("Unexpected system call");
	}
		
}

/* This assertion should be used when
   the argument sent by user is POINTER TYPE */
void assert_valid_address(struct intr_frame *f, void *uaddr) {
	/* invalid if uaddr is null or kernel virtual address
	   or unmapped to physical address */
	if (!uaddr || is_kernel_vaddr(uaddr) ||
		!pml4_get_page(thread_current()->pml4, uaddr)) {

#ifdef VM
		/* Try handle fault. */
		if (vm_try_handle_fault (f, uaddr, NULL, NULL, NULL))
			return;
#endif
		thread_current()->exit_code = -1;
		thread_exit();
	}

}

struct child *find_child (struct list *child_list, int tid) {
	struct list_elem *e = list_head(child_list);

	while ((e = list_next(e)) != list_end(child_list)) {
		struct child *child = list_entry(e, struct child, elem);
		if (tid == child->tid) {
			return child;
		}
	}

	return NULL;
}

/*
 * void
 * halt (void)
 */
void halt_syscall_handler (struct intr_frame *f) {
	power_off();
} 

/*
 * void
 * exit (int status)
 */
void exit_syscall_handler (struct intr_frame *f) {
	thread_current()->exit_code = f->R.rdi;
	thread_exit();
} 

/*
 * pid_t
 * fork (const char *thread_name)
 */
void fork_syscall_handler (struct intr_frame *f) {
	struct thread *curr = thread_current();
	struct list *child_list = &curr->child_list;
	int tid = process_fork(f->R.rdi, f);

	if (tid > 0) {	// if valid tid,
		struct child *child = find_child(child_list, tid);
		if (child != NULL) {
			f->R.rax = tid;
			return;
		}
	}

	/* including thread_create() fail, do_fork() fail */
	f->R.rax = TID_ERROR;
} 

/*
 * int 
 * exec (const char *file)
 */
void exec_syscall_handler (struct intr_frame *f) {
	assert_valid_address(f, f->R.rdi);

	char *arg, *arg_copy;
	arg = f->R.rdi;

	/* Make a copy of argument */
	arg_copy = palloc_get_page (0);
	if (arg_copy != NULL) {
		strlcpy(arg_copy, arg, PGSIZE);

		/* Never return if succeed */
		int result = process_exec(arg_copy);
	}
	/* Reaches here when failure. */
	thread_current()->exit_code = -1;
	thread_exit();
} 

/*
 * int 
 * wait (pid_t pid)
 */
void wait_syscall_handler (struct intr_frame *f) {
	int tid = f->R.rdi;

	f->R.rax = process_wait(tid);
} 

/*
 * bool
 * create (const char *file, unsigned initial_size)
 */
void create_syscall_handler (struct intr_frame *f) {
	assert_valid_address(f, f->R.rdi);

	char *name = f->R.rdi;
	unsigned initial_size = f->R.rsi;
	lock_acquire(&filesys_lock);
	bool success = filesys_create(name, initial_size);
	lock_release(&filesys_lock);

	f->R.rax = success;
} 

/*
 * bool
 * remove (const char *file)
 */
void remove_syscall_handler (struct intr_frame *f) {
	assert_valid_address(f, f->R.rdi);
	
	char *name = f->R.rdi;
	lock_acquire(&filesys_lock);
	bool success = filesys_remove(name);
	lock_release(&filesys_lock);

	f->R.rax = success;
} 

/*
 * int
 * open (const char *file)
 */
void open_syscall_handler (struct intr_frame *f) {
	assert_valid_address(f, f->R.rdi);

	char *file_name = f->R.rdi;
	uintptr_t *fd_table = thread_current()->fd_table;
	uintptr_t *file_opened;

	/* check file_name and fd_count */
	if (!file_name || thread_current()->fd_count >= FD_MAX) {
		f->R.rax = -1;
		return;
	}
	lock_acquire(&filesys_lock);
	file_opened = filesys_open(file_name);
	lock_release(&filesys_lock);

	/* check the file has been successfully opened */
	if (!file_opened) {
		f->R.rax = -1;
		return;
	}
	
	/* find empty entry */
	for (char idx = 2; idx < FD_MAX; idx++) {
		if (!fd_table[idx]) {	// 2번부터 빈 공간 선형 탐색
			fd_table[idx] = file_opened;
			f->R.rax = idx;		// 유저에게 fd를 넘겨주는 순간
			thread_current()->fd_count += 1;
			return;
		}
	}

	/* should not be reached */
	NOT_REACHED();
} 

/* 
 * int
 * filesize (int fd)
 */
void filesize_syscall_handler (struct intr_frame *f) {
	int fd = f->R.rdi;
	intptr_t *fd_table = thread_current()->fd_table;

	/* fd validity check */
	if (fd < 2 || fd >= FD_MAX || fd_table[fd] == NULL) {
		f->R.rax = -1;
		return;
	}

	lock_acquire(&filesys_lock);
	int32_t f_len = file_length(fd_table[fd]);
	lock_release(&filesys_lock);

	f->R.rax = f_len;
} 

/* 
 * int
 * read (int fd, void *buffer, unsigned size)
 */
void read_syscall_handler (struct intr_frame *f) {
	assert_valid_address(f, f->R.rsi);

	int fd = f->R.rdi;
	char *buffer = f->R.rsi;
	unsigned size = f->R.rdx;
	intptr_t *fd_table = thread_current()->fd_table;
	int32_t read_bytes = 0;
	int i = 0;

	if (fd == STDIN_FILENO) {
		while(size-- > 0) {
			buffer[i++] = (char) input_getc();
			read_bytes++;
		}
	}
	else {
		/* fd validity check */
		if (fd < 2 || fd >= FD_MAX || fd_table[fd] == NULL) {
			f->R.rax = -1;
			return;
		}
		lock_acquire(&filesys_lock);
		read_bytes = file_read(fd_table[fd], buffer, size);
		lock_release(&filesys_lock);
	}
	f->R.rax = read_bytes;
} 

/* 
 * int
 * write (int fd, const void *buffer, unsigned size)
 */
void write_syscall_handler (struct intr_frame *f) {
	assert_valid_address(f, f->R.rsi);

	int fd = f->R.rdi;
	const void *buffer = f->R.rsi;
	unsigned size = f->R.rdx;
	intptr_t *fd_table = thread_current()->fd_table;
	int32_t written_bytes = 0;

	if (fd == STDOUT_FILENO) {
		putbuf(buffer, size);
		written_bytes = size;
	}
	else {
		/* fd validity check */
		if (fd < 2 || fd >= FD_MAX || fd_table[fd] == NULL) {
			f->R.rax = -1;
			return;
		}
		lock_acquire(&filesys_lock);
		written_bytes = file_write(fd_table[fd], buffer, size);
		lock_release(&filesys_lock);
	}
	f->R.rax = written_bytes;
} 

/* 
 * void
 * seek (int fd, unsigned position)
 */
void seek_syscall_handler (struct intr_frame *f) {
	int fd = f->R.rdi;
	uintptr_t *fd_table = thread_current()->fd_table;
	unsigned new_pos = f->R.rsi;

	/* fd validity check */
	if (fd < 2 || fd >= FD_MAX || fd_table[fd] == NULL)
		
	/* new position validity check */
	if (new_pos < 0)
		return;
	
	lock_acquire(&filesys_lock);
	file_seek(fd_table[fd], new_pos);
	lock_release(&filesys_lock);
}

/* 
 * unsigned
 * tell (int fd)
 */
void tell_syscall_handler (struct intr_frame *f) {
	int fd = f->R.rdi;
	uintptr_t *fd_table = thread_current()->fd_table;
	unsigned position = 0;

	/* fd validity check */
	if (fd < 2 || fd >= FD_MAX || fd_table[fd] == NULL) {
		f->R.rax = -1;
		return;
	}

	lock_acquire(&filesys_lock);
	position = file_tell(fd_table[fd]);
	lock_release(&filesys_lock);

	f->R.rax = position;
} 

/* 
 * void
 * close (int fd)
 */
void close_syscall_handler (struct intr_frame *f) {
	int fd = f->R.rdi;
	uintptr_t *fd_table = thread_current()->fd_table;

	/* fd validity check */
	if (fd < 2 || fd >= FD_MAX || fd_table[fd] == NULL)
		return;	// silently fail...

	lock_acquire(&filesys_lock);
	file_close(fd_table[fd]);
	lock_release(&filesys_lock);

	fd_table[fd] = NULL;
	ASSERT(thread_current()->fd_count > 2);
	thread_current()->fd_count -= 1;
} 

/* 
 * int
 * dup2 (int oldfd, int newfd)
 */
void dup2_syscall_handler (struct intr_frame *f) {

}  

/* 
 * void *
 * mmap (void *addr, size_t length, int writable, int fd, off_t offset)
 */
void mmap_syscall_handler (struct intr_frame *f) {

}  

/* 
 * void
 * munmap (void *addr)
 */
void munmap_syscall_handler (struct intr_frame *f) {

}  

/* 
 * bool
 * chdir (const char *dir)
 */
void chdir_syscall_handler (struct intr_frame *f) {

}  

/* 
 * bool
 * mkdir (const char *dir)
 */
void mkdir_syscall_handler (struct intr_frame *f) {

}  

/* 
 * bool
 * readdir (int fd, char name[READDIR_MAX_LEN + 1])
 */
void readdir_syscall_handler (struct intr_frame *f) {

}  

/* 
 * bool
 * isdir (int fd)
 */
void isdir_syscall_handler (struct intr_frame *f) {

}  

/* 
 * int
 * inumber (int fd)
 */
void inumber_syscall_handler (struct intr_frame *f) {

}  

/* 
 * int
 * symlink (const char* target, const char* linkpath)
 */
void symlink_syscall_handler (struct intr_frame *f) {

}  

/* 
 * int
 * mount (const char *path, int chan_no, int dev_no)
 */
void mount_syscall_handler (struct intr_frame *f) {

}  

/* 
 * int
 * umount (const char *path)
 */
void umount_syscall_handler (struct intr_frame *f) {

} 

