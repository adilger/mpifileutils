#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h> /* asctime / localtime */

#include <pwd.h> /* for getpwent */
#include <grp.h> /* for getgrent */
#include <errno.h>
#include <string.h>

#include <libgen.h> /* dirname */

#include "libcircle.h"
#include "dtcmp.h"
#include "bayer.h"

#include <map>
#include <string>

using namespace std;

/****************************************
 * Define types
 ***************************************/

/* linked list element of stat data used during walk */
typedef struct list_elem {
  char* file;             /* file name (strdup'd) */
  int depth;              /* depth within directory tree */
  bayer_filetype type;    /* type of file object */
  int detail;             /* flag to indicate whether we have stat data */
  uint32_t mode;          /* stat mode */
  uint32_t uid;           /* user id */
  uint32_t gid;           /* group id */
  uint32_t atime;         /* access time */
  uint32_t mtime;         /* modify time */
  uint32_t ctime;         /* create time */
  uint64_t size;          /* file size in bytes */
  struct list_elem* next; /* pointer to next item */
} elem_t;

/* holds an array of objects: users, groups, or file data */
typedef struct {
  void* buf;
  uint64_t count;
  uint64_t chars;
  MPI_Datatype dt;
} buf_t;

/* abstraction for distributed file list */
typedef struct flist {
  int detail;           /* set to 1 if we have stat, 0 if just file name */
  uint64_t total_files; /* total file count in list across all procs */
  uint64_t max_file_name;
  int min_depth;
  int max_depth;

  /* variables to track linked list of stat data during walk */
  uint64_t list_count;
  elem_t*  list_head;
  elem_t*  list_tail;
  elem_t** list_index;

  /* buffers of users, groups, and files */
  buf_t users;
  buf_t groups;

  /* map linux userid to user name, and map groupid to group name */
  map<string,uint32_t>* user_name2id;
  map<uint32_t,string>* user_id2name;
  map<string,uint32_t>* group_name2id;
  map<uint32_t,string>* group_id2name;
} flist_t;

/****************************************
 * Globals
 ***************************************/

/* Need global variables during walk to record top directory
 * and file list */
static char CURRENT_DIR[PATH_MAX];
static flist_t* CURRENT_LIST;

/****************************************
 * Functions on types
 ***************************************/

static void buft_init(buf_t* items)
{
  items->buf   = NULL;
  items->count = 0;
  items->chars = 0;
  items->dt    = MPI_DATATYPE_NULL;
}

static void buft_free(buf_t* items)
{
  bayer_free(&items->buf);

  if (items->dt != MPI_DATATYPE_NULL) {
    MPI_Type_free(&(items->dt));
  }

  items->count = 0;
  items->chars = 0;

  return;
}

static bayer_filetype get_bayer_filetype(mode_t mode)
{
  /* set file type */
  bayer_filetype type;
  if (S_ISDIR(mode)) {
    type = TYPE_DIR;
  } else if (S_ISREG(mode)) {
    type = TYPE_FILE;
  } else if (S_ISLNK(mode)) {
    type = TYPE_LINK;
  } else {
    /* unknown file type */
    type = TYPE_UNKNOWN;
  }
  return type;
}

/* given path, return level within directory tree */
static int get_depth(const char* path)
{
    const char* c;
    int depth = 0;
    for (c = path; *c != '\0'; c++) {
        if (*c == '/') {
            depth++;
        }
    }
    return depth;
}

static void create_stattype(int detail, int chars, MPI_Datatype* dt_stat)
{
  /* build type for file path */
  MPI_Datatype dt_filepath;
  MPI_Type_contiguous(chars, MPI_CHAR, &dt_filepath);

  /* build keysat type */
  int fields;
  MPI_Datatype types[8];
  if (detail) {
    fields = 8;
    types[0] = dt_filepath;  /* file name */
    types[1] = MPI_UINT32_T; /* mode */
    types[2] = MPI_UINT32_T; /* uid */
    types[3] = MPI_UINT32_T; /* gid */
    types[4] = MPI_UINT32_T; /* atime */
    types[5] = MPI_UINT32_T; /* mtime */
    types[6] = MPI_UINT32_T; /* ctime */
    types[7] = MPI_UINT64_T; /* size */
  } else {
    fields = 2;
    types[0] = dt_filepath;  /* file name */
    types[1] = MPI_UINT32_T; /* file type */
  }
  DTCMP_Type_create_series(fields, types, dt_stat);

  MPI_Type_free(&dt_filepath);
  return;
}

/* append element to tail of linked list */
static void list_insert_elem(flist_t* flist, elem_t* elem)
{
  /* set head if this is the first item */
  if (flist->list_head == NULL) {
    flist->list_head = elem;
  }

  /* update last element to point to this new element */
  elem_t* tail = flist->list_tail;
  if (tail != NULL) {
    tail->next = elem;
  }

  /* make this element the new tail */
  flist->list_tail = elem;
  elem->next = NULL;

  /* increase list count by one */
  flist->list_count++;

  /* delete the index if we have one, it's out of date */
  bayer_free(&flist->list_index);

  return;
}

/* insert a file given its mode and optional stat data */
static void list_insert_stat(flist_t* flist, const char *fpath, mode_t mode, const struct stat *sb)
{
  /* create new element to record file path, file type, and stat info */
  elem_t* elem = (elem_t*) bayer_malloc(sizeof(elem_t), "File element", __FILE__, __LINE__);

  /* copy path */
  elem->file = bayer_strdup(fpath, "File name", __FILE__, __LINE__);

  /* set depth */
  elem->depth = get_depth(fpath);

  /* set file type */
  elem->type = get_bayer_filetype(mode);

  /* copy stat info */
  if (sb != NULL) {
    elem->detail = 1;
    elem->mode  = (uint32_t) sb->st_mode;
    elem->uid   = (uint32_t) sb->st_uid;
    elem->gid   = (uint32_t) sb->st_gid;
    elem->atime = (uint32_t) sb->st_atime;
    elem->mtime = (uint32_t) sb->st_mtime;
    elem->ctime = (uint32_t) sb->st_ctime;
    elem->size  = (uint64_t) sb->st_size;

    /* TODO: link to user and group names? */
  } else {
    elem->detail = 0;
  }

  /* append element to tail of linked list */
  list_insert_elem(flist, elem);

  return;
}

/* insert a file given just its name and type */
static void list_insert_lite(flist_t* flist, const char *fpath, bayer_filetype type)
{
  /* create new element to record file path and file type */
  elem_t* elem = (elem_t*) bayer_malloc(sizeof(elem_t), "File element", __FILE__, __LINE__);

  /* copy path */
  elem->file = bayer_strdup(fpath, "File name", __FILE__, __LINE__);

  /* set depth */
  elem->depth = get_depth(fpath);

  /* set file type */
  elem->type = type;

  /* set detail == 0 since we don't have stat info */
  elem->detail = 0;

  /* append element to tail of linked list */
  list_insert_elem(flist, elem);

  return;
}

/* insert a file given a pointer to packed data */
static void list_insert_ptr(flist_t* flist, char* ptr, uint64_t chars)
{
  /* create new element to record file path, file type, and stat info */
  elem_t* elem = (elem_t*) bayer_malloc(sizeof(elem_t), "File element", __FILE__, __LINE__);

  /* get name and advance pointer */
  const char* file = ptr;
  ptr += chars;

  /* copy path */
  elem->file = bayer_strdup(file, "File name", __FILE__, __LINE__);

  /* set depth */
  elem->depth = get_depth(file);

  elem->detail = 1;

  /* set file mode */
  elem->mode = *(uint32_t*)ptr;
  ptr += 4;

  /* use mode to set file type */
  elem->type = get_bayer_filetype((mode_t)elem->mode);

  elem->uid = *(uint32_t*)ptr;
  ptr += 4;

  elem->gid = *(uint32_t*)ptr;
  ptr += 4;

  elem->atime = *(uint32_t*)ptr;
  ptr += 4;

  elem->mtime = *(uint32_t*)ptr;
  ptr += 4;

  elem->ctime = *(uint32_t*)ptr;
  ptr += 4;

  elem->size  = *(uint64_t*)ptr;
  ptr += 8;

  /* append element to tail of linked list */
  list_insert_elem(flist, elem);

  return;
}

/* delete linked list of stat items */
static void list_delete(flist_t* flist)
{
  elem_t* current = flist->list_head;
  while (current != NULL) {
    elem_t* next = current->next;
    bayer_free(&current->file);
    bayer_free(&current);
    current = next;
  }
  flist->list_count = 0;
  flist->list_head  = NULL;
  flist->list_tail  = NULL;

  /* delete the cached index */
  bayer_free(&flist->list_index);

  return;
}

/* given an index, return pointer to that file element,
 * NULL if index is not in range */
static elem_t* list_get_elem(flist_t* flist, int index)
{
  uint64_t max = flist->list_count;

  /* build index of list elements if we don't already have one */
  if (flist->list_index == NULL) {
    /* allocate array to record pointer to each element */
    size_t index_size = max * sizeof(elem_t*);
    flist->list_index = (elem_t**) bayer_malloc(index_size, "List index", __FILE__, __LINE__);

    /* get pointer to each element */
    uint64_t i = 0;
    elem_t* current = flist->list_head;
    while (i < max && current != NULL) {
      flist->list_index[i] = current;
      current = current->next;
      i++;
    }
  }

  /* return pointer to element if index is within range */
  if (index >= 0 && index < max) {
    elem_t* elem = flist->list_index[index];
    return elem;
  }
  return NULL;
}

static void list_compute_summary(flist_t* flist)
{
  /* initialize summary values */
  flist->max_file_name = 0;
  flist->min_depth     = 0;
  flist->max_depth     = 0;
  flist->total_files   = 0;

  /* get total number of files in list */
  uint64_t total;
  uint64_t count = flist->list_count;
  MPI_Allreduce(&count, &total, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
  flist->total_files = total;

  /* bail out early if no one has anything */
  if (total <= 0) {
    return;
  }

  /* compute local min/max values */
  int min_depth = -1;
  int max_depth = -1;
  uint64_t max_name = 0;
  elem_t* current = flist->list_head;
  while (current != NULL) {
    uint64_t len = (uint64_t) (strlen(current->file) + 1);
    if (len > max_name) {
      max_name = len;
    }

    int depth = current->depth;
    if (depth < min_depth || min_depth == -1) {
      min_depth = depth;
    }
    if (depth > max_depth || max_depth == -1) {
      max_depth = depth;
    }

    /* go to next item */
    current = current->next;
  }

  /* get global maximums */
  int global_max_depth;
  MPI_Allreduce(&max_depth, &global_max_depth, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

  uint64_t global_max_name;
  MPI_Allreduce(&max_name, &global_max_name, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);

  /* since at least one rank has an item and max will be -1 on ranks
   * without an item, set our min to global max if we have no items,
   * this will ensure that our contribution is >= true global min */
  int global_min_depth;
  if (count == 0) {
    min_depth = global_max_depth;
  }
  MPI_Allreduce(&min_depth, &global_min_depth, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);

  /* set summary values */
  flist->max_file_name = global_max_name;
  flist->min_depth = global_min_depth;
  flist->max_depth = global_max_depth;

  return;
}

static int list_convert_to_dt(flist_t* flist, buf_t* items)
{
  int detail = flist->detail;
  elem_t* head = flist->list_head;

  /* initialize output params */
  items->buf   = NULL;
  items->count = 0;
  items->chars = 0;
  items->dt    = MPI_DATATYPE_NULL;

  /* get our rank and the size of comm_world */
  int rank, ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ranks);

  /* count number of items in list and identify longest filename */
  int max = 0;
  uint64_t count = 0;
  elem_t* current = head;
  while (current != NULL) {
    const char* file = current->file;
    size_t len = strlen(file) + 1;
    if (len > max) {
      max = (int) len;
    }
    count++;
    current = current->next;
  }

  /* find smallest length that fits max and consists of integer
   * number of 8 byte segments */
  int max8 = max / 8;
  if (max8 * 8 < max) {
    max8++;
  }
  max8 *= 8;

  /* compute longest file path across all ranks */
  int chars;
  MPI_Allreduce(&max8, &chars, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

  /* nothing to do if no one has anything */
  if (chars <= 0) {
    return 0;
  }

  /* build stat type */
  MPI_Datatype dt;
  create_stattype(detail, chars, &dt);

  /* get extent of stat type */
  MPI_Aint lb, extent;
  MPI_Type_get_extent(dt, &lb, &extent);

  /* allocate buffer */
  size_t bufsize = extent * count;
  void* buf = bayer_malloc(bufsize, "array for stat data", __FILE__, __LINE__);

  /* copy stat data into stat datatypes */
  char* ptr = (char*) buf;
  current = head;
  while (current != NULL) {
    /* get pointer to file name and stat structure */
    char* file = current->file;

    uint32_t* ptr_uint32t;
    uint64_t* ptr_uint64t;

    /* copy in file name */
    strcpy(ptr, file);
    ptr += chars;

    if (detail) {
      /* copy in mode time */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = current->mode;
      ptr += sizeof(uint32_t);

      /* copy in user id */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = current->uid;
      ptr += sizeof(uint32_t);

      /* copy in group id */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = current->gid;
      ptr += sizeof(uint32_t);

      /* copy in access time */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = current->atime;
      ptr += sizeof(uint32_t);

      /* copy in modify time */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = current->mtime;
      ptr += sizeof(uint32_t);

      /* copy in create time */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = current->ctime;
      ptr += sizeof(uint32_t);

      /* copy in size */
      ptr_uint64t = (uint64_t*) ptr;
      *ptr_uint64t = current->size;
      ptr += sizeof(uint64_t);
    } else {
      /* just have the file type */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = (uint32_t) current->type;
      ptr += sizeof(uint32_t);
    }

    /* go to next element */
    current = current->next;
  }

  /* set output params */
  items->buf   = buf;
  items->count = count;
  items->chars = (uint64_t)chars;
  items->dt    = dt;

  return 0;
}

/* build a name-to-id map and an id-to-name map */
static void create_maps(
  const buf_t* items,
  map<string,uint32_t>* name2id,
  map<uint32_t,string>* id2name)
{
  int i;
  const char* ptr = (const char*)items->buf;
  for (i = 0; i < items->count; i++) {
    const char* name = ptr;
    ptr += items->chars;

    uint32_t id = *(uint32_t*)ptr;
    ptr += 4;

    (*name2id)[name] = id;
    (*id2name)[id] = name;
  }
  return;
}

/* given an id, lookup its corresponding name, returns id converted
 * to a string if no matching name is found */
static const char* get_name_from_id(uint32_t id, int chars, map<uint32_t,string>* id2name)
{
  map<uint32_t,string>::iterator it = id2name->find(id);
  if (it != id2name->end()) {
    const char* name = (*it).second.c_str();
    return name;
  } else {
    /* store id as name and return that */
    char temp[12];
    size_t len = snprintf(temp, sizeof(temp), "%d", id);
    if (len > (sizeof(temp) - 1) || len > (chars - 1)) {
      /* TODO: ERROR! */
      printf("ERROR!!!\n");
    }

    string newname = temp;
    (*id2name)[id] = newname;

    it = id2name->find(id);
    if (it != id2name->end()) {
      const char* name = (*it).second.c_str();
      return name;
    } else {
      /* TODO: ERROR! */
      printf("ERROR!!!\n");
    }
  }
  return NULL;
}

/****************************************
 * File list user API
 ***************************************/

/* create object that BAYER_FLIST_NULL points to */
static flist_t flist_null;
bayer_flist BAYER_FLIST_NULL = &flist_null;

/* initialize file list */
static bayer_flist bayer_flist_new()
{
  /* allocate memory for file list, cast it to handle, initialize and return */
  flist_t* flist = (flist_t*) bayer_malloc(sizeof(flist_t), "File list handle", __FILE__, __LINE__);

  flist->detail = 0;
  flist->total_files = 0;

  /* initialize linked list */
  flist->list_count = 0;
  flist->list_head  = NULL;
  flist->list_tail  = NULL;
  flist->list_index = NULL;

  /* initialize user, group, and file buffers */
  buft_init(&flist->users);
  buft_init(&flist->groups);

  /* allocate memory for maps */
  flist->user_name2id  = new map<string,uint32_t>;
  flist->user_id2name  = new map<uint32_t,string>;
  flist->group_name2id = new map<string,uint32_t>;
  flist->group_id2name = new map<uint32_t,string>;

  bayer_flist bflist = (bayer_flist) flist;
  return bflist;
}

/* free resouces in file list */
void bayer_flist_free(bayer_flist* pbflist)
{
  /* convert handle to flist_t */
  flist_t* flist = *(flist_t**)pbflist;

  /* delete linked list */
  list_delete(flist);

  buft_free(&flist->users);
  buft_free(&flist->groups);

  delete flist->user_name2id;
  delete flist->user_id2name;
  delete flist->group_name2id;
  delete flist->group_id2name;

  bayer_free(&flist);

  /* set caller's pointer to NULL */
  *pbflist = BAYER_FLIST_NULL;

  return;
}

/* return number of files across all procs */
uint64_t bayer_flist_global_size(bayer_flist bflist)
{
  flist_t* flist = (flist_t*) bflist;
  uint64_t size = flist->total_files;
  return size;
}

/* return number of files in local list */
uint64_t bayer_flist_size(bayer_flist bflist)
{
  flist_t* flist = (flist_t*) bflist;
  uint64_t size = flist->list_count;
  return size;
}

/* return number of users */
uint64_t bayer_flist_user_count(bayer_flist bflist)
{
  flist_t* flist = (flist_t*) bflist;
  uint64_t count = flist->users.count;
  return count;
}

/* return number of groups */
uint64_t bayer_flist_group_count(bayer_flist bflist)
{
  flist_t* flist = (flist_t*) bflist;
  uint64_t count = flist->groups.count;
  return count;
}

/* return maximum length of file names */
uint64_t bayer_flist_file_max_name(bayer_flist bflist)
{
  flist_t* flist = (flist_t*) bflist;
  uint64_t count = flist->max_file_name;
  return count;
}

/* return maximum length of user names */
uint64_t bayer_flist_user_max_name(bayer_flist bflist)
{
  flist_t* flist = (flist_t*) bflist;
  uint64_t count = flist->users.chars;
  return count;
}

/* return maximum length of group names */
uint64_t bayer_flist_group_max_name(bayer_flist bflist)
{
  flist_t* flist = (flist_t*) bflist;
  uint64_t count = flist->groups.chars;
  return count;
}

/* return max/min user,group,filename string
 * return max/min depth
 */

int bayer_flist_have_detail(bayer_flist bflist)
{
  flist_t* flist = (flist_t*) bflist;
  int detail = flist->detail;
  return detail;
}

int bayer_flist_file_name(bayer_flist bflist, int index, char** name)
{
  flist_t* flist = (flist_t*) bflist;
  elem_t* elem = list_get_elem(flist, index);
  if (elem != NULL) {
    *name = elem->file;
    return 0;
  }
  return -1;
}

int bayer_flist_file_depth(bayer_flist bflist, int index, int* depth)
{
  flist_t* flist = (flist_t*) bflist;
  elem_t* elem = list_get_elem(flist, index);
  if (elem != NULL) {
    *depth = elem->depth;
    return 0;
  }
  return -1;
}

int bayer_flist_file_type(bayer_flist bflist, int index, bayer_filetype* type)
{
  flist_t* flist = (flist_t*) bflist;
  elem_t* elem = list_get_elem(flist, index);
  if (elem != NULL) {
    *type = elem->type;
    return 0;
  }
  return -1;
}

int bayer_flist_file_mode(bayer_flist bflist, int index, mode_t* mode)
{
  flist_t* flist = (flist_t*) bflist;
  if (flist->detail > 0) {
    elem_t* elem = list_get_elem(flist, index);
    if (elem != NULL) {
      *mode = (mode_t) elem->mode;
      return 0;
    }
  }
  return -1;
}

uint32_t bayer_flist_file_get_uid(bayer_flist bflist, int index)
{
  uint32_t ret = (uint32_t) -1;
  flist_t* flist = (flist_t*) bflist;
  elem_t* elem = list_get_elem(flist, index);
  if (elem != NULL && flist->detail) {
    ret = elem->uid;
  }
  return ret;
}

uint32_t bayer_flist_file_get_gid(bayer_flist bflist, int index)
{
  uint32_t ret = (uint32_t) -1;
  flist_t* flist = (flist_t*) bflist;
  elem_t* elem = list_get_elem(flist, index);
  if (elem != NULL && flist->detail) {
    ret = elem->gid;
  }
  return ret;
}

uint32_t bayer_flist_file_get_atime(bayer_flist bflist, int index)
{
  uint32_t ret = (uint32_t) -1;
  flist_t* flist = (flist_t*) bflist;
  elem_t* elem = list_get_elem(flist, index);
  if (elem != NULL && flist->detail) {
    ret = elem->atime;
  }
  return ret;
}

uint32_t bayer_flist_file_get_mtime(bayer_flist bflist, int index)
{
  uint32_t ret = (uint32_t) -1;
  flist_t* flist = (flist_t*) bflist;
  elem_t* elem = list_get_elem(flist, index);
  if (elem != NULL && flist->detail) {
    ret = elem->mtime;
  }
  return ret;
}

uint32_t bayer_flist_file_get_ctime(bayer_flist bflist, int index)
{
  uint32_t ret = (uint32_t) -1;
  flist_t* flist = (flist_t*) bflist;
  elem_t* elem = list_get_elem(flist, index);
  if (elem != NULL && flist->detail) {
    ret = elem->ctime;
  }
  return ret;
}

uint64_t bayer_flist_file_get_size(bayer_flist bflist, int index)
{
  uint64_t ret = (uint64_t) -1;
  flist_t* flist = (flist_t*) bflist;
  elem_t* elem = list_get_elem(flist, index);
  if (elem != NULL && flist->detail) {
    ret = elem->size;
  }
  return ret;
}

const char* bayer_flist_file_get_username(bayer_flist bflist, int index)
{
  const char* ret = NULL;
  flist_t* flist = (flist_t*) bflist;
  if (flist->detail) {
    uint32_t id = bayer_flist_file_get_uid(bflist, index);
    ret = get_name_from_id(id, flist->users.chars, flist->user_id2name);
  }
  return ret;
}

const char* bayer_flist_file_get_groupname(bayer_flist bflist, int index)
{
  const char* ret = NULL;
  flist_t* flist = (flist_t*) bflist;
  if (flist->detail) {
    uint32_t id = bayer_flist_file_get_gid(bflist, index);
    ret = get_name_from_id(id, flist->groups.chars, flist->group_id2name);
  }
  return ret;
}

/****************************************
 * Walk directory tree using stat at top level and readdir
 ***************************************/

static void walk_readdir_process_dir(char* dir, CIRCLE_handle* handle)
{
  /* TODO: may need to try these functions multiple times */
  DIR* dirp = bayer_opendir(dir);

  if (! dirp) {
    /* TODO: print error */
  } else {
    /* Read all directory entries */
    while (1) {
      /* read next directory entry */
      struct dirent* entry = bayer_readdir(dirp);
      if (entry == NULL) {
        break;
      }

      /* process component, unless it's "." or ".." */
      char* name = entry->d_name;
      if((strncmp(name, ".", 2)) && (strncmp(name, "..", 3))) {
        /* <dir> + '/' + <name> + '/0' */
        char newpath[CIRCLE_MAX_STRING_LEN];
        size_t len = strlen(dir) + 1 + strlen(name) + 1;
        if (len < sizeof(newpath)) {
          /* build full path to item */
          strcpy(newpath, dir);
          strcat(newpath, "/");
          strcat(newpath, name);

          #ifdef _DIRENT_HAVE_D_TYPE
            /* record info for item */
            mode_t mode;
            int have_mode = 0;
            if (entry->d_type != DT_UNKNOWN) {
              /* we can read object type from directory entry */
              have_mode = 1;
              mode = DTTOIF(entry->d_type);
              list_insert_stat(CURRENT_LIST, newpath, mode, NULL);
            } else {
              /* type is unknown, we need to stat it */
              struct stat st;
              int status = bayer_lstat(newpath, &st);
              if (status == 0) {
                have_mode = 1;
                mode = st.st_mode;
                list_insert_stat(CURRENT_LIST, newpath, mode, &st);
              } else {
                /* error */
              }
            }

            /* recurse into directories */
            if (have_mode && S_ISDIR(mode)) {
              handle->enqueue(newpath);
            }
          #endif
        } else {
          /* TODO: print error in correct format */
          /* name is too long */
          printf("Path name is too long: %lu chars exceeds limit %lu\n", len, sizeof(newpath));
          fflush(stdout);
        }
      }
    }
  }

  bayer_closedir(dirp);

  return;
}

/** Call back given to initialize the dataset. */
static void walk_readdir_create(CIRCLE_handle* handle)
{
  char* path = CURRENT_DIR;

  /* stat top level item */
  struct stat st;
  int status = bayer_lstat(path, &st);
  if (status != 0) {
    /* TODO: print error */
    return;
  }

  /* record item info */
  list_insert_stat(CURRENT_LIST, path, st.st_mode, &st);

  /* recurse into directory */
  if (S_ISDIR(st.st_mode)) {
    walk_readdir_process_dir(path, handle);
  }

  return;
}

/** Callback given to process the dataset. */
static void walk_readdir_process(CIRCLE_handle* handle)
{
  /* in this case, only items on queue are directories */
  char path[CIRCLE_MAX_STRING_LEN];
  handle->dequeue(path);
  walk_readdir_process_dir(path, handle);
  return;
}

/****************************************
 * Walk directory tree using stat on every object
 ***************************************/

static void walk_stat_process_dir(char* dir, CIRCLE_handle* handle)
{
  /* TODO: may need to try these functions multiple times */
  DIR* dirp = bayer_opendir(dir);

  if (! dirp) {
    /* TODO: print error */
  } else {
    while (1) {
      /* read next directory entry */
      struct dirent* entry = bayer_readdir(dirp);
      if (entry == NULL) {
        break;
      }
       
      /* We don't care about . or .. */
      char* name = entry->d_name;
      if ((strncmp(name, ".", 2)) && (strncmp(name, "..", 3))) {
        /* <dir> + '/' + <name> + '/0' */
        char newpath[CIRCLE_MAX_STRING_LEN];
        size_t len = strlen(dir) + 1 + strlen(name) + 1;
        if (len < sizeof(newpath)) {
          /* build full path to item */
          strcpy(newpath, dir);
          strcat(newpath, "/");
          strcat(newpath, name);

          /* add item to queue */
          handle->enqueue(newpath);
        } else {
          /* TODO: print error in correct format */
          /* name is too long */
          printf("Path name is too long: %lu chars exceeds limit %lu\n", len, sizeof(newpath));
          fflush(stdout);
        }
      }
    }
  }

  bayer_closedir(dirp);

  return;
}

/** Call back given to initialize the dataset. */
static void walk_stat_create(CIRCLE_handle* handle)
{
  /* we'll call stat on every item */
  handle->enqueue(CURRENT_DIR);
}

/** Callback given to process the dataset. */
static void walk_stat_process(CIRCLE_handle* handle)
{
  /* get path from queue */
  char path[CIRCLE_MAX_STRING_LEN];
  handle->dequeue(path);

  /* stat item */
  struct stat st;
  int status = bayer_lstat(path, &st);
  if (status != 0) {
    /* print error */
    return;
  }

  /* TODO: filter items by stat info */

  /* record info for item in list */
  list_insert_stat(CURRENT_LIST, path, st.st_mode, &st);

  /* recurse into directory */
  if (S_ISDIR(st.st_mode)) {
    /* TODO: check that we can recurse into directory */
    walk_stat_process_dir(path, handle);
  }

  return;
}

/****************************************
 * Functions to read user and group info
 ***************************************/

/* create a type consisting of chars number of characters
 * immediately followed by a uint32_t */
static void create_stridtype(int chars, MPI_Datatype* dt)
{
  /* build type for string */
  MPI_Datatype dt_str;
  MPI_Type_contiguous(chars, MPI_CHAR, &dt_str);

  /* build keysat type */
  MPI_Datatype types[2];
  types[0] = dt_str;       /* file name */
  types[1] = MPI_UINT32_T; /* id */
  DTCMP_Type_create_series(2, types, dt);

  MPI_Type_free(&dt_str);
  return;
}

/* element for a linked list of name/id pairs */
typedef struct strid {
  char* name;
  uint32_t id;
  struct strid* next;
} strid_t;

/* insert specified name and id into linked list given by
 * head, tail, and count, also increase maxchars if needed */
static void strid_insert(
  const char* name,
  uint32_t id,
  strid_t** head,
  strid_t** tail,
  int* count,
  int* maxchars)
{
  /* add username and id to linked list */
  strid_t* elem = (strid_t*) malloc(sizeof(strid_t));
  elem->name = strdup(name);
  elem->id = id;
  elem->next = NULL;
  if (*head == NULL) {
    *head = elem;
  }
  if (*tail != NULL) {
    (*tail)->next = elem;
  }
  *tail = elem;
  (*count)++;

  /* increase maximum username if we need to */
  size_t len = strlen(name) + 1;
  if (*maxchars < (int)len) {
    /* round up to nearest multiple of 4 */
    size_t len4 = len / 4;
    if (len4 * 4 < len) {
      len4++;
    }
    len4 *= 4;

    *maxchars = (int)len4;
  }

  return;
}

/* copy data from linked list to array */
static void strid_serialize(strid_t* head, int chars, void* buf)
{
  char* ptr = (char*)buf;
  strid_t* current = head;
  while (current != NULL) {
    char* name  = current->name;
    uint32_t id = current->id;

    strcpy(ptr, name);
    ptr += chars;

    uint32_t* p32 = (uint32_t*) ptr;
    *p32 = id;
    ptr += 4;

    current = current->next;
  }
  return;
}

/* delete linked list and reset head, tail, and count values */
static void strid_delete(strid_t** head, strid_t** tail, int* count)
{
  /* free memory allocated in linked list */
  strid_t* current = *head;
  while (current != NULL) {
    strid_t* next = current->next;
    bayer_free(&current->name);
    bayer_free(&current);
    current = next;
  }

  /* set list data structure values back to NULL */
  *head  = NULL;
  *tail  = NULL;
  *count = 0;

  return;
}

/* read user array from file system using getpwent() */
static void get_users(buf_t* items)
{
  /* initialize output parameters */
  buft_init(items);

  /* get our rank */
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  /* rank 0 iterates over users with getpwent */
  strid_t* head = NULL;
  strid_t* tail = NULL;
  int count = 0;
  int chars = 0;
  if (rank == 0) {
    struct passwd* p;
    while (1) {
      /* get next user, this function can fail so we retry a few times */
      int retries = 3;
retry:
      p = getpwent();
      if (p == NULL) {
        if (errno == EIO) {
          retries--;
        } else {
          /* TODO: ERROR! */
          retries = 0;
        }
        if (retries > 0) {
          goto retry;
        }
      }

      if (p != NULL) {
        /*
        printf("User=%s Pass=%s UID=%d GID=%d Name=%s Dir=%s Shell=%s\n",
          p->pw_name, p->pw_passwd, p->pw_uid, p->pw_gid, p->pw_gecos, p->pw_dir, p->pw_shell
        );
        printf("User=%s UID=%d GID=%d\n",
          p->pw_name, p->pw_uid, p->pw_gid
        );
        */
        char* name  = p->pw_name;
        uint32_t id = p->pw_uid;
        strid_insert(name, id, &head, &tail, &count, &chars);
      } else {
        /* hit the end of the user list */
        endpwent();
        break;
      }
    }

//    printf("Max username %d, count %d\n", (int)chars, count);
  }

  /* bcast count and number of chars */
  MPI_Bcast(&count, 1, MPI_INT, 0, MPI_COMM_WORLD);
  MPI_Bcast(&chars, 1, MPI_INT, 0, MPI_COMM_WORLD);

  /* create datatype to represent a username/id pair */
  MPI_Datatype dt;
  create_stridtype(chars, &dt);

  /* get extent of type */
  MPI_Aint lb, extent;
  MPI_Type_get_extent(dt, &lb, &extent);

  /* allocate an array to hold all user names and ids */
  char* buf = NULL;
  size_t bufsize = count * extent;
  if (bufsize > 0) {
    buf = (char*) malloc(bufsize);
  }

  /* copy items from list into array */
  if (rank == 0) {
    strid_serialize(head, chars, buf);
  }

  /* broadcast the array of usernames and ids */
  MPI_Bcast(buf, count, dt, 0, MPI_COMM_WORLD);

  /* set output parameters */
  items->buf   = buf;
  items->count = (uint64_t) count;
  items->chars = (uint64_t) chars; 
  items->dt    = dt;

  /* delete the linked list */
  if (rank == 0) {
    strid_delete(&head, &tail, &count);
  }

  return;
}

/* read group array from file system using getgrent() */
static void get_groups(buf_t* items)
{
  /* initialize output parameters */
  buft_init(items);

  /* get our rank */
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  /* rank 0 iterates over users with getpwent */
  strid_t* head = NULL;
  strid_t* tail = NULL;
  int count = 0;
  int chars = 0;
  if (rank == 0) {
    struct group* p;
    while (1) {
      /* get next user, this function can fail so we retry a few times */
      int retries = 3;
retry:
      p = getgrent();
      if (p == NULL) {
        if (errno == EIO || errno == EINTR) {
          retries--;
        } else {
          /* TODO: ERROR! */
          retries = 0;
        }
        if (retries > 0) {
          goto retry;
        }
      }

      if (p != NULL) {
/*
        printf("Group=%s GID=%d\n",
          p->gr_name, p->gr_gid
        );
*/
        char* name  = p->gr_name;
        uint32_t id = p->gr_gid;
        strid_insert(name, id, &head, &tail, &count, &chars);
      } else {
        /* hit the end of the group list */
        endgrent();
        break;
      }
    }

//    printf("Max groupname %d, count %d\n", chars, count);
  }

  /* bcast count and number of chars */
  MPI_Bcast(&count, 1, MPI_INT, 0, MPI_COMM_WORLD);
  MPI_Bcast(&chars, 1, MPI_INT, 0, MPI_COMM_WORLD);

  /* create datatype to represent a username/id pair */
  MPI_Datatype dt;
  create_stridtype(chars, &dt);

  /* get extent of type */
  MPI_Aint lb, extent;
  MPI_Type_get_extent(dt, &lb, &extent);

  /* allocate an array to hold all user names and ids */
  char* buf = NULL;
  size_t bufsize = count * extent;
  if (bufsize > 0) {
    buf = (char*) malloc(bufsize);
  }

  /* copy items from list into array */
  if (rank == 0) {
    strid_serialize(head, chars, buf);
  }

  /* broadcast the array of usernames and ids */
  MPI_Bcast(buf, count, dt, 0, MPI_COMM_WORLD);

  /* set output parameters */
  items->buf   = buf;
  items->count = (uint64_t) count;
  items->chars = (uint64_t) chars; 
  items->dt    = dt;

  /* delete the linked list */
  if (rank == 0) {
    strid_delete(&head, &tail, &count);
  }

  return;
}

/* Set up and execute directory walk */
void bayer_flist_walk_path(const char* dirpath, int use_stat, bayer_flist* pbflist)
{
  /* check that we got a valid pointer */
  if (pbflist == NULL) {
  }

  /* allocate a new file list */
  *pbflist = bayer_flist_new();

  /* convert handle to flist_t */
  flist_t* flist = *(flist_t**)pbflist;

  /* initialize libcircle */
  CIRCLE_init(0, NULL, CIRCLE_SPLIT_EQUAL);

  /* set libcircle verbosity level */
  enum CIRCLE_loglevel loglevel = CIRCLE_LOG_WARN;
  CIRCLE_enable_logging(loglevel);

  /* set some global variables to do the file walk */
  strncpy(CURRENT_DIR, dirpath, PATH_MAX);
  CURRENT_LIST = flist;

  /* we lookup users and groups first in case we can use
   * them to filter the walk */
  flist->detail = 0;
  if (use_stat) {
    flist->detail = 1;
    get_users(&flist->users);
    get_groups(&flist->groups);
    create_maps(&flist->users, flist->user_name2id, flist->user_id2name);
    create_maps(&flist->groups, flist->group_name2id, flist->group_id2name);
  }

  /* register callbacks */
  if (use_stat) {
    /* walk directories by calling stat on every item */
    CIRCLE_cb_create(&walk_stat_create);
    CIRCLE_cb_process(&walk_stat_process);
  } else {
    /* walk directories using file types in readdir */
    CIRCLE_cb_create(&walk_readdir_create);
    CIRCLE_cb_process(&walk_readdir_process);
  }

  /* run the libcircle job */
  CIRCLE_begin();
  CIRCLE_finalize();

  /* get total file count */
  uint64_t total;
  uint64_t count = flist->list_count;
  MPI_Allreduce(&count, &total, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
  flist->total_files = total;

  return;
}

/****************************************
 * Read file list from file
 ***************************************/

/* file format:
 *   uint64_t timestamp when walk started
 *   uint64_t timestamp when walk ended
 *   uint64_t total number of files
 *   uint64_t max filename length
 *   list of <filenames(str), filetype(uint32_t)> */
static void read_cache_v2(
  const char* name,
  MPI_Offset* outdisp,
  MPI_File fh,
  char* datarep,
  uint64_t* outstart,
  uint64_t* outend,
  flist_t* flist)
{
  MPI_Status status;

  MPI_Offset disp = *outdisp;

  /* indicate that we just have file names */
  flist->detail = 0;

  /* create buf_t object to hold data from file */
  buf_t files;
  buft_init(&files);

  /* get our rank */
  int rank, ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ranks);

  /* rank 0 reads and broadcasts header */
  uint64_t header[4];
  MPI_File_set_view(fh, disp, MPI_UINT64_T, MPI_UINT64_T, datarep, MPI_INFO_NULL);
  if (rank == 0) {
    MPI_File_read_at(fh, disp, header, 4, MPI_UINT64_T, &status);
  }
  MPI_Bcast(header, 4, MPI_UINT64_T, 0, MPI_COMM_WORLD);
  disp += 4 * 8; /* 4 consecutive uint64_t types in external32 */

  uint64_t all_count;
  *outstart   = header[0];
  *outend     = header[1];
  all_count   = header[2];
  files.chars = header[3];

  /* compute count for each process */
  uint64_t count = all_count / ranks;
  uint64_t remainder = all_count - count * ranks;
  if (rank < remainder) {
    count++;
  }
  files.count = count;

  /* get our offset */
  uint64_t offset;
  MPI_Exscan(&count, &offset, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
  if (rank == 0) {
    offset = 0;
  }

  /* create file datatype and read in file info if there are any */
  if (all_count > 0 && files.chars > 0) {
    /* create types */
    create_stattype(flist->detail, (int)files.chars,   &(files.dt));

    /* get extents */
    MPI_Aint lb_file, extent_file;
    MPI_Type_get_extent(files.dt, &lb_file, &extent_file);

    /* allocate memory to hold data */
    size_t bufsize_file = files.count * extent_file;
    files.buf = (void*) bayer_malloc(bufsize_file, "File data buffer", __FILE__, __LINE__);

    /* collective read of stat info */
    MPI_File_set_view(fh, disp, files.dt, files.dt, datarep, MPI_INFO_NULL);
    MPI_Offset read_offset = disp + offset * extent_file;
    MPI_File_read_at_all(fh, read_offset, files.buf, (int)files.count, files.dt, &status);
    disp += all_count * extent_file;
  }

  /* for each file, insert an entry into our list */
  uint64_t i = 0;
  char* ptr = (char*) files.buf;
  while (i < count) {
    const char* name = ptr;
    ptr += files.chars;

    uint32_t type = *(uint32_t*)ptr;
    ptr += 4;

    list_insert_lite(flist, name, (bayer_filetype)type);

    i++;
  }

  /* free buffer */
  buft_free(&files);

  *outdisp = disp;
  return;
}

/* file format:
 *   uint64_t timestamp when walk started
 *   uint64_t timestamp when walk ended
 *   uint64_t total number of users
 *   uint64_t max username length
 *   uint64_t total number of groups
 *   uint64_t max groupname length
 *   uint64_t total number of files
 *   uint64_t max filename length
 *   list of <username(str), userid(uint32_t)>
 *   list of <groupname(str), groupid(uint32_t)>
 *   list of <files(str)>
 *   */
static void read_cache_v3(
  const char* name,
  MPI_Offset* outdisp,
  MPI_File fh,
  char* datarep,
  uint64_t* outstart,
  uint64_t* outend,
  flist_t* flist)
{
  MPI_Status status;

  MPI_Offset disp = *outdisp;

  /* indicate that we have stat data */
  flist->detail = 1;

  /* pointer to users, groups, and file buffer data structure */
  buf_t* users  = &flist->users;
  buf_t* groups = &flist->groups;

  /* create buf_t object to hold data from file */
  buf_t files;
  buft_init(&files);

  /* get our rank */
  int rank, ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ranks);

  /* rank 0 reads and broadcasts header */
  uint64_t header[8];
  MPI_File_set_view(fh, disp, MPI_UINT64_T, MPI_UINT64_T, datarep, MPI_INFO_NULL);
  if (rank == 0) {
    MPI_File_read_at(fh, disp, header, 8, MPI_UINT64_T, &status);
  }
  MPI_Bcast(header, 8, MPI_UINT64_T, 0, MPI_COMM_WORLD);
  disp += 8 * 8; /* 8 consecutive uint64_t types in external32 */

  uint64_t all_count;
  *outstart     = header[0];
  *outend       = header[1];
  users->count  = header[2];
  users->chars  = header[3];
  groups->count = header[4];
  groups->chars = header[5];
  all_count     = header[6];
  files.chars   = header[7];

  /* compute count for each process */
  uint64_t count = all_count / ranks;
  uint64_t remainder = all_count - count * ranks;
  if (rank < remainder) {
    count++;
  }
  files.count = count;

  /* get our offset */
  uint64_t offset;
  MPI_Exscan(&count, &offset, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
  if (rank == 0) {
    offset = 0;
  }

  /* read users, if any */
  if (users->count > 0 && users->chars > 0) {
    /* create type */
    create_stridtype((int)users->chars,  &(users->dt));

    /* get extent */
    MPI_Aint lb_user, extent_user;
    MPI_Type_get_extent(users->dt, &lb_user, &extent_user);

    /* allocate memory to hold data */
    size_t bufsize_user = users->count * extent_user;
    users->buf = (void*) bayer_malloc(bufsize_user, "User data buffer", __FILE__, __LINE__);

    /* read data */
    MPI_File_set_view(fh, disp, users->dt, users->dt, datarep, MPI_INFO_NULL);
    if (rank == 0) {
      MPI_File_read_at(fh, disp, users->buf, (int)users->count, users->dt, &status);
    }
    MPI_Bcast(users->buf, (int)users->count, users->dt, 0, MPI_COMM_WORLD);
    disp += bufsize_user;
  }

  /* read groups, if any */
  if (groups->count > 0 && groups->chars > 0) {
    /* create type */
    create_stridtype((int)groups->chars, &(groups->dt));

    /* get extent */
    MPI_Aint lb_group, extent_group;
    MPI_Type_get_extent(groups->dt, &lb_group, &extent_group);

    /* allocate memory to hold data */
    size_t bufsize_group = groups->count * extent_group;
    groups->buf = (void*) bayer_malloc(bufsize_group, "Group data buffer", __FILE__, __LINE__);

    /* read data */
    MPI_File_set_view(fh, disp, groups->dt, groups->dt, datarep, MPI_INFO_NULL);
    if (rank == 0) {
      MPI_File_read_at(fh, disp, groups->buf, (int)groups->count, groups->dt, &status);
    }
    MPI_Bcast(groups->buf, (int)groups->count, groups->dt, 0, MPI_COMM_WORLD);
    disp += bufsize_group;
  }

  /* read files, if any */
  if (all_count > 0 && files.chars > 0) {
    /* create types */
    create_stattype(flist->detail, (int)files.chars,   &(files.dt));

    /* get extents */
    MPI_Aint lb_file, extent_file;
    MPI_Type_get_extent(files.dt, &lb_file, &extent_file);

    /* allocate memory to hold data */
    size_t bufsize_file = files.count * extent_file;
    files.buf = (void*) bayer_malloc(bufsize_file, "File data buffer", __FILE__, __LINE__);

    /* collective read of stat info */
    MPI_File_set_view(fh, disp, files.dt, files.dt, datarep, MPI_INFO_NULL);
    MPI_Offset read_offset = disp + offset * extent_file;
    MPI_File_read_at_all(fh, read_offset, files.buf, (int)files.count, files.dt, &status);
    disp += all_count * extent_file;

    /* for each file, insert an entry into our list */
    uint64_t i = 0;
    char* ptr = (char*) files.buf;
    while (i < count) {
      list_insert_ptr(flist, ptr, files.chars);
      ptr += extent_file;
      i++;
    }
  }

  /* create maps of users and groups */
  create_maps(&flist->users, flist->user_name2id, flist->user_id2name);
  create_maps(&flist->groups, flist->group_name2id, flist->group_id2name);

  /* free buffer */
  buft_free(&files);

  *outdisp = disp;
  return;
}

void bayer_flist_read_cache(
  const char* name,
  bayer_flist* pbflist)
{
  /* check that we got a valid pointer */
  if (pbflist == NULL) {
  }

  /* allocate a new file list */
  *pbflist = bayer_flist_new();

  /* convert handle to flist_t */
  flist_t* flist = *(flist_t**)pbflist;

  /* get our rank */
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  /* open file */
  int rc;
  MPI_Status status;
  MPI_File fh;
  char datarep[] = "external32";
  int amode = MPI_MODE_RDONLY;
  rc = MPI_File_open(MPI_COMM_WORLD, (char*)name, amode, MPI_INFO_NULL, &fh);
  if (rc != MPI_SUCCESS) {
    if (rank == 0) {
      printf("Failed to open file %s", name);
    }
    return;
  }

  /* set file view */
  MPI_Offset disp = 0;

  /* rank 0 reads and broadcasts version */
  uint64_t version;
  MPI_File_set_view(fh, disp, MPI_UINT64_T, MPI_UINT64_T, datarep, MPI_INFO_NULL);
  if (rank == 0) {
    MPI_File_read_at(fh, disp, &version, 1, MPI_UINT64_T, &status);
  }
  MPI_Bcast(&version, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
  disp += 1 * 8; /* 9 consecutive uint64_t types in external32 */

  /* need a couple of dummy params to record walk start and end times */
  uint64_t outstart = 0;
  uint64_t outend = 0;

  /* read data from file */
  if (version == 2) {
    read_cache_v2(name, &disp, fh, datarep, &outstart, &outend, flist);
  } else if (version == 3) {
    read_cache_v3(name, &disp, fh, datarep, &outstart, &outend, flist);
  } else {
    /* TODO: unknown file format */
  }

  /* close file */
  MPI_File_close(&fh);

  return;
}

/****************************************
 * Write file list to file
 ***************************************/

/* file version
 * 1: version, start, end, files, file chars, list (file)
 * 2: version, start, end, files, file chars, list (file, type)
 * 3: version, start, end, files, users, user chars, groups, group chars,
 *    files, file chars, list (user, userid), list (group, groupid),
 *    list (stat) */

/* file format:
 *   uint64_t timestamp when walk started
 *   uint64_t timestamp when walk ended
 *   uint64_t total number of files
 *   uint64_t max filename length
 *   list of <filenames(str), filetype(uint32_t)> */

static void write_cache_readdir(
  const char* name,
  uint64_t walk_start,
  uint64_t walk_end,
  flist_t* flist)
{
  /* convert list to contiguous buffer */
  buf_t files;
  buft_init(&files);
  list_convert_to_dt(flist, &files);

  /* get our rank and number of ranks in job */
  int rank, ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ranks);

  /* get total file count */
  uint64_t count = files.count;
  uint64_t all_count = flist->total_files;

  /* get our offset */
  uint64_t offset;
  MPI_Exscan(&count, &offset, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
  if (rank == 0) {
    offset = 0;
  }

  /* open file */
  MPI_Status status;
  MPI_File fh;
  char datarep[] = "external32";
  //int amode = MPI_MODE_WRONLY | MPI_MODE_CREATE | MPI_MODE_SEQUENTIAL;
  int amode = MPI_MODE_WRONLY | MPI_MODE_CREATE;
  MPI_File_open(MPI_COMM_WORLD, (char*)name, amode, MPI_INFO_NULL, &fh);

  /* truncate file to 0 bytes */
  MPI_File_set_size(fh, 0);

  /* prepare header */
  uint64_t header[5];
  header[0] = 2;            /* file version */
  header[1] = walk_start;   /* time_t when file walk started */
  header[2] = walk_end;     /* time_t when file walk stopped */
  header[3] = all_count;    /* total number of stat entries */
  header[4] = files.chars;  /* number of chars in file name */

  /* write the header */
  MPI_Offset disp = 0;
  MPI_File_set_view(fh, disp, MPI_UINT64_T, MPI_UINT64_T, datarep, MPI_INFO_NULL);
  if (rank == 0) {
    MPI_File_write_at(fh, disp, header, 5, MPI_UINT64_T, &status);
  }
  disp += 5 * 8;

  if (files.dt != MPI_DATATYPE_NULL) {
    /* get extents of file datatypes */
    MPI_Aint lb_file, extent_file;
    MPI_Type_get_extent(files.dt, &lb_file, &extent_file);

    /* collective write of file info */
    MPI_File_set_view(fh, disp, files.dt, files.dt, datarep, MPI_INFO_NULL);
    MPI_Offset write_offset = disp + offset * extent_file;
    int write_count = (int) count;
    MPI_File_write_at_all(fh, write_offset, files.buf, write_count, files.dt, &status);
    disp += all_count * extent_file;
  }

  /* close file */
  MPI_File_close(&fh);

  /* free buffer */
  buft_free(&files);

  return;
}

static void write_cache_stat(
  const char* name,
  uint64_t walk_start,
  uint64_t walk_end,
  flist_t* flist)
{
  buf_t* users  = &flist->users;
  buf_t* groups = &flist->groups;

  /* convert list to contiguous buffer */
  buf_t files;
  buft_init(&files);
  list_convert_to_dt(flist, &files);

  /* get our rank and number of ranks in job */
  int rank, ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ranks);

  /* get total file count */
  uint64_t count = files.count;
  uint64_t all_count = flist->total_files;

  /* get our offset */
  uint64_t offset;
  MPI_Exscan(&count, &offset, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
  if (rank == 0) {
    offset = 0;
  }

  /* open file */
  MPI_Status status;
  MPI_File fh;
  char datarep[] = "external32";
  //int amode = MPI_MODE_WRONLY | MPI_MODE_CREATE | MPI_MODE_SEQUENTIAL;
  int amode = MPI_MODE_WRONLY | MPI_MODE_CREATE;
  MPI_File_open(MPI_COMM_WORLD, (char*)name, amode, MPI_INFO_NULL, &fh);

  /* truncate file to 0 bytes */
  MPI_File_set_size(fh, 0);

  /* prepare header */
  uint64_t header[9];
  header[0] = 3;             /* file version */
  header[1] = walk_start;    /* time_t when file walk started */
  header[2] = walk_end;      /* time_t when file walk stopped */
  header[3] = users->count;  /* number of user records */
  header[4] = users->chars;  /* number of chars in user name */
  header[5] = groups->count; /* number of group records */
  header[6] = groups->chars; /* number of chars in group name */
  header[7] = all_count;     /* total number of stat entries */
  header[8] = files.chars;   /* number of chars in file name */

  /* write the header */
  MPI_Offset disp = 0;
  MPI_File_set_view(fh, disp, MPI_UINT64_T, MPI_UINT64_T, datarep, MPI_INFO_NULL);
  if (rank == 0) {
    MPI_File_write_at(fh, disp, header, 9, MPI_UINT64_T, &status);
  }
  disp += 9 * 8;

  if (users->dt != MPI_DATATYPE_NULL) {
    /* get extent user */
    MPI_Aint lb_user, extent_user;
    MPI_Type_get_extent(users->dt, &lb_user, &extent_user);

    /* write out users */
    MPI_File_set_view(fh, disp, users->dt, users->dt, datarep, MPI_INFO_NULL);
    if (rank == 0) {
      MPI_File_write_at(fh, disp, users->buf, users->count, users->dt, &status);
    }
    disp += users->count * extent_user;
  }

  if (groups->dt != MPI_DATATYPE_NULL) {
    /* get extent group */
    MPI_Aint lb_group, extent_group;
    MPI_Type_get_extent(groups->dt, &lb_group, &extent_group);

    /* write out groups */
    MPI_File_set_view(fh, disp, groups->dt, groups->dt, datarep, MPI_INFO_NULL);
    if (rank == 0) {
      MPI_File_write_at(fh, disp, groups->buf, groups->count, groups->dt, &status);
    }
    disp += groups->count * extent_group;
  }

  if (files.dt != MPI_DATATYPE_NULL) {
    /* get extent file */
    MPI_Aint lb_file, extent_file;
    MPI_Type_get_extent(files.dt, &lb_file, &extent_file);

    /* collective write of stat info */
    MPI_File_set_view(fh, disp, files.dt, files.dt, datarep, MPI_INFO_NULL);
    MPI_Offset write_offset = disp + offset * extent_file;
    int write_count = (int) count;
    MPI_File_write_at_all(fh, write_offset, files.buf, write_count, files.dt, &status);
    disp += all_count * extent_file;
  }

  /* close file */
  MPI_File_close(&fh);

  /* free buffer */
  buft_free(&files);

  return;
}

void bayer_flist_write_cache(
  const char* name,
  bayer_flist bflist)
{
  /* convert handle to flist_t */
  flist_t* flist = (flist_t*) bflist;

  if (flist->detail) {
    write_cache_stat(name, 0, 0, flist);
  } else {
    write_cache_readdir(name, 0, 0, flist);
  }

  return;
}
