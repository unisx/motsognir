/*
 * Test application for extmap.
 *
 * This file is part of the Motsognir gopher server.
 * Copyright (C) Mateusz Viste 2014
 */


#include "extmap.h"
#include <stdio.h>


int main(int argc, char **argv) {
  struct extmap_t *extmap;
  int extnum;

  if (argc < 3) {
    puts("extmaptest is a simple tool to test motsognir's extmap engine.");
    puts("usage: extmaptest file.conf ext1 [ext2] ... [extN]");
    return(1);
  }

  if (argv[1][0] == 0) {
      puts("load the default extmap...");
      extmap = extmap_load(NULL);
    } else {
      printf("load an extmap from %s...\n", argv[1]);
      extmap = extmap_load(argv[1]);
  }
  if (extmap == NULL) {
    puts("extmap_load() failed");
    return(1);
  }

  puts("perform lookups on the extmap...");
  for (extnum = 2; extnum < argc; extnum++) {
    printf("  %s -> %c\n", argv[extnum], extmap_lookup(extmap, argv[extnum]));
  }

  puts("free() the extmap...");
  extmap_free(extmap);
  return(0);
}
