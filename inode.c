#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "filesys/cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT_BLOCK 12
#define SINGLE_BLOCK (DISK_SECTOR_SIZE / sizeof (disk_sector_t))

#define MODIFY_CACHE \ 
  cache_read (sector, p, (off_t) p - (off_t) cache_data, sizeof (disk_sector_t)); \
  if (*p == 0) \
    { \
      success = free_map_allocate (1, p); \
      ASSERT (success); \
      cache_write (sector, p, (off_t) p - (off_t) cache_data, sizeof (disk_sector_t)); \
      cache_create (*p); \
    } \
  sector = *p

/* On-disk inode.
   Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    off_t length;                       /* File size in bytes. */
    bool dir;
    disk_sector_t direct[DIRECT_BLOCK];
    disk_sector_t indirect;
    disk_sector_t double_indirect;
    unsigned magic;                     /* Magic number. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    disk_sector_t sector;               /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
  };

/* Returns the disk sector that contains byte offset POS within
   INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  struct inode_disk *data;
  disk_sector_t cache_data[DISK_SECTOR_SIZE];
  disk_sector_t sector;
  disk_sector_t *p;
  int num, single;
  bool success;

  ASSERT (inode != NULL);
  sector = inode->sector;
  data = (struct inode_disk *) cache_data;
  p = &data->length;
  cache_read (sector, p, (off_t) p - (off_t) cache_data, sizeof (disk_sector_t));
          
  if (pos < data->length)
    {
      num = pos / DISK_SECTOR_SIZE;
      single = SINGLE_BLOCK;

      if (num < DIRECT_BLOCK)
        {
          p = &data->direct[num];
          MODIFY_CACHE;
          return sector;
        }
      else if (num < DIRECT_BLOCK + single)
        {
          num -= DIRECT_BLOCK;

          p = &data->indirect;
          MODIFY_CACHE;
          
          p = &cache_data[num];
          MODIFY_CACHE;
          return sector;
        }
      else
        {
          num -= DIRECT_BLOCK + single;

          p = &data->double_indirect;
          MODIFY_CACHE;

          p = &cache_data[num / single];
          MODIFY_CACHE;

          p = &cache_data[num % single];
          MODIFY_CACHE;
          return sector;
        }
    }
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

static struct lock inode_lock;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&inode_lock);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   disk.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (disk_sector_t sector, off_t length)
{
  struct inode_disk *data;

  ASSERT (length >= 0);

  data = calloc (sizeof *data, 1);
  data->length = length;
  data->magic = INODE_MAGIC;
  if (sector == 0)
    {
      ASSERT (free_map_allocate(1, &data->direct[0]));
    }

  lock_acquire (&inode_lock);
  cache_create (sector);
  cache_write (sector, data, 0, sizeof *data);
  lock_release (&inode_lock);

  free (data);
  return true;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (disk_sector_t sector) 
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  lock_acquire (&inode_lock);
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_release (&inode_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  void *cache_data, *cache_data1;
  struct inode_disk *data;
  disk_sector_t *indirect, *double_indirect;
  disk_sector_t indirect_sector, double_indirect_sector;
  int i, j, single;

  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          single = SINGLE_BLOCK;

          cache_data = calloc (sizeof *data, 1);
          data = cache_data;
          ASSERT (cache_data != NULL);

          cache_read (inode->sector, cache_data, 0, sizeof *data);
          cache_remove (inode->sector);
          free_map_release (inode->sector, 1);

          for (i = 0; i < DIRECT_BLOCK; i++)
            {
              if (data->direct[i] != 0)
                {
                  cache_remove (data->direct[i]);
                  free_map_release (data->direct[i], 1);
                }
            }

          indirect_sector = data->indirect;
          double_indirect_sector = data->double_indirect;
          free (cache_data);

          if (indirect_sector != 0)
            {
              cache_data = calloc (DISK_SECTOR_SIZE, 1);
              indirect = cache_data;
              ASSERT (cache_data != NULL);

              cache_read (indirect_sector, cache_data, 0, DISK_SECTOR_SIZE);
              cache_remove (indirect_sector);
              free_map_release (indirect_sector, 1);

              for (i = 0; i < single; i++)
                {
                  if (indirect[i] != 0)
                    {
                      cache_remove (indirect[i]);
                      free_map_release (indirect[i], 1);
                    }
                }

              free (cache_data);
            }

          if (double_indirect_sector != 0)
            {
              cache_data = calloc (DISK_SECTOR_SIZE, 1);
              indirect = cache_data;
              ASSERT (cache_data != NULL);

              cache_read (double_indirect_sector, cache_data, 0, DISK_SECTOR_SIZE);
              cache_remove (double_indirect_sector);
              free_map_release (double_indirect_sector, 1);

              for (i = 0; i < single; i++)
                {
                  if (indirect[i] != 0)
                    {
                      cache_data1 = calloc (DISK_SECTOR_SIZE, 1);
                      double_indirect = cache_data1;
                      ASSERT (cache_data1 != NULL);

                      cache_read (indirect[i], cache_data1, 0, DISK_SECTOR_SIZE);
                      cache_remove (indirect[i]);
                      free_map_release (indirect[i], 1);

                      for (j = 0; j < single; j++)
                        {
                          if (double_indirect[j] != 0)
                            {
                              cache_remove (double_indirect[j]);
                              free_map_release (double_indirect[i], 1);
                            }
                        }
                    }
                }
            }
        }
      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      cache_read (sector_idx, buffer + bytes_read, sector_ofs, chunk_size);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0, length;

  if (inode->deny_write_cnt)
    return 0;

  length = inode_length (inode);
  if (length < size + offset)
    length = size + offset;
  cache_write (inode->sector, &length, offsetof (struct inode_disk, length), sizeof (off_t));

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = length - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      cache_write (sector_idx, buffer + bytes_written, sector_ofs, chunk_size);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  off_t length;
  cache_read (inode->sector, &length, offsetof (struct inode_disk, length), sizeof length);
  return length;
}