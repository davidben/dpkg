/*
 * dpkg - main program for package management
 * filesdb.c - management of database of files installed on system
 *
 * Copyright © 1995 Ian Jackson <ian@chiark.greenend.org.uk>
 * Copyright © 2000,2001 Wichert Akkerman <wakkerma@debian.org>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with dpkg; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <config.h>
#include <compat.h>

#include <dpkg/i18n.h>

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include <pwd.h>
#include <grp.h>
#include <sys/types.h>

#include <dpkg/dpkg.h>
#include <dpkg/dpkg-db.h>
#include <dpkg/path.h>
#include <dpkg/progress.h>

#include "filesdb.h"
#include "main.h"

/*** Generic data structures and routines ***/

static int allpackagesdone= 0;
static int nfiles= 0;

void
ensure_package_clientdata(struct pkginfo *pkg)
{
  if (pkg->clientdata)
    return;
  pkg->clientdata = nfmalloc(sizeof(struct perpackagestate));
  pkg->clientdata->istobe = itb_normal;
  pkg->clientdata->fileslistvalid = 0;
  pkg->clientdata->files = NULL;
  pkg->clientdata->trigprocdeferred = NULL;
}

void note_must_reread_files_inpackage(struct pkginfo *pkg) {
  allpackagesdone= 0;
  ensure_package_clientdata(pkg);
  pkg->clientdata->fileslistvalid= 0;
}

static int saidread=0;

 /* erase the files saved in pkg
  */
static void _erase_pkg_file_data(struct pkginfo *pkg) {
  struct fileinlist *current;
  struct filepackages *packageslump;
  int search, findlast;

  if (!pkg->clientdata)
    return; /* nothing to empty */
  for (current= pkg->clientdata->files;
       current;
       current= current->next) {
    /* For each file that used to be in the package,
     * go through looking for this package's entry in the list
     * of packages containing this file, and blank it out.
     */
    for (packageslump= current->namenode->packages;
         packageslump;
         packageslump= packageslump->more)
      for (search= 0;
           search < PERFILEPACKAGESLUMP && packageslump->pkgs[search];
           search++)
        if (packageslump->pkgs[search] == pkg) {
          /* Hah!  Found it. */
          for (findlast= search+1;
               findlast < PERFILEPACKAGESLUMP && packageslump->pkgs[findlast];
               findlast++);
          findlast--;
          /* findlast is now the last occupied entry, which may be the same as
           * search.  We blank out the entry for this package.  We also
           * have to copy the last entry into the empty slot, because
           * the list is null-pointer-terminated.
           */
          packageslump->pkgs[search]= packageslump->pkgs[findlast];
          packageslump->pkgs[findlast] = NULL;
          /* This may result in an empty link in the list.  This is OK. */
          goto xit_search_to_delete_from_perfilenodelist;
        }
  xit_search_to_delete_from_perfilenodelist:
    ;
    /* The actual filelist links were allocated using nfmalloc, so
     * we shouldn't free them.
     */
  }
  pkg->clientdata->files = NULL;
}

 /* add a file to pkg. pos_hint is to help find the end of the list
  */
typedef struct fileinlist **fileinlist_hint;
static fileinlist_hint _add_file_to_package(struct pkginfo *pkg, const char *file, enum fnnflags flags, fileinlist_hint pos_hint) {
  struct fileinlist *newent;
  struct filepackages *packageslump;
  int putat;

  ensure_package_clientdata(pkg);
  if (pos_hint == NULL)
    pos_hint = &pkg->clientdata->files;
  /* Make sure we're at the end */
  while ((*pos_hint) != NULL) {
    pos_hint = &((*pos_hint)->next);
  }

  /* Create a new node */
  newent = nfmalloc(sizeof(struct fileinlist));
  newent->namenode = findnamenode(file, flags);
  newent->next = NULL;
  *pos_hint = newent;
  pos_hint = &newent->next;

  /* Add pkg to newent's package list */
  packageslump = newent->namenode->packages;
  putat = 0;
  if (packageslump) {
    for (; putat < PERFILEPACKAGESLUMP && packageslump->pkgs[putat];
         putat++);
    if (putat >= PERFILEPACKAGESLUMP)
      packageslump = NULL;
  }
  if (!packageslump) {
    packageslump = nfmalloc(sizeof(struct filepackages));
    packageslump->more = newent->namenode->packages;
    newent->namenode->packages = packageslump;
    putat= 0;
  }
  packageslump->pkgs[putat]= pkg;
  if (++putat < PERFILEPACKAGESLUMP)
    packageslump->pkgs[putat] = NULL;

  /* Return the position for the next guy */
  return pos_hint;
}

 /* load the list of files in this package into memory, or update the
  * list if it is there but stale
  */
void ensure_packagefiles_available(struct pkginfo *pkg) {
  int fd;
  const char *filelistfile;
  fileinlist_hint lendp;
  struct stat stat_buf;
  char *loaded_list, *loaded_list_end, *thisline, *nextline, *ptr;

  if (pkg->clientdata && pkg->clientdata->fileslistvalid) return;
  ensure_package_clientdata(pkg);

  /* Throw away any the stale data, if there was any. */
  _erase_pkg_file_data(pkg);

  /* Packages which aren't installed don't have a files list. */
  if (pkg->status == stat_notinstalled) {
    pkg->clientdata->fileslistvalid= 1; return;
  }

  filelistfile= pkgadminfile(pkg,LISTFILE);

  onerr_abort++;
  
  fd= open(filelistfile,O_RDONLY);

  if (fd==-1) {
    if (errno != ENOENT)
      ohshite(_("unable to open files list file for package `%.250s'"),pkg->name);
    onerr_abort--;
    if (pkg->status != stat_configfiles) {
      if (saidread == 1) putc('\n',stderr);
      warning(_("files list file for package `%.250s' missing, assuming "
                "package has no files currently installed."), pkg->name);
    }
    pkg->clientdata->files = NULL;
    pkg->clientdata->fileslistvalid= 1;
    return;
  }

  push_cleanup(cu_closefd, ehflag_bombout, NULL, 0, 1, &fd);
  
   if(fstat(fd, &stat_buf))
     ohshite(_("unable to stat files list file for package '%.250s'"),
             pkg->name);

   if (stat_buf.st_size) {
     loaded_list = nfmalloc(stat_buf.st_size);
     loaded_list_end = loaded_list + stat_buf.st_size;
  
    fd_buf_copy(fd, loaded_list, stat_buf.st_size, _("files list for package `%.250s'"), pkg->name);
  
    lendp = &pkg->clientdata->files;
    thisline = loaded_list;
    while (thisline < loaded_list_end) {
      if (!(ptr = memchr(thisline, '\n', loaded_list_end - thisline))) 
        ohshit(_("files list file for package '%.250s' is missing final newline"),
               pkg->name);
      /* where to start next time around */
      nextline = ptr + 1;
      /* strip trailing "/" */
      if (ptr > thisline && ptr[-1] == '/') ptr--;
      /* add the file to the list */
      if (ptr == thisline)
        ohshit(_("files list file for package `%.250s' contains empty filename"),pkg->name);
      *ptr = '\0';
      lendp = _add_file_to_package(pkg, thisline, fnn_nocopy, lendp);
      thisline = nextline;
    }
  }
  pop_cleanup(ehflag_normaltidy); /* fd= open() */
  if (close(fd))
    ohshite(_("error closing files list file for package `%.250s'"),pkg->name);

  onerr_abort--;

  pkg->clientdata->fileslistvalid= 1;
}

void ensure_allinstfiles_available(void) {
  struct pkgiterator *it;
  struct pkginfo *pkg;
  struct progress progress;
  int tar_pid;
  int pipefd[2];

  if (allpackagesdone) return;
  if (saidread<2) {
    int max = countpackages();

    saidread=1;
    progress_init(&progress, _("(Reading database ... "), max);
  }

  m_pipe(pipefd);
  if (!(tar_pid = m_fork())) {
    /* Child process */
    m_dup2(pipefd[1], STDOUT_FILENO);
    m_dup2(STDOUT_FILENO, STDERR_FILENO);
    close(pipefd[1]);
    close(pipefd[0]);
    execlp(TAR, "tar", "xf", "/tmp/list-files.tar", "-O", "-v", NULL);
    exit(1); /* TODO: call ohshite() or something */
  } else {
    FILE *pipefile;
    struct pkginfo *pkg = NULL;
    /* FIXME: handle long lines */
    char buf[1024];
    fileinlist_hint pos_hint = NULL;

    close(pipefd[1]);
    pipefile = fdopen(pipefd[0], "r");
    while (fgets(buf, sizeof(buf), pipefile)) {
      size_t len = strlen(buf);
      if (buf[0] != '/') {
        /* It's a package */
        char *dot = buf + len - 6; /* find the .list */
        if (strcmp(dot, ".list\n"))
          ohshit("Could not parse line: %s", buf);
        *dot = '\0';

        /* finalize the previous package */
        if (pkg) {
          ensure_package_clientdata(pkg);
          pkg->clientdata->fileslistvalid = 1;
          if (saidread == 1)
            progress_step(&progress);
        }

        /* grab the new one */
        pkg = findpackage(buf); /* TODO: make sure it exists */
        pos_hint = NULL;
        _erase_pkg_file_data(pkg);
      } else {
        /* It's a file */
        if (!pkg)
          ohshit("Not a package line: %s", buf);

        char *br = buf + len - 1;
        if (*br != '\n')
          ohshit("Missing trailing line break: %s", buf);
        *br = '\0';

        pos_hint = _add_file_to_package(pkg, buf, 0, pos_hint);
      }
    }
    /* finalize the last package */
    if (pkg) {
      ensure_package_clientdata(pkg);
      pkg->clientdata->fileslistvalid= 1;
      if (saidread == 1)
        progress_step(&progress);
    }
    fclose(pipefile);
  }

  /*
  it= iterpkgstart();
  while ((pkg = iterpkgnext(it)) != NULL) {
    ensure_packagefiles_available(pkg);

    if (saidread == 1)
      progress_step(&progress);
  }
  iterpkgend(it);
  */
  allpackagesdone= 1;

  if (saidread==1) {
    progress_done(&progress);
    printf(_("%d files and directories currently installed.)\n"),nfiles);
    saidread=2;
  }
}

void ensure_allinstfiles_available_quiet(void) {
  saidread=2;
  ensure_allinstfiles_available();
}

void write_filelist_except(struct pkginfo *pkg, struct fileinlist *list, int leaveout) {
  /* If leaveout is nonzero, will not write any file whose filenamenode
   * has the fnnf_elide_other_lists flag set.
   */
  static struct varbuf vb, newvb;
  FILE *file;

  varbufreset(&vb);
  varbufaddstr(&vb,admindir);
  varbufaddstr(&vb,"/" INFODIR);
  varbufaddstr(&vb,pkg->name);
  varbufaddstr(&vb,"." LISTFILE);
  varbufaddc(&vb,0);

  varbufreset(&newvb);
  varbufaddstr(&newvb,vb.buf);
  varbufaddstr(&newvb,NEWDBEXT);
  varbufaddc(&newvb,0);
  
  file= fopen(newvb.buf,"w+");
  if (!file)
    ohshite(_("unable to create updated files list file for package %s"),pkg->name);
  push_cleanup(cu_closefile, ehflag_bombout, NULL, 0, 1, (void *)file);
  while (list) {
    if (!(leaveout && (list->namenode->flags & fnnf_elide_other_lists))) {
      fputs(list->namenode->name,file);
      putc('\n',file);
    }
    list= list->next;
  }
  if (ferror(file))
    ohshite(_("failed to write to updated files list file for package %s"),pkg->name);
  if (fflush(file))
    ohshite(_("failed to flush updated files list file for package %s"),pkg->name);
  if (fsync(fileno(file)))
    ohshite(_("failed to sync updated files list file for package %s"),pkg->name);
  pop_cleanup(ehflag_normaltidy); /* file= fopen() */
  if (fclose(file))
    ohshite(_("failed to close updated files list file for package %s"),pkg->name);
  if (rename(newvb.buf,vb.buf))
    ohshite(_("failed to install updated files list file for package %s"),pkg->name);

  note_must_reread_files_inpackage(pkg);
}

void reversefilelist_init(struct reversefilelistiter *iterptr,
                          struct fileinlist *files) {
  /* Initialises an iterator that appears to go through the file
   * list `files' in reverse order, returning the namenode from
   * each.  What actually happens is that we walk the list here,
   * building up a reverse list, and then peel it apart one
   * entry at a time.
   */
  struct fileinlist *newent;
  
  iterptr->todo = NULL;
  while (files) {
    newent= m_malloc(sizeof(struct fileinlist));
    newent->namenode= files->namenode;
    newent->next= iterptr->todo;
    iterptr->todo= newent;
    files= files->next;
  }
}

struct filenamenode *reversefilelist_next(struct reversefilelistiter *iterptr) {
  struct filenamenode *ret;
  struct fileinlist *todo;

  todo= iterptr->todo;
  if (!todo)
    return NULL;
  ret= todo->namenode;
  iterptr->todo= todo->next;
  free(todo);
  return ret;
}

void reversefilelist_abort(struct reversefilelistiter *iterptr) {
  /* Clients must call this function to clean up the reversefilelistiter
   * if they wish to break out of the iteration before it is all done.
   * Calling this function is not necessary if reversefilelist_next has
   * been called until it returned 0.
   */
  while (reversefilelist_next(iterptr));
}

struct fileiterator {
  struct filenamenode *namenode;
  int nbinn;
};

#define BINS (1 << 17)
 /* This must always be a power of two.  If you change it
  * consider changing the per-character hashing factor (currently
  * 1785 = 137*13) too.
  */

static struct filenamenode *bins[BINS];

struct fileiterator *iterfilestart(void) {
  struct fileiterator *i;
  i= m_malloc(sizeof(struct fileiterator));
  i->namenode = NULL;
  i->nbinn= 0;
  return i;
}

struct filenamenode *iterfilenext(struct fileiterator *i) {
  struct filenamenode *r= NULL;

  while (!i->namenode) {
    if (i->nbinn >= BINS)
      return NULL;
    i->namenode= bins[i->nbinn++];
  }
  r= i->namenode;
  i->namenode= r->next;
  return r;
}

void iterfileend(struct fileiterator *i) {
  free(i);
}

void filesdbinit(void) {
  struct filenamenode *fnn;
  int i;

  for (i=0; i<BINS; i++)
    for (fnn= bins[i]; fnn; fnn= fnn->next) {
      fnn->flags= 0;
      fnn->oldhash = NULL;
      fnn->filestat = NULL;
    }
}

static int hash(const char *name) {
  int v= 0;
  while (*name) { v *= 1787; v += *name; name++; }
  return v;
}

struct filenamenode *findnamenode(const char *name, enum fnnflags flags) {
  struct filenamenode **pointerp, *newnode;
  const char *orig_name = name;

  /* We skip initial slashes and ./ pairs, and add our own single leading slash. */
  name = path_skip_slash_dotslash(name);

  pointerp= bins + (hash(name) & (BINS-1));
  while (*pointerp) {
/* Why is this assert nescessary?  It is checking already added entries. */
    assert((*pointerp)->name[0] == '/');
    if (!strcmp((*pointerp)->name+1,name)) break;
    pointerp= &(*pointerp)->next;
  }
  if (*pointerp) return *pointerp;

  if (flags & fnn_nonew)
    return NULL;

  newnode= nfmalloc(sizeof(struct filenamenode));
  newnode->packages = NULL;
  if((flags & fnn_nocopy) && name > orig_name && name[-1] == '/')
    newnode->name = name - 1;
  else {
    char *newname= nfmalloc(strlen(name)+2);
    newname[0]= '/'; strcpy(newname+1,name);
    newnode->name= newname;
  }
  newnode->flags= 0;
  newnode->next = NULL;
  newnode->divert = NULL;
  newnode->statoverride = NULL;
  newnode->filestat = NULL;
  newnode->trig_interested = NULL;
  *pointerp= newnode;
  nfiles++;

  return newnode;
}

/* vi: ts=8 sw=2
 */
