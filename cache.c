#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "devices/disk.h"
#include <list.h>
#include <string.h>
#include <debug.h>
#include "threads/malloc.h"
#include "threads/synch.h"

static struct cache *cache_make (disk_sector_t, bool);
static struct cache *cache_lookup (disk_sector_t, bool);
static void cache_save_all (void);
static void cache_evict (void);
static void cache_save (struct cache *);

static struct list caches;
static struct lock lock;
static int cache_count;

void
cache_init (void)
{
  list_init (&caches);
  lock_init (&lock);
  cache_count = 0;
}

void
cache_done (void)
{
  cache_save_all ();
  while (!list_empty (&caches))
    free (list_pop_front (&caches));
}

void
cache_backup (void)
{
  lock_acquire (&lock);
  cache_save_all ();
  lock_release (&lock);
}

void
cache_create (disk_sector_t sector)
{
  lock_acquire (&lock);
  cache_make (sector, false);
  lock_release (&lock);
}

void
cache_write (disk_sector_t sector, const void *buf, off_t off, size_t size)
{
  struct cache *c;

  ASSERT (off + size <= DISK_SECTOR_SIZE);
  if (size == 0) return;

  lock_acquire (&lock);
  c = cache_lookup (sector, true);
  memcpy (c->data + off, buf, size);
  c->dirty = true;
  list_remove (&c->elem);
  list_push_front (&caches, &c->elem);
  lock_release (&lock);
}

void
cache_read (disk_sector_t sector, void *buf, off_t off, size_t size)
{
  struct cache *c;

  ASSERT (off + size <= DISK_SECTOR_SIZE);
  if (size == 0) return;

  lock_acquire (&lock);
  c = cache_lookup (sector, true);
  memcpy (buf, c->data + off, size);
  list_remove (&c->elem);
  list_push_front (&caches, &c->elem);
  lock_release (&lock);
}

void
cache_remove (disk_sector_t sector)
{
  struct cache *c;

  lock_acquire (&lock);
  c = cache_lookup (sector, false);
  if (c != NULL)
    {
      list_remove (&c->elem);
      cache_count--;
      free (c);
    }
  lock_release (&lock);

  free (c);
}

static struct cache *
cache_make (disk_sector_t sector, bool read)
{
  struct cache *c;

  if (cache_count == CACHE_MAX)
    cache_evict ();

  c = calloc (sizeof *c, 1);
  ASSERT (c != NULL);
  c->sector = sector;
  if (read)
    disk_read (filesys_disk, sector, c->data);

  list_push_front (&caches, &c->elem);
  cache_count++;
  return c;
}

static struct cache *
cache_lookup (disk_sector_t sector, bool make)
{
  struct cache *c;
  struct list_elem *e;

  for (e = list_begin (&caches); e != list_end (&caches); e = list_next (e))
    {
      c = list_entry (e, struct cache, elem);
      if (c->sector == sector)
        return c;
    }
  if (make)
    return cache_make (sector, true);
  else
    return NULL;
}

static void
cache_save_all (void)
{
  struct list_elem *e;

  for (e = list_begin (&caches); e != list_end (&caches); e = list_next (e))
    cache_save (list_entry (e, struct cache, elem));
}

static void
cache_save (struct cache *c)
{
  if (c->dirty)
    disk_write (filesys_disk, c->sector, c->data);
}

static void
cache_evict (void)
{
  struct cache *c;
  
  c = list_entry (list_pop_back (&caches), struct cache, elem);
  cache_count--;
  cache_save (c);
  free (c);
}
