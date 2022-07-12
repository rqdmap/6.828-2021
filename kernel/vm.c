#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"
#include "fcntl.h"
#include "sleeplock.h"
#include "file.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // map kernel stacks
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if(size == 0)
    panic("mappages: size");
  
  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// va 地址并未对应一个物理地址，进行分配。
int
pgMissHandler(pagetable_t pagetable, uint64 va, 
    struct mmapInfo* mi){
  pte_t *pte;
  char *mem;
  uint prot;
  uint tot;
  struct file* fp = mi->fp;

  if((pte = walk(pagetable, va, 0)) == 0)
    panic("pgMissHandler: pte should exist");
  
  if((mem = kalloc()) == 0) 
    return -1;

  // 为mem写入内容
  ilock(fp->ip);
  if((tot = readi(fp->ip, 0, (uint64)mem, PGROUNDDOWN(va) - mi->addr, PGSIZE)) < 0)
    return -1;
  iunlock(fp->ip);
  for(uint i = tot; i < PGSIZE; i++) mem[i] = 0;
  
  prot = PTE_V | PTE_U;
  //假设prot不包含Exec
  if(mi->prot & PROT_READ) 
    prot |= PTE_R;
  if(mi->prot & PROT_WRITE)  
    prot |= PTE_W;

  uvmunmap(pagetable, va, 1, 0);
  //重新映射回去
  if(mappages(pagetable, va, PGSIZE, (uint64)mem, prot) != 0){
    kfree(mem);
    return -1;
  }
  return 0;
}

uint64 
mmapalloc(struct proc *p, uint pages){
  uint64 res;
  acquire(&p->lock);
  p->mp -= pages * PGSIZE;
  res = p->mp;
  release(&p->lock);
  return res;
}

uint64
sys_mmap(void){
  uint64 addr;
  int length, prot, flags, fd, offset;
  struct proc *p = myproc();
  uint64 start, a;
  uint pages;
  pagetable_t pagetable;
  pte_t *pte;
  struct mmapInfo *mi = 0;

  pagetable = p->pagetable;

  if(argaddr(0, &addr) || argint(1, &length) || argint(2, &prot) 
    || argint(3, &flags) || argint(4, &fd) || argint(5, &offset))
    return -1;

  if(length == 0) 
    return -1;

  for(int i = 0; i < NOFILE; i++) if(p->mmapInfo[i].length == 0){
    mi = &p->mmapInfo[i];
    break;
  }
  if(mi == 0)
    panic("mmap: No free mmap areas");
  
  mi->fp = p->ofile[fd];
  mi->flags = flags;
  if(!mi->fp->readable && prot & PROT_READ)
    return -1;
  if(!mi->fp->writable && prot & PROT_WRITE && mi->flags & MAP_SHARED)
    return -1;
  
  mi->prot = prot;
  mi->length = length;
  mi->offset = offset;

  pages = PGROUNDUP(length) / PGSIZE;
  mi->addr = mmapalloc(p, pages);

  for(a = 0; a < length; a += PGSIZE){
    if((pte = walk(pagetable, a + mi->addr, 1)) == 0)
      goto err;

    *pte = PTE_V | PTE_E;
  }

  filedup(p->ofile[fd]);

  return mi->addr;

err:
  uvmunmap(pagetable, mi->addr, (a + mi->addr) / PGSIZE, 1);
  return -1;
}

//以页框为单位的写回
int 
write_back(struct inode *ip, uint64 va, uint off){
  if(va % PGSIZE)
    panic("write_back: not aligned");
  
  begin_op();
  ilock(ip);
  if(writei(ip, 1, va, off, PGSIZE) < 0){
    end_op();
    return -1;
  }
  iunlock(ip);
  end_op();
  return 0;
}

void 
__munmap(pagetable_t pagetable, uint64 start, uint64 end, struct mmapInfo *mi){
  pte_t *pte;

  for(uint64 va = start; va < end; va += PGSIZE){
    if((pte = walk(pagetable, va, 0)) == 0)
      panic("munmap: walk");

    if(mi->flags & MAP_SHARED){
      if(write_back(mi->fp->ip, va, va - mi->addr + mi->offset) < 0)
        panic("munmap: write_back");
    }

    if(!(*pte & PTE_E))
      kfree((void*)(uint64)PTE2PA(*pte));
    *pte = 0;
  }
}

uint64
sys_munmap(void){
  uint64 addr;
  int length;
  struct proc *p = myproc();
  pagetable_t pagetable;
  int fd = -1;
  struct mmapInfo* mi;

  pagetable = p->pagetable;

  if(argaddr(0, &addr) || argint(1, &length))
    return -1;

  if(length == 0) 
    return 0;
  
  for(int i = 0; i < NOFILE; i++){
    if(p->mmapInfo[i].addr <= addr && addr < p->mmapInfo[i].addr + p->mmapInfo[i].length){
      fd = i;
      break;
    }
  }
  if(fd == -1)
    panic("munmap: No such file");
  mi = &p->mmapInfo[fd];

  if(mi->addr <= addr && addr + length <= mi->addr + mi->length);
  else
    panic("munmap: Out of bound");
  //保证是前缀或后缀
  if(mi->addr == addr);
  else if(mi->addr + mi->length == addr + length);
  else
    panic("munmap: Not valid range");

  if(length == mi->length){
    __munmap(pagetable, PGROUNDDOWN(mi->addr), PGROUNDUP(mi->addr + mi->length), mi);
    fileclose(mi->fp);
    memset(mi, 0, sizeof(struct mmapInfo));
  }else if(addr == mi->addr){
    //可能会有部分页残余
    __munmap(pagetable, PGROUNDDOWN(mi->addr), PGROUNDDOWN(mi->addr + length), mi);
    mi->addr += length;
    mi->length -= length;
    mi->offset += length;
  }
  else {
    __munmap(pagetable, PGROUNDUP(mi->addr), PGROUNDUP(mi->addr + length), mi);
    mi->length -= length;
  }
  return 0;
}