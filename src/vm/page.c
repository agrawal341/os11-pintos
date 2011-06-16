#include <debug.h>
#include <stdio.h>
#include "vm/page.h"
#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/pte.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

static bool install_lazy_user_page (void *upage, void *kpage, bool writable);
static struct sup_page *page_lookup (const void *address);

void *
get_multiple_user_pages(enum palloc_flags flags, size_t page_cnt)
{
	/* allocate user frame */
	return alloc_user_frames(flags, page_cnt);
}


void *
get_user_page(enum palloc_flags flags)
{
	return get_multiple_user_pages(flags, 1);
}


void
free_multiple_user_pages(void * pages, size_t page_cnt)
{
	lock_acquire(&user_frames_lock);

	unsigned i;
	uint32_t * pte;

	/*
	for(i = 0; i < page_cnt; i++)
	{
		pte = get_pte(thread_current()->pagedir, (const void *) pages + i * PGSIZE);

		if(*pte & PTE_P)
		{

		}

	}
	 */

	/* delete hash entry */
	unregister_frames(pages, page_cnt);

	/* free pages */
	palloc_free_multiple(pages, page_cnt);

	/* cleanup page dir */
	for(i = 0; i < page_cnt; i++)
	{
		pagedir_clear_page(thread_current()->pagedir, pages);
	}

	lock_release(&user_frames_lock);
}

void
free_user_page(void * page)
{
	free_multiple_user_pages(page, 1);
}


void
destroy_user_pages(void)
{
	/* TODO check swap memory for user frames */
	destroy_user_frames();
}

void
create_lazy_user_page (struct file* file, struct Elf32_Ehdr *ehdr)
{
	/* create sup pte */
	struct sup_page*  p = (struct sup_page *) malloc(sizeof(struct sup_page));

	p->f = file;
	p->isExec = true;
	p->swap = false;
	p->ehdr = ehdr;
	p->vaddr = (void *) (ehdr->e_entry & PTE_ADDR);

	/* insert into sup page table */
	ASSERT(hash_replace (&thread_current()->sup_page_table, &p->elem) == NULL);

	/* create page dir dummy pointing to first kernel page.
	 * ASSUMPTION: first kernel page is always zeroed */
	ASSERT(install_lazy_user_page(p->vaddr, PHYS_BASE, true));

	/* get page table entry */
	uint32_t *pte = get_pte(thread_current()->pagedir, p->vaddr);

	/* set present to false */
	*pte = *pte & ~PTE_P;

	/* set vaddress to eip */
	*pte = (*pte & PTE_FLAGS) | (ehdr->e_entry & PTE_ADDR);

}


bool
is_legal_stack_growth (void *fault_addr, void* esp)
{	
	if(DEBUG) printf("Check for legal stack growth. esp: %x - access at: %x\n", (uint32_t) esp, (uint32_t) fault_addr);
	
	uint32_t fault_ptr = (uint32_t) fault_addr;
	uint32_t stack_ptr = (uint32_t) esp;

	/* fault address under esp */
	if(fault_ptr <= stack_ptr)
	{
		if(DEBUG) printf("fault address under esp\n");
		/* max 32 byte */		
		return ((stack_ptr - fault_ptr) <= STACK_GROW_LIMIT);
	}
	/* fault address above esp */
	else
	{
		/* somewhere between first page and esp */
		return fault_ptr < (uint32_t)(PHYS_BASE - PGSIZE);
	}
}


bool
install_user_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  register_frame(upage, kpage);

  if(DEBUG) printf("Installing User Page: %x -> %x\n", (unsigned int) upage, (unsigned int) kpage);

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

static bool
install_lazy_user_page (void *upage, void *kpage, bool writable)
{
	struct thread *t = thread_current ();

	if(DEBUG) printf("Installing Lazy User Page: %x -> %x\n", (unsigned int) upage, (unsigned int) kpage);

	/* Verify that there's not already a page at that virtual
	   address, then map our page there. */
	return (pagedir_get_page (t->pagedir, upage) == NULL
			&& pagedir_set_page (t->pagedir, upage, kpage, writable));
}

/* Returns a hash value for frame f. */
unsigned
sup_page_hash (const struct hash_elem *p_, void *aux UNUSED)
{
  const struct sup_page *p = hash_entry (p_, struct sup_page, elem);
  return hash_int ((int) (((unsigned int) p->vaddr) & PTE_ADDR));
}

/* Returns true if frame a precedes frame b. */
bool
sup_page_less (const struct hash_elem *a_, const struct hash_elem *b_,
           void *aux UNUSED)
{
  const struct sup_page *a = hash_entry (a_, struct sup_page, elem);
  const struct sup_page *b = hash_entry (b_, struct sup_page, elem);

  return ((unsigned int) a->vaddr & PTE_ADDR) < ((unsigned int) b->vaddr & PTE_ADDR);
}


/* finds hash entry and handles swap / load */
bool
find_and_load_page(void* vaddr)
{
	bool success = false;
	struct sup_page* p = page_lookup(vaddr);
	if(p != NULL)
	{
		/* complete initialization */
		if((unsigned int) p->vaddr == USER_CODE_START)
		{
			success = load_user_code_and_data(p->f, p->ehdr);
		}
		else if(p->swap)
		{
			/* swap in page */
		}
		else
		{
			/* load memory mapped page from file */
			success = load_mmap_data(p);
		}
	}

	if(DEBUG && !success) printf("did not find page");
	return success;
}


void
grow_stack (void *fault_addr)
{
	if(DEBUG) printf("growing stack for address %x\n", (unsigned int) fault_addr);

	/* checks whether there is enough space left (less than 8MB occupied) */
	if ((PHYS_BASE - fault_addr) < MAX_USER_STACK_SIZE)
	{
		lock_acquire(&user_frames_lock);

		uint8_t *kpage;
		kpage = get_user_page (PAL_ZERO);

		ASSERT (kpage != NULL);

		ASSERT (install_user_page (pg_round_down(fault_addr), kpage, true));

		lock_release(&user_frames_lock);
	}

}

/* hash search */
static struct sup_page *
page_lookup (const void *address)
{
  struct sup_page p;
  struct hash_elem *e;

  p.vaddr = (void *) ((unsigned int)address & PTE_ADDR);
  e = hash_find (&thread_current()->sup_page_table, &p.elem);
  return e != NULL ? hash_entry (e, struct sup_page, elem) : NULL;
}

void
create_lazy_mmap_page (struct file* file, uint32_t file_length, uint32_t offset, void* upage)
{
	ASSERT(offset % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);

	/* create sup pte */
	struct sup_page*  p = (struct sup_page *) malloc(sizeof(struct sup_page));
	p->vaddr = upage;

	p->f = file;
	p->offset = offset;
	p->length = file_length;

	p->isExec = false;
	p->swap = false;
	p->ehdr = NULL;

	/* insert into sup page table */
	ASSERT(hash_replace (&thread_current()->sup_page_table, &p->elem) == NULL);

	/* create page table dummy pointing to first kernel page.
	 * ASSUMPTION: first kernel page is always zeroed */
	ASSERT(install_lazy_user_page(p->vaddr, PHYS_BASE, true));

	/* get page table entry */
	uint32_t *pte = get_pte(thread_current()->pagedir, p->vaddr);

	/* set present to false */
	*pte = *pte & ~PTE_P;

	/* set vaddress to upage */
	*pte = (*pte & PTE_FLAGS) | ((uint32_t)upage & PTE_ADDR);
}

void delete_lazy_mmap_page(void* upage)
{
	struct sup_page * page = page_lookup ((const void*) upage);

	hash_delete(&thread_current()->sup_page_table, &page->elem);
}

bool
load_mmap_data(struct sup_page* p)
{
	/* fetch information */
	struct file* file = p->f;
	uint32_t offset = p->offset;
	uint32_t length = p->length;

	void *upage = p->vaddr;

	lock_acquire(&user_frames_lock);

	/* allocate user page */
	void* kpage = get_user_page(PAL_ZERO);

	ASSERT(kpage != NULL);

	/* calculate size */
	off_t size = PGSIZE;
	if(length - offset < PGSIZE)
	{
		size = length - offset;
	}

	/* acquire file system lock */
	lock_acquire(&filesystem_lock);

	/* copy contents */
	file_seek(file, (off_t) offset);
	off_t len = file_read(file, kpage, size);

	/* acquire file system lock */
	lock_release(&filesystem_lock);

	if(DEBUG && len != size){
		printf("read: %u, size: %u\n", (uint32_t) len, (uint32_t) size);
	}
	ASSERT(install_user_page(upage, kpage, true));

	lock_release(&user_frames_lock);

	return true;
}
