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
  struct buf *b, *now;

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

  for(int i = 1; i < NCACHE; i++){
    acquire(&bcache[HASH(i + blockno)].lock);

    now = &bcache[HASH(i + blockno)].head;
    for(b = now->prev; b != now; b = b->prev){
      if(b->refcnt != 0) continue;
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;

      b->next->prev = b->prev;
      b->prev->next = b->next;


      b->prev = &bcache[idx].head;
      b->next = bcache[idx].head.next;
      bcache[idx].head.next->prev = b;
      bcache[idx].head.next = b;

      release(&bcache[idx].lock);
      release(&bcache[HASH(i + blockno)].lock);
      acquiresleep(&b->lock);
      return b;
    }
    release(&bcache[HASH(i + blockno)].lock);
  }

    // now = &bcache[HASH(blockno)].head;
  // for(b = now->prev; b != now; b = b->prev){
  //   if(b->refcnt != 0) continue;
  //   b->dev = dev;
  //   b->blockno = blockno;
  //   b->valid = 0;
  //   b->refcnt = 1;

  //   b->next->prev = b->prev;
  //   b->prev->next = b->next;


  //   b->prev = &bcache[idx].head;
  //   b->next = bcache[idx].head.next;
  //   bcache[idx].head.next->prev = b;
  //   bcache[idx].head.next = b;

  //   release(&bcache[idx].lock);
  //   acquiresleep(&b->lock);
  //   return b;
  // }
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
  if (b->refcnt == 0){
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
