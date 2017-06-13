#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "devices/disk.h"
#include "filesys/cache.h"
#include "threads/thread.h"
#include "threads/malloc.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);
static struct inode *filesys_open_from_dir (char **, struct dir *, int, int);
static int token_num (char *);
static char **tokenize_path (char *, int);
static bool filesys_make (const char *, bool, off_t);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  filesys_disk = disk_get (0, 1);
  if (filesys_disk == NULL)
    PANIC ("hd0:1 (hdb) not present, file system initialization failed");

  inode_init ();
  free_map_init ();
  cache_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
}

char *
filesys_absolute (const char *name)
{
  int len, len1;
  char *path, *dir;

  if (name == NULL)
    return NULL;

  len = strlen(name);
  if (len == 0)
    return NULL;

  if (name[0] == '/')
    {
      path = malloc (len + 2);
      ASSERT (path != NULL);
      strlcpy (path, name, len + 1);
      path[len] = '/';
      path[len + 1] = '\0';
    }
  else
    {
      dir = thread_current ()->dir;
      len1 = strlen (dir);
      path = malloc (len + len1 + 2);
      ASSERT (path != NULL);
      strlcpy (path, dir, len1 + 1);
      strlcpy (path + len1, name, len + 1);
      path[len + len1] = '/';
      path[len + len1 + 1] = '\0';
    }
  return path;
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  return filesys_make (name, false, initial_size);
}

bool
filesys_create_dir (const char *name)
{
  return filesys_make (name, true, 0);
}

struct inode *
filesys_open_inode (const char *name)
{
  char **path_name;
  char *path;
  int count;
  struct inode *inode;
  struct dir *dir;

  path = filesys_absolute (name);
  if (path == NULL)
    return NULL;

  count = token_num (path);
  inode = inode_open (ROOT_DIR_SECTOR);
  if (count == 0)
    {
      free (path);
      return inode;
    }

  path_name = tokenize_path (path, count);
  dir = dir_open (inode);
  inode = filesys_open_from_dir (path_name, dir, 0, count);
  dir_close (dir);

  free (path);
  free (path_name);
  return inode;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct inode *inode = filesys_open_inode (name);
  if (inode == NULL || inode_dir (inode))
    return NULL;
  else
    return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  char *path;
  char **path_name;
  int count;
  struct dir *parent;
  struct inode *inode;

  path = filesys_absolute (name);
  if (path == NULL)
    return false;

  count = token_num (path);
  if (count == 0)
    {
      free (path);
      return false;
    }

  path_name = tokenize_path (path, count);
  parent = dir_open_root ();
  if (count != 1)
    {
      inode = filesys_open_from_dir (path_name, parent, 0, count - 1);
      dir_close (parent);
      if (inode == NULL || !inode_dir (inode))
        {
          free (path);
          free (path_name);
          return false;
        }
      parent = dir_open (inode);
    }

  bool success = dir_remove (parent, path_name[count - 1]);
  dir_close (parent); 
  free (path_name);
  free (path);
  return success;
}


/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}


static int
token_num (char *path)
{
  int len, count;
  char *path_, *token, *save_ptr;

  len = strlen (path);
  path_ = malloc (len + 1);
  ASSERT (path_ != NULL);
  strlcpy (path_, path, len + 1);

  count = 0;
  for (token = strtok_r (path_, "/", &save_ptr); token != NULL;
    token = strtok_r (NULL, "/", &save_ptr))
    count++;
  free (path_);

  return count;
}

static char ** 
tokenize_path (char *path, int num)
{
  char **path_name;
  char *token, *save_ptr;
  int i;

  path_name = malloc (sizeof (char *) * num);
  ASSERT (path_name != NULL);

  i = 0;
  for (token = strtok_r (path, "/", &save_ptr); token != NULL;
    token = strtok_r (NULL, "/", &save_ptr))
    path_name[i++] = token;
  
  return path_name;
}


static struct inode *
filesys_open_from_dir (char **path_name, struct dir *dir, int idx, int length)
{
  struct inode *inode, *result;
  struct dir *dir_new;
  char *name;

  name = path_name[idx];
  dir_lookup (dir, path_name[idx], &inode);
  if (inode == NULL)
    return NULL;

  if (idx == length - 1)
    return inode;

  if (inode_dir (inode))
    {
      dir_new = dir_open (inode);
      result = filesys_open_from_dir (path_name, dir_new, idx + 1, length);
      dir_close (dir_new);
      return result;
    }

  inode_close (inode);
  return NULL;
}

static bool
filesys_make (const char *name, bool dir, off_t initial_size)
{
  char **path_name;
  char *path;
  int count;
  disk_sector_t sector;
  struct dir *parent;
  struct inode *inode;
  bool success;

  path = filesys_absolute (name);
  if (path == NULL)
    return false;

  count = token_num (path);
  if (count == 0)
    {
      free (path);
      return false;
    }

  path_name = tokenize_path (path, count);
  parent = dir_open_root ();
  if (count != 1)
    {
      inode = filesys_open_from_dir (path_name, parent, 0, count - 1);
      dir_close (parent);
      if (inode == NULL || !inode_dir (inode))
        {
          free (path_name);
          free (path);
          return false;
        }
      parent = dir_open (inode);
    }

  ASSERT (free_map_allocate (1, &sector));
  if (dir)
    dir_create (sector, 16);
  else 
    inode_create (sector, initial_size);
  success = dir_add (parent, path_name[count - 1], sector);
  if (!success)
    free_map_release (sector, 1);
  dir_close (parent);
  free (path_name);
  free (path);
  return success;
}
