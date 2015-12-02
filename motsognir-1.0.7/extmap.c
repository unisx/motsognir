/*
 * This file is part of the Motsognir gopher server
 * Copyright (C) Mateusz Viste 2014
 *
 * Provides a simple db-like system for storing and mapping file extensions
 */

#include <stdio.h>   /* FILE */
#include <stdlib.h>  /* malloc(), calloc(), free() */
#include <string.h>  /* strcmp() */

#include "extmap.h"  /* include self for control */

struct extmap_node {
  struct extmap_node *next;
  char type;
  char extension[1]; /* this MUST be at the end - struct is made bigger to accomodate longer extensions */
};

struct extmap_t {
  struct extmap_node *maplist[1024];  /* mapping on two first letters fits in 10 bits */
  char fallback;
};

/* copy src string to dst, forcing lower case at the same time */
static void copystringlcase(char *dst, char *src) {
  int x;
  for (x = 0; src[x] != 0; x++) {
    if ((src[x] >= 'A') && (src[x] <= 'Z')) {
        dst[x] = ('a' - 'A') + src[x];
      } else {
        dst[x] = src[x];
    }
  }
  dst[x] = 0;
}

/* right trim string and return new length */
static int rtrimgetlen(char *s) {
  int lastrealchar = -1, x;
  for (x = 0; s[x] != 0; x++) {
    if ((s[x] != ' ') && (s[x] != '\t')) lastrealchar = x;
  }
  lastrealchar++;
  s[lastrealchar] = 0;
  return(lastrealchar);
}

/* read line from file pointed by fd, read into linebuff up to maxlen
 * characters, and trim the rest. Returns non zero if EOF found */
static int getlinefromfile(FILE *fd, char *linebuff, int maxlen) {
  int eofflag = 0, linelen = 0, bytebuf;
  for (;;) {
    bytebuf = fgetc(fd);
    if (bytebuf == '\n') break;
    if (bytebuf == EOF) {
      eofflag = 1;
      break;
    }
    if (bytebuf == '\r') continue;
    if (linelen < maxlen) linebuff[linelen++] = bytebuf;
  }
  linebuff[linelen] = 0;
  return(eofflag);
}

/* provides a 10-bit mapping id of the extension / the extension MUST be fed
 * in lower case format! */
static int ext2map(char *extension) {
  int res;
  /* map only if two first chars are both in range a-z */
  if ((extension[0] >= 'a') && (extension[0] <= 'z') && (extension[1] >= 'a') && (extension[1] <= 'z')) {
      res = extension[0] - 'a';
      res <<= 5;
      res |= extension[1] - 'a';
      res += 1; /* plus 1, because id 0 is reserved for unmappable filetypes */
    } else { /* this is an unmappable extension -> goes to id 0 */
      res = 0;
  }
  return(res);
}

/* adds an extension to an extmap db */
static void additem(struct extmap_t *obj, char *ext, char type) {
  int hashmap;
  struct extmap_node *newnode;
  newnode = calloc(1, sizeof(struct extmap_node) + strlen(ext));
  if (newnode == NULL) return;
  /* populate the new node with data */
  copystringlcase(newnode->extension, ext);
  newnode->type = type;
  /* compute extension's hash map value looking at its lower case version */
  hashmap = ext2map(newnode->extension);
  /* attach the new node to the proper list */
  newnode->next = obj->maplist[hashmap];
  obj->maplist[hashmap] = newnode;
}

/* loads a extension->filetype mapping file, or loads a default map if file is NULL */
struct extmap_t *extmap_load(char *file) {
  int slen;
  FILE *fd;
  int eofflag;
  char linebuff[64];
  struct extmap_t *res;
  res = calloc(1, sizeof(struct extmap_t));
  if (res == NULL) return(NULL);
  /* set default fallback type */
  res->fallback = '9';
  /* load a default map if file is NULL */
  if (file == NULL) {
    additem(res, "aac", 's');
    additem(res, "aiff", 's');
    additem(res, "bas", '0');
    additem(res, "bmp", 'I');
    additem(res, "c", '0');
    additem(res, "css", '0');
    additem(res, "eps", 'I');
    additem(res, "flac", 's');
    additem(res, "gif", 'g');
    additem(res, "htm", 'h');
    additem(res, "html", 'h');
    additem(res, "ico", 'I');
    additem(res, "jpeg", 'I');
    additem(res, "jpg", 'I');
    additem(res, "mp2", 's');
    additem(res, "mp3", 's');
    additem(res, "mpc", 's');
    additem(res, "mid", 's');
    additem(res, "pcx", 'I');
    additem(res, "pdf", 'P');
    additem(res, "png", 'I');
    additem(res, "tif", 'I');
    additem(res, "tiff", 'I');
    additem(res, "txt", '0');
    additem(res, "svg", 'I');
    additem(res, "wav", 's');
    additem(res, "wma", 's');
    return(res);
  }
  /* a real file has been provided -> load it */
  fd = fopen(file, "rb");
  if (fd == NULL) {
    free(res);
    return(NULL);
  }
  /* load types (read line by line, and parse) */
  for (;;) {
    eofflag = getlinefromfile(fd, linebuff, 63);
    /* compute string length, and right trim */
    slen = rtrimgetlen(linebuff);
    /* skip lines shorter than 2 chars */
    if (slen < 2) {
      if (eofflag != 0) break;
      continue;
    }
    /* skip comments and lines starting with a space */
    if ((linebuff[0] == '#') || (linebuff[0] == ' ')) continue;
    /* check that the line is ending with a colon ':' followed by filetype */
    if (linebuff[slen - 2] != ':') continue;
    /* separate string and add new type */
    linebuff[slen - 2] = 0;
    additem(res, linebuff, linebuff[slen - 1]);
  }
  fclose(fd);
  return(res);
}

/* frees the memory allocated to an extmap */
void extmap_free(struct extmap_t *obj) {
  int x;
  struct extmap_node *lnode, *previousnode;
  /* iterate through all mappings, and free linked lists */
  for (x = 0; x < 676; x++) {
    lnode = obj->maplist[x];
    while (lnode != NULL) {
      previousnode = lnode;
      lnode = lnode->next;
      free(previousnode);
    }
  }
  /* finally, free the container itself */
  free(obj);
}

/* performs a lookup of an extension in our mapping system, and return the gopher type char */
char extmap_lookup(struct extmap_t *obj, char *extension) {
  char lowext[16];
  struct extmap_node *rootnode;

  /* if the extension is longer than 15 chars, do not map */
  if (strlen(extension) > 15) return(obj->fallback);

  /* convert the extension to lower case */
  copystringlcase(lowext, extension);

  /* select the proper linked list, by mapping the extension */
  rootnode = obj->maplist[ext2map(lowext)];

  /* iterate through the list to match an extension */
  while (rootnode != NULL) {
    if (strcmp(rootnode->extension, lowext) == 0) return(rootnode->type);
    rootnode = rootnode->next;
  }

  /* if nothing matched, return the default type */
  return(obj->fallback);
}
