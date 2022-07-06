// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NCACHE 13
#define HASH(blockno) ((blockno) % NCACHE)


struct buf buf[NBUF];

struct {
  struct spinlock lock;
	
  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache[NCACHE];


void
binit(void)
{
  struct buf *b;

  //头结点指向自己
  for(int i = 0; i < NCACHE; i++){
      bcache[i].head.prev = &bcache[i].head;
      bcache[i].head.next = &bcache[i].head;
      initlock(&bcache[i].lock, "bcache_bucket");
  }

  // Create linked list of buffers
  for(b = buf; b < buf+NBUF; b++){
    b->next = bcache[0].head.next;
    b->prev = &bcache[0].head;
    initsleeplock(&b->lock, "buffer");
    bcache[0].head.next->prev = b;
    bcache[0].head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b, *res = 0;

  uint idx = HASH(blockno);

  acquire(&bcache[idx].lock);
  // Is the block already cached?
  for(b = bcache[idx].head.next; b != &bcache[idx].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
			release(&bcache[idx].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(int i = 0; i < NCACHE; i++){
    if(i != idx) acquire(&bcache[i].lock);
    int rls = 1;

    //时刻保证当前的最优解的锁被持有
    for(b = bcache[i].head.prev; b != &bcache[i].head; b = b->prev) if(b->refcnt == 0){
      if(i == 0){
        res = b;
        rls = 0;
      }
      else if(b->ticks < res->ticks){
        release(&bcache[HASH(res->blockno)].lock);
        res = b;
        rls = 0;
      }

      break;
    }

    if(rls && i != idx) 
      release(&bcache[i].lock);
  }

  if(HASH(res->blockno) == idx){
    res->dev = dev;
    res->blockno = blockno;
    res->valid = 0;
    res->refcnt = 1;
    release(&bcache[idx].lock);

    acquiresleep(&res->lock);
    return res;
  }
  else{
    if(!holding(&bcache[idx].lock) || !holding(&bcache[HASH(res->blockno)].lock))
      panic("bget: Error");
    
    res->next->prev = res->prev;
    res->prev->next = res->next;
    release(&bcache[HASH(res->blockno)].lock);


    res->dev = dev;
    res->blockno = blockno;
    res->valid = 0;
    res->refcnt = 1;
    res->ticks = ticks;

    res->prev = &bcache[idx].head;
    res->next = bcache[idx].head.next;
    bcache[idx].head.next->prev = res;
    bcache[idx].head.next = res;

    release(&bcache[idx].lock);
    acquiresleep(&res->lock);
    return res;
  }


  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  
  uint idx = HASH(b->blockno);
  acquire(&bcache[idx].lock);
  b->refcnt--;
  if (b->refcnt == 0) if(bcache[idx].head.next != b){
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache[idx].head.next;
    b->prev = &bcache[idx].head;
    bcache[idx].head.next->prev = b;
    bcache[idx].head.next = b;
  }
  
  release(&bcache[idx].lock);
}

void
bpin(struct buf *b) {
  acquire(&bcache[HASH(b->blockno)].lock);
  b->refcnt++;
  release(&bcache[HASH(b->blockno)].lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache[HASH(b->blockno)].lock);
  b->refcnt--;
  release(&bcache[HASH(b->blockno)].lock);
}

// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


// #include "types.h"
// #include "param.h"
// #include "spinlock.h"
// #include "sleeplock.h"
// #include "riscv.h"
// #include "defs.h"
// #include "fs.h"
// #include "buf.h"

// struct {
//   struct spinlock locks[NBUCKET];
//   struct buf buf[NBUF];
//   struct buf head[NBUCKET];
// } bcache;

// void
// binit(void)
// {
//   struct buf *b;

//   for(int i = 0; i < NBUCKET; ++i) {
//     initlock(&bcache.locks[i], "bcache.bucket");
//     bcache.head[i].prev = &bcache.head[i];
//     bcache.head[i].next = &bcache.head[i];
//   }

//   for(b = bcache.buf; b < bcache.buf+NBUF; b++){
//     b->next = bcache.head[0].next;
//     b->prev = &bcache.head[0];
//     initsleeplock(&b->lock, "buffer");
//     bcache.head[0].next->prev = b;
//     bcache.head[0].next = b;
//   }
// }

// // Look through buffer cache for block on device dev.
// // If not found, allocate a buffer.
// // In either case, return locked buffer.
// static struct buf*
// bget(uint dev, uint blockno)
// {
//   struct buf *b, *head, *freehead;

//   acquire(&bcache.locks[hash(blockno)]);
//   head = &bcache.head[hash(blockno)];

//   // Is the block already cached?
//   for(b = head->next; b != head; b = b->next){
//     if(b->dev == dev && b->blockno == blockno){
//       b->refcnt++;
//       release(&bcache.locks[hash(blockno)]);
//       acquiresleep(&b->lock);
//       return b;
//     }
//   }

//   // Not cached.
//   // Recycle the least recently used (LRU) unused buffer.
//   for(int i = 1; i < NBUCKET; ++i) {
//     acquire(&bcache.locks[hash(blockno+i)]);
//     freehead = &bcache.head[hash(blockno+i)];
//     for(b = freehead->prev; b != freehead; b = b->prev) {
//       if(b->refcnt != 0) continue;
//       b->dev = dev;
//       b->blockno = blockno;
//       b->valid = 0;
//       b->refcnt = 1;
//       // remove from free list
//       b->next->prev = b->prev;
//       b->prev->next = b->next;
//       // insert to head
//       b->next = head->next;
//       b->prev = head;
//       head->next->prev = b;
//       head->next = b;
//       release(&bcache.locks[hash(blockno+i)]);
//       release(&bcache.locks[hash(blockno)]);
//       acquiresleep(&b->lock);
//       return b;
//     }
//     release(&bcache.locks[hash(blockno+i)]);
//   }
//   panic("bget: no buffers");
// }

// // Return a locked buf with the contents of the indicated block.
// struct buf*
// bread(uint dev, uint blockno)
// {
//   struct buf *b;

//   b = bget(dev, blockno);
//   if(!b->valid) {
//     virtio_disk_rw(b, 0);
//     b->valid = 1;
//   }
//   return b;
// }

// // Write b's contents to disk.  Must be locked.
// void
// bwrite(struct buf *b)
// {
//   if(!holdingsleep(&b->lock))
//     panic("bwrite");
//   virtio_disk_rw(b, 1);
// }

// // Release a locked buffer.
// // Move to the head of the most-recently-used list.
// void
// brelse(struct buf *b)
// {
//   if(!holdingsleep(&b->lock))
//     panic("brelse");

//   releasesleep(&b->lock);

//   acquire(&bcache.locks[hash(b->blockno)]);
//   b->refcnt--;
//   if (b->refcnt == 0) {
//     // no one is waiting for it.
//     b->next->prev = b->prev;
//     b->prev->next = b->next;
//     b->next = &bcache.head[hash(b->blockno)];
//     b->prev = bcache.head[hash(b->blockno)].prev;
//     bcache.head[hash(b->blockno)].prev->next = b;
//     bcache.head[hash(b->blockno)].prev = b;
//   }
  
//   release(&bcache.locks[hash(b->blockno)]);
// }

// void
// bpin(struct buf *b) {
//   acquire(&bcache.locks[hash(b->blockno)]);
//   b->refcnt++;
//   release(&bcache.locks[hash(b->blockno)]);
// }

// void
// bunpin(struct buf *b) {
//   acquire(&bcache.locks[hash(b->blockno)]);
//   b->refcnt--;
//   release(&bcache.locks[hash(b->blockno)]);
// }


