#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdbool.h>
#include <list.h>
#include "filesys/off_t.h"
#include "devices/disk.h"

#define CACHE_MAX 64

struct cache
  {
    disk_sector_t sector;
    bool dirty;
	struct list_elem elem;
    char data[DISK_SECTOR_SIZE];
  };

void cache_init (void);
void cache_done (void);
void cache_backup (void);
void cache_create (disk_sector_t);
void cache_write (disk_sector_t, const void *, off_t, size_t);
void cache_read (disk_sector_t, void *, off_t, size_t);
void cache_remove (disk_sector_t);

#endif /* filesys/cache.h */
