/*
    * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
    *  Motsognir - The mighty gopher server                               *
    *  Copyright (C) Mateusz Viste 2008-2015                              *
    * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *

   ----------------------------------------------------------------------
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
   ----------------------------------------------------------------------
*/

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>  /* required by FreeBSD to define PATH_MAX */
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>  /* required by FreeBSD to define in6addr_any */
#include <netinet/tcp.h>
#include <sys/uio.h>     /* writev() */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>  /* WEXITSTATUS */

#include "binary.h"
#include "extmap.h"

/* Constants */
#define pVer "1.0.7"
#define pDate "2008-2015"
#define HOMEPAGE "http://motsognir.sourceforge.net"

/* declare the default config file location, if not already declared from CLI
 * at compile-time - esp. useful for systems that store config files in other
 * locations, like /usr/local/etc/ for FreeBSD... */
#ifndef CONFIGFILE
  #define CONFIGFILE "/etc/motsognir.conf"
#endif


struct MotsognirConfig {
  char *gopherroot;
  char *userdir;
  int gopherport;
  char *gopherhostname;
  char *defaultgophermap;
  int verbosemode;
  int capssupport;
  char *capsservergeolocationstring;
  char *capsserverarchitecture;
  char *capsserverdescription;
  char *capsserverdefaultencoding;
  int cgisupport;
  int phpsupport;
  int subgophermaps;
  int paranoidmode;
  char *runasuser;
  uid_t runasuser_uid;
  gid_t runasuser_gid;
  char *runasuser_home;
  char *chroot;
  char *httperrfile;
  char *bind;
  char *extmapfile;
  struct extmap_t *extmap;
  char securldelim;
};


/* Unset any extraneous environment variables which CGI/PHP is unlikely to need. */
static void sanitizeenv(void) {
  unsetenv("COLUMNS");
  unsetenv("DISPLAY");
  unsetenv("INPUTRC");
  unsetenv("LINES");
  unsetenv("SHLVL");
  unsetenv("TERM");
}


/* Reads a file from disk, loads it into an array, and returns a pointer to it */
static char *readfiletomem(char *file) {
  char *source = NULL;
  FILE *fp = fopen(file, "rb");
  if (fp != NULL) {
    /* go to the end of the file */
    if (fseek(fp, 0L, SEEK_END) == 0) {
      /* get the size of the file */
      long bufsize = ftell(fp);
      if (bufsize == -1) { /* Error */ }
      /* allocate our buffer to that size */
      source = malloc(sizeof(char) * (bufsize + 1));
      if (source != NULL) {
        size_t newLen;
        /* go back to the start of the file */
        fseek(fp, 0L, SEEK_SET);
        /* read the entire file into memory */
        newLen = fread(source, sizeof(char), bufsize, fp);
        source[++newLen] = '\0'; /* string terminator, just to be safe */
      }
    }
    fclose(fp);
  }
  return(source);
}


/* Drop root privileges - returns 0 on success, non-zero on failure */
static int droproot(struct MotsognirConfig *config) {
  /* drop privileges */
  if (initgroups(config->runasuser, config->runasuser_gid) != 0 || setgid(config->runasuser_gid) != 0 || setuid(config->runasuser_uid) != 0) {
    syslog(LOG_WARNING, "ERROR: Couldn't change to '%.32s' uid=%lu gid=%lu: %s", config->runasuser, (unsigned long)config->runasuser_uid, (unsigned long)config->runasuser_gid, strerror(errno));
    return(-1);
  }
  /* it's all good, but let's double check (you never know) */
  if (getuid() != config->runasuser_uid) {
    syslog(LOG_WARNING, "ERROR: For some mysterious reasons Motsognir was unable to switch to user '%s'.", config->runasuser);
    return(-1);
  }
  /* Clean up the remnants of running as root */
  setenv("USER", config->runasuser, 1);
  setenv("USERNAME", config->runasuser, 1);
  unsetenv("SUDO_USER");
  unsetenv("SUDO_UID");
  unsetenv("SUDO_GID");
  unsetenv("SUDO_COMMAND");
  if (config->runasuser_home != NULL) setenv("HOME", config->runasuser_home, 1);
  return(0);
}


static void sendline(int sock, char *dataline) {
  /* I am using writev() here to make sure that the line and the \r\n trailer will be sent at the same time (in one packet) */
  struct iovec iov[2];
  iov[0].iov_base = dataline;
  iov[0].iov_len = strlen(dataline);
  iov[1].iov_base = "\r\n";
  iov[1].iov_len = 2;
  writev(sock, iov, 2);
}


static char *skip_whitespace(char *s) {
  while (*s == ' ' || (unsigned char)(*s - 9) <= (13 - 9)) s++;
  return (char *) s;
}


/* Trims any whitespaces before and after a string. */
static void trimstr(char *s) {
  size_t len = strlen(s);
  /* trim trailing whitespace */
  while (len && isspace(s[len-1])) len--;
  /* trim leading whitespace */
  if (len != 0) {
    char *nws = skip_whitespace(s);
    if ((nws - s) != 0) {
      len -= (nws - s);
      memmove(s, nws, len);
    }
  }
  s[len] = '\0';
}


/* Encode a string to percent encoding */
static void percencode(char *src, char *dst, int dstmaxlen) {
  int x, encodingrequired, dstlen = 0;
  const char *hexchar = "0123456789ABCDEF";
  for (x = 0; src[x] != 0; x++) {
    encodingrequired = 0;
    for (;;) { /* not a 'real' loop, just an easy way to break out */
      if ((src[x] >= 'a') && (src[x] <= 'z')) break; /* do not encode a..z */
      if ((src[x] >= 'A') && (src[x] <= 'Z')) break; /* do not encode A..Z */
      if ((src[x] >= '0') && (src[x] <= '9')) break; /* do not encode 0..9 */
      if ((src[x] == '-') || (src[x] == '/') || (src[x] == '_') || (src[x] == '.') || (src[x] == '~')) break; /* do not encode some chars */
      /* since I am here, I need to encode the stuff I got */
      encodingrequired = 1;
      break;
    }
    if (dstlen + 4 >= dstmaxlen) {
      syslog(LOG_WARNING, "WARNING: reached percent encoding length limit - aborting");
      break; /* stop the work if we reached our limit */
    }
    if (encodingrequired == 0) { /* if no encoding is needed, just put the char as-is */
        dst[dstlen++] = src[x];
      } else { /* otherwise I need to encode the char */
        dst[dstlen++] = '%';
        dst[dstlen++] = hexchar[(unsigned)(src[x] & 0xF0) >> 4];
        dst[dstlen++] = hexchar[(unsigned)(src[x] & 0x0F)];
    }
  }
  /* terminate the dst string with a NULL terminator */
  dst[dstlen] = 0;
}


/* Decodes a single hex character (0..F) and returns its value. Returns -1 if
 * the character is invalid. */
static int hex2int(char ch) {
  switch (ch) {
    case '0':
      return(0);
    case '1':
      return(1);
    case '2':
      return(2);
    case '3':
      return(3);
    case '4':
      return(4);
    case '5':
      return(5);
    case '6':
      return(6);
    case '7':
      return(7);
    case '8':
      return(8);
    case '9':
      return(9);
    case 'A':
    case 'a':
      return(10);
    case 'B':
    case 'b':
      return(11);
    case 'C':
    case 'c':
      return(12);
    case 'D':
    case 'd':
      return(13);
    case 'E':
    case 'e':
      return(14);
    case 'F':
    case 'f':
      return(15);
    default:
      return(-1);
  }
}


/* decodes a string from percent encoding to a 'normal' string, in-place. returns 0 on success, non-zero if decoding fails. */
static int percdecode(char *string) {
  int x, y, firstnibble, secondnibble;
  y = 0;
  for (x = 0; string[x] != 0; x++) {
    /* if different than %, write it as-is (or as a space, if we got a +), and continue */
    if (string[x] != '%') {
      if (string[x] == '+') {
          string[y++] = ' ';
        } else {
          string[y++] = string[x];
      }
      continue;
    }
    /* since we are here, we are dealing with a percent-encoded thing - first make sure we are not in a dangerous position */
    if ((string[x + 1] == 0) || (string[x + 2] == 0)) {
      string[x] = 0;
      syslog(LOG_WARNING, "ERROR: detected invalid percent encoding");
      return(-1);
    }
    /* detect NULL chars, these shall never be decoded */
    if ((string[x + 1] == '0') && (string[x + 2] == '0')) {
      string[x] = 0;
      syslog(LOG_WARNING, "ERROR: detected a dangerous percent encoding (%%00)");
      return(-1);
    }
    /* decode anything else */
    firstnibble = hex2int(string[++x]);
    secondnibble = hex2int(string[++x]);
    if ((firstnibble < 0) || (secondnibble < 0)) {
      string[x - 2] = 0;
      syslog(LOG_WARNING, "ERROR: detected an invalid percent encoding");
      return(-1);
    }
    string[y++] = (firstnibble << 4) | secondnibble;
  }
  /* terminate the result */
  string[y] = 0;
  return(0);
}


static void printcapstxt(int sock, struct MotsognirConfig *config, char *version) {
  char linebuff[1024];
  sendline(sock, "CAPS");                 /* These four characters must be at the beginning to identify the file as successfully fetched. */
  sendline(sock, "CapsVersion=1");        /* Spec version of this caps file. This should be the first key specified. */
  sendline(sock, "ExpireCapsAfter=3600"); /* This tells the client the recommended caps cache expiry time, in seconds. */
  sendline(sock, "PathDelimiter=/");      /* This tells the client how to cut up a selector into a breadcrumb menu. */
  sendline(sock, "PathIdentity=.");       /* Tells the client what the "identity" path is, i.e., it can treat this as a no-op, turning x/./y into x/y. */
  sendline(sock, "PathParent=..");        /* Tells the client what the parent path is, i.e., it can treat this as an instruction to delete previous path, turning x/y/../z into x/z */
  sendline(sock, "PathParentDouble=FALSE"); /* Tells the client that consecutive path delimeters are treated as parent */
  /* PathEscapeCharacter=\ */ /* Tells the client the escape character for quoting the above metacharacters. */
                              /* Most of the time this is \. If this is not specified, no escape characters are used. */
  sendline(sock, "PathKeepPreDelimeter=FALSE"); /* Tells the client not to cut everything up to the first path */
                                                /* delimeter. Normally caps makes gopher://x/11/xyz into /xyz as */
                                                /* well as gopher://x/1/xyz, assuming your server is happy with */
                                                /* the latter URL (almost all will be). If this is not specified, */
                                                /* it is by default FALSE. This should be TRUE *only* if your server */
                                                /* requires URLs like gopher://x/0xyz. */
  sendline(sock, "ServerSoftware=Motsognir");   /* Server's name */

  sprintf(linebuff, "ServerSoftwareVersion=%s", version);
  sendline(sock, linebuff);  /* Server's version */

  if (config->capsserverarchitecture != NULL) {
    sprintf(linebuff, "ServerArchitecture=%s", config->capsserverarchitecture);
    sendline(sock, linebuff);
  }
  if (config->capsserverdescription != NULL) {
    sprintf(linebuff, "ServerDescription=%s", config->capsserverdescription);
    sendline(sock, linebuff);
  }
  if (config->capsservergeolocationstring != NULL) {
    sprintf(linebuff, "ServerGeolocationString=%s", config->capsservergeolocationstring);
    sendline(sock, linebuff);
  }
  if (config->capsserverdefaultencoding != NULL) {
    sprintf(linebuff, "ServerDefaultEncoding=%s", config->capsserverdefaultencoding);
    sendline(sock, linebuff);
  }
}


static void about(char *version, char *datestring, char *homepage) {
  printf("Motsognir v%s Copyright (C) Mateusz Viste %s\n\n", version, datestring);
  printf("This program is free software: you can redistribute it and/or modify it under\n"
         "the terms of the GNU General Public License as published by the Free Software\n"
         "Foundation, either version 3 of the License, or (at your option) any later\n"
         "version.\n");
  printf("This program is distributed in the hope that it will be useful, but WITHOUT ANY\n"
         "WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A\n"
         "PARTICULAR PURPOSE. See the GNU General Public License for more details.\n\n");
  printf("Motsognir is a robust and reliable open-source gopher server for POSIX systems.\n"
         "Motsognir is entirely written in ANSI C, without any external dependencies.\n\n");
  printf("Available command-line parameters:\n"
         "  --config file.conf       use a configuration file in a custom location\n"
         "\n");
  printf("homepage: %s\n\n", homepage);
}


static void sendbackhttperror(int sock, struct MotsognirConfig *config) {
  char txtline[1024], portstr[16];
  syslog(LOG_INFO, "HTTP request detected - a HTTP error message is returned.");
  sendline(sock, "HTTP/1.1 400 Bad request");
  sendline(sock, "Content-Type: text/html; charset=UTF-8");
  sendline(sock, "Server: Motsognir");
  sendline(sock, "Connection: close");
  sendline(sock, "");
  if (config->httperrfile != NULL) {
      sendline(sock, config->httperrfile);
    } else {
      sendline(sock, "<!DOCTYPE html>");
      sendline(sock, "<html>");
      sendline(sock, "  <head>");
      sendline(sock, "    <title>Error 400 - Bad request</title>");
      sendline(sock, "    <style>");
      sendline(sock, "      body { font-family: sans-serif; font-size: 1.1em; margin: 1em; }");
      sendline(sock, "      h1 { color: red; text-align: center; }");
      sendline(sock, "    </style>");
      sendline(sock, "  </head>");
      sendline(sock, "  <body>");
      sendline(sock, "    <h1>Error 400 - BAD REQUEST</h1>");
      sendline(sock, "    <p>Your request is not admissible. Sorry. This is a gopher server, which means that you have to use the gopher protocol to access it. Right now, you used the HTTP protocol instead.</p>");
      sendline(sock, "    <p style='text-align: center'>");
      if (config->gopherport == 70) {
          portstr[0] = 0;
        } else {
          sprintf(portstr, ":%d", config->gopherport);
      }
      sprintf(txtline, "      <a href='gopher://%s%s/' style='font-size: 1.15em;'>Click here to access this server using the gopher protocol.</a>", config->gopherhostname, portstr);
      sendline(sock, txtline);
      sendline(sock, "    </p>");
      sendline(sock, "  </body>");
      sendline(sock, "</html>");
  }
}


/* checks if a file exists. returns zero if the file does not exit, non-zero otherwise. */
static int fexist(char *filename) {
  FILE *fd;
  fd = fopen(filename, "rb");
  if (fd != NULL) { /* file exists */
      fclose(fd);
      return(1);
    } else {   /* file doesn't exist */
      return(0);
  }
}


static int loadconfig(struct MotsognirConfig *config, char *configfile) {
  FILE *fd;
  char tokenbuff[1024], valuebuff[1024];
  int tokenbuffpos = 0;
  int valuebuffpos = 0;
  int bytebuff;
  int state = 0; /* 0=reading token, 1=reading value, 2=reading comment */
  struct passwd *pw;

  /* zero out the config structure, just in case */
  memset(config, 0, sizeof(*config));

  /* load default values first */
  config->gopherroot = "/var/gopher/";
  config->userdir = NULL;
  config->gopherport = 70;
  config->gopherhostname = NULL;
  config->verbosemode = 0;
  config->capssupport = 0;
  config->capsservergeolocationstring = NULL;
  config->capsserverarchitecture = NULL;
  config->capsserverdescription = NULL;
  config->capsserverdefaultencoding = NULL;
  config->defaultgophermap = NULL;
  config->cgisupport = 0;
  config->phpsupport = 0;
  config->subgophermaps = 0;
  config->paranoidmode = 0;
  config->runasuser = NULL;
  config->runasuser_uid = 0;
  config->runasuser_gid = 0;
  config->runasuser_home = NULL;
  config->chroot = NULL;
  config->httperrfile = NULL;
  config->bind = NULL;
  config->extmapfile = NULL;
  config->extmap = NULL;
  config->securldelim = 0;

  fd = fopen(configfile, "r");
  if (fd == NULL) {
    syslog(LOG_WARNING, "WARNING: Failed to open the configuration file at '%s'", configfile);
    return(-1);
  }

  for (;;) {
    bytebuff = getc(fd);
    if (state == 0) {  /* if reading token... */
        if (bytebuff < 0) break;
        if (bytebuff == '=') {
            tokenbuff[tokenbuffpos] = 0;
            trimstr(tokenbuff);
            tokenbuffpos = 0;
            state = 1;
          } else if (bytebuff == '\n') {
            tokenbuff[tokenbuffpos] = 0;
            trimstr(tokenbuff);
            tokenbuffpos = 0;
            state = 0;
          } else {
            if (tokenbuffpos < 1023) tokenbuff[tokenbuffpos++] = bytebuff;
        }
      } else if (state == 1) { /* if reading value... */
        if ((bytebuff == '\n') || (bytebuff == '\n') || (bytebuff < 0) || (bytebuff == '#')) {
            state = 0;
            if (bytebuff == '#') state = 2;
            /* check token and assign value */
            valuebuff[valuebuffpos] = 0;
            trimstr(valuebuff);
            if (valuebuff[0] == 0) tokenbuff[0] = 0; /* make sure to ignore any parameter with an empty value */
            /* printf("Got conf: '%s' -> '%s'\n", tokenbuff, valuebuff); */
            if (strcasecmp(tokenbuff, "verbose") == 0) {
                config->verbosemode = atoi(valuebuff);
              } else if (strcasecmp(tokenbuff, "bind") == 0) {
                config->bind = strdup(valuebuff);
              } else if (strcasecmp(tokenbuff, "capssupport") == 0) {
                config->capssupport = atoi(valuebuff);
              } else if (strcasecmp(tokenbuff, "CapsServerArchitecture") == 0) {
                config->capsserverarchitecture = strdup(valuebuff);
              } else if (strcasecmp(tokenbuff, "CapsServerDescription") == 0) {
                config->capsserverdescription = strdup(valuebuff);
              } else if (strcasecmp(tokenbuff, "CapsServerGeolocationString") == 0) {
                config->capsservergeolocationstring = strdup(valuebuff);
              } else if (strcasecmp(tokenbuff, "CapsServerDefaultEncoding") == 0) {
                config->capsserverdefaultencoding = strdup(valuebuff);
              } else if (strcasecmp(tokenbuff, "DefaultGophermap") == 0) {
                config->defaultgophermap = strdup(valuebuff);
              } else if (strcasecmp(tokenbuff, "GopherRoot") == 0) {
                config->gopherroot = strdup(valuebuff);
              } else if (strcasecmp(tokenbuff, "RunAsUser") == 0) {
                config->runasuser = strdup(valuebuff);
              } else if (strcasecmp(tokenbuff, "GopherPort") == 0) {
                config->gopherport = atoi(valuebuff);
              } else if (strcasecmp(tokenbuff, "GopherHostname") == 0) {
                config->gopherhostname = strdup(valuebuff);
              } else if (strcasecmp(tokenbuff, "GopherCgiSupport") == 0) {
                config->cgisupport = atoi(valuebuff);
              } else if (strcasecmp(tokenbuff, "GopherPhpSupport") == 0) {
                config->phpsupport = atoi(valuebuff);
              } else if (strcasecmp(tokenbuff, "SubGophermaps") == 0) {
                config->subgophermaps = atoi(valuebuff);
              } else if (strcasecmp(tokenbuff, "paranoidmode") == 0) {
                config->paranoidmode = atoi(valuebuff);
              } else if (strcasecmp(tokenbuff, "chroot") == 0) {
                config->chroot = strdup(valuebuff);
              } else if (strcasecmp(tokenbuff, "userdir") == 0) {
                config->userdir = strdup(valuebuff);
              } else if (strcasecmp(tokenbuff, "httperrfile") == 0) {
                config->httperrfile = readfiletomem(valuebuff);
                if (config->httperrfile == NULL) syslog(LOG_WARNING, "WARNING: Failed to load custom http error file '%s'. Default content will be used instead.", valuebuff);
              } else if (strcasecmp(tokenbuff, "ExtMapFile") == 0) {
                config->extmapfile = strdup(valuebuff);
              } else if (strcasecmp(tokenbuff, "SecUrlDelim") == 0) {
                config->securldelim = atoi(valuebuff);
            }
            valuebuffpos = 0;
          } else if (bytebuff != '\r') {
            if (valuebuffpos < 1023) valuebuff[valuebuffpos++] = bytebuff;
        }
        if (bytebuff < 0) break;
      } else { /* if reading comment... */
        if (bytebuff < 0) break;
        if (bytebuff == '\n') state = 0;
    }

  }
  fclose(fd);

  /* Perform some validation of the configuration content... */

  if (config->verbosemode < 0) {
    syslog(LOG_ERR, "ERROR: Invalid verbose level found in the configuration file (%d)", config->verbosemode);
    return(-1);
  }

  if (config->gopherport < 1) {
    syslog(LOG_ERR, "ERROR: Invalid gopher port found in the configuration file (%d)", config->gopherport);
    return(-1);
  }

  if (config->gopherroot[0] == 0) {
    syslog(LOG_ERR, "ERROR: Missing gopher root path in the configuration file. Please add a valid 'GopherRoot=' directive");
    return(-1);
  }

  /* if userdir is definied, it shall be an absolute path that includes a %s */
  if (config->userdir != NULL) {
    if ((config->userdir[0] != '/') || (strstr(config->userdir, "%s") == NULL)) {
      syslog(LOG_ERR, "ERROR: The UserDir configuration is invalid. It shall be an absolute path (start by '/') and contain the '%%s' placeholder.");
      return(-1);
    }
  }

  if (config->gopherhostname == NULL) {
    syslog(LOG_WARNING, "WARNING: Missing gopher hostname in the configuration file. The local IP address will be used instead. Please add a valid 'GopherHostname=' directive.");
  }

  /* load extension mappings (ext -> gopher type pairs) */
  config->extmap = extmap_load(config->extmapfile);
  if (config->extmap == NULL) {
    syslog(LOG_ERR, "ERROR: failed to load the extension mapping file '%s'", config->extmapfile);
    return(-1);
  }

  /* if a 'RunAsUser' directive is present, resolve the username to a proper uid/gid (because later we might be unable to do it from within a chroot jail) */
  if (config->runasuser != NULL) {
    pw = getpwnam(config->runasuser);
    if (pw == NULL) {
      syslog(LOG_ERR, "ERROR: Could not map the username '%s' to a valid uid", config->runasuser);
      return(-1);
    }
    free(config->runasuser);              /* free the original username... */
    config->runasuser = strdup(pw->pw_name); /* ...and replace it with the resolved username */
    config->runasuser_uid = pw->pw_uid;
    config->runasuser_gid = pw->pw_gid;
    config->runasuser_home = strdup(pw->pw_dir);
  }

  return(0);
}


static char **explode_serverside_params_from_query(char *directorytolist, struct MotsognirConfig *config) {
  char *ptr, *tabposition = NULL, *queposition = NULL;
  static char *res[2] = { NULL, NULL };
  /* find out the positions of tabs and question marks */
  for (ptr = directorytolist; *ptr != 0; ptr++) {
    if ((*ptr == '?') && (queposition == NULL)) {
      queposition = ptr;
    }
    if ((*ptr == config->securldelim) && (queposition == NULL)) {
      queposition = ptr;
    }
    if ((*ptr == '\t') && (tabposition == NULL)) {
      tabposition = ptr;
      break;
    }
  }
  /* if a tab was found, extract the search query */
  if (tabposition != NULL) {
    for (ptr = tabposition + 1; *ptr != 0; ptr++) {
      if (*ptr == '\t') {
        *ptr = 0;
        break;
      }
    }
    res[1] = strdup(tabposition + 1);
    *tabposition = 0; /* set tabposition to zero, to end up the URL nicely */
  }
  /* if a question mark was found, extract the URL query */
  if (queposition != NULL) {
    res[0] = strdup(queposition + 1);
    *queposition = 0; /* set queposition to zero, to end up the URL nicely */
  }
  /* Retrieve server-side parameters */
  syslog(LOG_INFO, "Got following server-side parameters: %s | %s", res[0], res[1]);
  return(res); /* return the array with params */
}


/* Returns a pointer to the extension part of a filename (or to an empty
 * string if the filename has no extension) */
static char *getfileextension(char *filename) {
  char *ext = NULL;
  int x;
  /* find the LAST occurence of the '.' char in the name */
  for (x = 0; filename[x] != 0; x++) {
    if (filename[x] == '.') ext = &filename[x + 1];
  }
  if (ext != NULL) return(ext);
  /* if no dot present in the filename, return an empty string using the filename's NULL terminator */
  return(filename + x);
}


/* Map the gopher type of a file, based on its extension */
static char DetectGopherType(char *filename, struct extmap_t *extmap) {
  char *ext = getfileextension(filename);
  return(extmap_lookup(extmap, ext));
}


/* Reads a single line from a file descriptor. Returns the length of the line
 * (can be zero). Returns -1 on error or EOF). */
static int sockreadline(int sock, char *buf, int n, time_t *timeoutStartTime) {
  int numRead, totRead = 0, gotatleastonebyte = 0;
  char ch;
  struct timeval tv;
  /* set the socket to return after 1s, so we can check for real timeout every second */
  if (timeoutStartTime != NULL) {
    tv.tv_sec = 1;  /* 1s timeout */
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,(struct timeval *)&tv,sizeof(struct timeval));
  }
  /* start the loop */
  for (;;) {
    numRead = read(sock, &ch, 1);
    /* check for timeout first (we accept requests that are sent in max 10s) */
    if ((timeoutStartTime != NULL) && (time(NULL) - *timeoutStartTime >= 10)) {
      syslog(LOG_INFO, "Request takes too long to come. Connection aborted.");
      return(-1);
    }
    /* if timeout not reached yet, let's see what read() said */
    if (numRead == -1) {
        if ((errno == EINTR) || (errno == EAGAIN) || (errno == EWOULDBLOCK)) {   /* Interrupted --> restart read() */
            continue;
          } else {
            return(-1);             /* Some other error */
        }
      } else if (numRead == 0) {    /* EOF */
        if (gotatleastonebyte == 0) totRead = -1;
        break;
      } else {                      /* 'numRead' must be 1 if we get here */
        gotatleastonebyte = 1;
        if (ch == '\r') continue;   /* skip CR characters (it's probably followed by an LF) */
        if (ch == '\n') break;
        if (totRead < n - 1) {      /* Discard > (n - 1) bytes */
          totRead++;
          *buf++ = ch;
        }
    }
  }
  /* restore the RX timeout on the socket to be within default value */
  if (timeoutStartTime != NULL) {
    tv.tv_sec = 0;  /* no timeout */
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,(struct timeval *)&tv,sizeof(struct timeval));
  }
  /* terminate the buffer and return the result */
  *buf = 0;
  return(totRead);
}


static void exturlredirector(int sock, char *directorytolist) {
  char *rawurl = directorytolist + 4;
  char linebuff[1024];
  syslog(LOG_INFO, "The request is asking for a URL redirection - returned a html document redirecting to '%s'", rawurl);
  sendline(sock, "<!DOCTYPE html>");
  sendline(sock, "<html>");
  sendline(sock, "  <head>");
  sendline(sock, "    <title>Non-gopher link detected</title>");
  sprintf(linebuff, "    <meta http-equiv=\"refresh\" content=\"10;url=%s\">", rawurl);
  sendline(sock, linebuff);
  sendline(sock, "  </head>");
  sendline(sock, "  <body style=\"margin: 1em 2em 1em 2em; background-color: #D0E0FF; color: #101010;\">");
  sendline(sock, "    <table style=\"margin-left: auto; margin-right: auto; width: 70%%; border: 1px solid black; padding: 1.5em 1.1em 1.5em 1.1em; background-color: #E0F0FF;\">");
  sendline(sock, "      <tr>");
  sendline(sock, "        <td>");
  sendline(sock, "          <p style=\"text-align: center; font-size: 1.3em; margin: 0 0 2em 0;\">A non-gopher link has been detected.</p>");
  sendline(sock, "          <p style=\"text-align: justify; margin: 0 0 0 0;\">It appears that you clicked on a non-gopher link, which will make you use another protocol from now on (typically HTTP). Your gopher journey ends here.</p>");
  sendline(sock, "          <p style=\"text-align: center; margin: 0.8em 0 0 0;\">Click on the link below to continue (or wait 10 seconds):</p>");
  sprintf(linebuff, "          <p style=\"text-align: center; font-size: 1.1em; margin: 0.8em 0 0 0;\"><a href=\"%s\" style=\"color: #0000F0;\">%s</a></p>", rawurl, rawurl);
  sendline(sock, linebuff);
  sendline(sock, "        </td>");
  sendline(sock, "      </tr>");
  sendline(sock, "    </table>");
  sendline(sock, "  </body>");
  sendline(sock, "</html>");
}


static void RemoveDoubleChar(char *string, char ch) {
  char chstr[3] = {0, 0, 0};
  char *occur;
  chstr[0] = ch;
  chstr[1] = ch;
  for (;;) {
    occur = strstr(string, chstr);
    if (occur == NULL) break; /* we're done */
    while (*occur != 0) {
      *occur = occur[1];
      occur += 1;
    }
  }
}


/* sorting backend used by outputdircontent to sort the content of a directory */
static int motsognir_dirsort(const struct dirent **a, const struct dirent **b) {
  /* directories should always be listed first */
  if (((*a)->d_type == DT_DIR) && ((*b)->d_type != DT_DIR)) return(-1);
  if (((*a)->d_type != DT_DIR) && ((*b)->d_type == DT_DIR)) return(1);
  /* no directory tie break found, sort them by name then */
  return(strcasecmp((*a)->d_name, (*b)->d_name));
}


static void outputdircontent(int sock, struct MotsognirConfig *config, char *localfile, char *directorytolist) {
  char tempstring[1024];
  DIR *dirptr;
  int direntriescount;
  int x;
  int entriesdisplayed;
  struct dirent **direntries;

  /* load the content of the directory */
  dirptr = opendir(localfile);
  if (dirptr == NULL) {
    syslog(LOG_WARNING, "ERROR: Could not access directory '%s' (%s)", localfile, strerror(errno));
    sendline(sock, "3Error: could not access directory\tfake\tfake\t0");
    return;
  }

  direntriescount = scandir(localfile, &direntries, NULL, motsognir_dirsort);
  closedir(dirptr);
  if (direntriescount < 0) {
    syslog(LOG_WARNING, "ERROR: Failed to scan the directory '%s': %s", localfile, strerror(errno));
    return;
  }

  syslog(LOG_INFO, "Found %d items in '%s'", direntriescount, localfile);

  /* iterate on every entry */
  entriesdisplayed = 0;
  for (x = 0; x < direntriescount; x++) {
    char entrytype;
    char entryselector[1024];
    char entryselector_encoded[1024];
    if (direntries[x]->d_name[0] == '.') continue; /* skip any entry starting with '.' (these are either hidden files or system stuff like '.' or '..') */
    if (strcmp(direntries[x]->d_name, "gophermap") == 0) continue;     /* skip gophermap entries (txt) */
    if (strcmp(direntries[x]->d_name, "gophermap.cgi") == 0) continue; /* skip gophermap entries (cgi) */
    if (strcmp(direntries[x]->d_name, "gophermap.php") == 0) continue; /* skip gophermap entries (php) */
    entriesdisplayed += 1;
    if (direntries[x]->d_type == DT_DIR) {
        entrytype = '1';
      } else {
        entrytype = DetectGopherType(direntries[x]->d_name, config->extmap);
    }
    snprintf(entryselector, sizeof(entryselector), "%s%s", directorytolist, direntries[x]->d_name);
    percencode(entryselector, entryselector_encoded, sizeof(entryselector_encoded));
    snprintf(tempstring, 1024, "%c%s\t%s\t%s\t%d", entrytype, direntries[x]->d_name, entryselector_encoded, config->gopherhostname, config->gopherport);
    sendline(sock, tempstring);
    free(direntries[x]);
  }
  free(direntries);

  /* if no entries were displayed, write so */
  if (entriesdisplayed == 0) sendline(sock, "iThis directory is empty.\tfake\tfake\t0");
}


/* cuts a gophermap line into chunks and fills values. returns 0 on success, non-zero on error. */
static int explodegophermapline(char *linebuff, char *itemtype, char *itemdesc, char *itemselector, char *itemserver, long *itemport) {
  int x;
  char tmpstring[16];
  /* first make sure to clear out all variables */
  *itemtype = 0;
  itemdesc[0] = 0;
  itemselector[0] = 0;
  itemserver[0] = 0;
  *itemport = 0;
  /* if the line is empty, stop right now */
  if (*linebuff == 0) {
    *itemtype = 'i';
    return(0);
  }
  /* read the itemtype */
  *itemtype = *linebuff;
  linebuff += 1;
  /* read the item's description */
  for (x = 0;;) {
    if (x == 1023) return(-1);
    if (*linebuff == '\t') break;
    if (*linebuff == 0) return(0);
    itemdesc[x] = *linebuff;
    itemdesc[++x] = 0;
    linebuff += 1;
  }
  linebuff += 1;
  /* read the item's selector */
  for (x = 0;;) {
    if (x == 1023) return(-1);
    if (*linebuff == '\t') break;
    if (*linebuff == 0) return(0);
    itemselector[x] = *linebuff;
    itemselector[++x] = 0;
    linebuff += 1;
  }
  linebuff += 1;
  /* read the item's server */
  for (x = 0;;) {
    if (x == 63) return(-1);
    if (*linebuff == '\t') break;
    if (*linebuff == 0) return(0);
    itemserver[x] = *linebuff;
    itemserver[++x] = 0;
    linebuff += 1;
  }
  linebuff += 1;
  /* read the item's port */
  for (x = 0;;) {
    if (x == 8) return(-1);
    if (*linebuff == '\t') break;
    if (*linebuff == 0) break;
    tmpstring[x] = *linebuff;
    tmpstring[++x] = 0;
    linebuff += 1;
  }
  *itemport = atol(tmpstring);
  if ((*itemport < 1) || (*itemport > 65535)) *itemport = 0;
  return(0);
}


static void computerelativepath(char *result, int result_maxlen, char *curdir, char *relpath) {
  char *tempptr, *lastslash;
  int x;
  /* first glue both paths together */
  snprintf(result, result_maxlen, "%s/%s", curdir, relpath);

  /* make sure we have no // doublons */
  RemoveDoubleChar(result, '/');

  /* simplify all /../ */
  for (;;) {
    tempptr = strstr(result, "/../");
    if (tempptr == NULL) break;
    /* find out where is the last / before our point */
    if (tempptr == result) {
        lastslash = result;
      } else {
        for (lastslash = tempptr - 1; lastslash > result; lastslash--) if (*lastslash == '/') break;
    }
    /* move the right part of URL to the left */
    for (x = 0;; x++) {
      lastslash[x] = tempptr[x + 3];
      if (lastslash[x] == 0) break;
    }
  }

  /* if the result ends with a '/..', we need to simplify this as well */
  x = strlen(result);
  if (x < 3) return;

  x -= 3;
  if ((result[x] == '/') && (result[x + 1] == '.') && (result[x + 2] == '.')) {
    if (x == 0) {
      result[1] = 0;
      return;
    }
    result[x] = 0;
    for (x--; x >= 0; x--) {
      if (result[x] == '/') {
        result[x + 1] = 0;
        break;
      }
    }
  }
}



/* tests whether 'string' starts with 'start'. Returns 0 if not, non-zero
 * otherwise. */
static int stringstartswith(char *string, char *start) {
  while (*start != 0) {
    if (*string != *start) return(0);
    start += 1;
    string += 1;
  }
  return(1);
}


/* tests whether 'string' ends with 'end'. Returns 0 if not, non-zero
 * otherwise. */
static int stringendswith(char *string, char *end) {
  int endlen = strlen(end);
  int stringlen = strlen(string);
  if (stringlen < endlen) return(0);
  if (endlen == 0) return(1);
  string += (stringlen - endlen);
  if (strcmp(string, end) == 0) return(1);
  return(0);
}


/* returns the last char of a string. returns 0 if the string is empty. */
static char lastcharofstring(char *string) {
  int x;
  for (x = 0; string[x] != 0; x++);
  if (x > 0) return(string[x - 1]);
  return(0);
}


static void execCgi(int sock, char *localfile, char **srvsideparams, struct MotsognirConfig *config, char *version, char *scriptname, char *remoteclientaddr, char *launcher) {
  char tmpstring[4096];
  char *cmd;
  int res;
  char *emptyarr[2] = { NULL, NULL };
  FILE *cgifd;
  /* if srvsideparams is NULL, replace it temporarily by an empty array */
  if (srvsideparams == NULL) srvsideparams = emptyarr;
  if ((srvsideparams[0] != NULL) || (srvsideparams[1] != NULL)) {
      syslog(LOG_INFO, "running server-side app '%s' with queries '%s' + '%s'", localfile, srvsideparams[0], srvsideparams[1]);
    } else {
      syslog(LOG_INFO, "running server-side app '%s'", localfile);
  }
  /* Set environment variables */
  setenv("SERVER_NAME", config->gopherhostname, 1);       /* The server's hostname, DNS alias, or IP address as it would appear in self-referencing URLs. */
  sprintf(tmpstring, "%d", config->gopherport);
  setenv("SERVER_PORT", tmpstring, 1);                    /* The server's port, as it would appear in self-referencing URLs. */
  sprintf(tmpstring, "Motsognir/%s", version);
  setenv("SERVER_SOFTWARE", tmpstring, 1);                /* The name and version of the server software. Format: name/version */
  setenv("GATEWAY_INTERFACE", "CGI/1.0", 1);              /* The revision of the CGI specification to which this server complies (typically CGI/1.0 or CGI/1.1) */
  setenv("REMOTE_HOST", remoteclientaddr, 1);             /* remote host's IP address */
  setenv("REMOTE_ADDR", remoteclientaddr, 1);             /* remote host's IP address */
  /* choose one of the available parameters as QUERY_STRING */
  if (srvsideparams[0] != NULL) {
      setenv("QUERY_STRING", srvsideparams[0], 1); /* QUERY_STRING should not be decoded in any fashion! */
    } else if (srvsideparams[1] != NULL) {
      setenv("QUERY_STRING", srvsideparams[1], 1); /* QUERY_STRING should not be decoded in any fashion! */
  }
  /* provide both QUERY_STRING_URL and QUERY_STRING_SEARCH */
  if (srvsideparams[0] != 0) setenv("QUERY_STRING_URL", srvsideparams[0], 1);
  if (srvsideparams[1] != 0) setenv("QUERY_STRING_SEARCH", srvsideparams[1], 1);
  setenv("SCRIPT_NAME", scriptname, 1);
  if (launcher == NULL) {
      cmd = localfile;
    } else {
      cmd = tmpstring;
      snprintf(tmpstring, sizeof(tmpstring), "%s %s", launcher, localfile);
  }
  cgifd = popen(cmd, "r");
  if (cgifd == NULL) {
    syslog(LOG_WARNING, "ERROR: failed to run the server-side app '%s'", localfile);
    return;
  }
  /* read from the CGI application, and send to the socket */
  for (;;) {
    res = fread(tmpstring, 1, sizeof(tmpstring), cgifd);
    if (res <= 0) break;
    send(sock, tmpstring, res, 0);
  }
  /* close the pipe */
  res = pclose(cgifd);
  if (res == -1) {
      syslog(LOG_WARNING, "WARNING: call to server-side app '%s' failed (%s)", localfile, strerror(errno));
    } else if (WEXITSTATUS(res) != 0) {
      syslog(LOG_WARNING, "WARNING: server-side app '%s' terminated with a non-zero exit code (%d)", localfile, WEXITSTATUS(res));
  }
}


static void outputgophermap(int sock, struct MotsognirConfig *config, char *localfile, char *gophermapfile, char *directorytolist, char *remoteclientaddr, char **srvsideparams) {
  FILE *gophermapfd;
  char linebuff[4096];
  char itemtype;
  char itemdesc[1024];
  char itemselector[1024];
  char itemserver[64];
  long itemport;

  /* first check if the gophermap is of dynamic type (cgi or php), and if so, execute it */
  if ((config->cgisupport != 0) && (stringendswith(gophermapfile, ".cgi") != 0)) { /* is it a CGI file? */
    execCgi(sock, gophermapfile, srvsideparams, config, pVer, directorytolist, remoteclientaddr, NULL);
    return;
  } else if ((config->phpsupport != 0) && (stringendswith(gophermapfile, ".php") != 0)) { /* is it a PHP file? */
    execCgi(sock, gophermapfile, srvsideparams, config, pVer, directorytolist, remoteclientaddr, "php");
    return;
  }

  gophermapfd = fopen(gophermapfile, "rb");
  if (gophermapfd == NULL) {
    syslog(LOG_WARNING, "ERROR: Failed to open the gophermap at '%s' (%s)", gophermapfile, strerror(errno));
    return;
  }
  syslog(LOG_INFO, "Response=\"Return gophermap. (%s)", gophermapfile);

  for (;;) {
    if (sockreadline(fileno(gophermapfd), linebuff, 1023, NULL) < 0) break;
    /* if it's an instruction to list files, do it, and move to next line */
    if (strcasecmp(linebuff, "%FILES%") == 0) {
      outputdircontent(sock, config, localfile, directorytolist);
      continue;
    }
    /* explode the gophermap line into separate items */
    if (explodegophermapline(linebuff, &itemtype, itemdesc, itemselector, itemserver, &itemport) != 0) {
      sendline(sock, "3Parsing error\tfake\tfake\t0");
      continue;
    }
    /* if a sub-gophermap script is provided (and feature is enabled), run it now */
    if (itemtype == '=') {
      if (config->subgophermaps != 0) execCgi(sock, itemdesc, NULL, config, pVer, "", remoteclientaddr, NULL);
      continue;
    }
    /* check values, and put default ones if some are missing */
    if (itemserver[0] == 0) snprintf(itemserver, sizeof(itemserver), "%s", config->gopherhostname);
    if (itemport == 0) itemport = 70;
    /* if we are dealing with relative path on the local server, resolve it first */
    if ((itemtype != 'i') && (itemselector[0] != '/') && (itemselector[0] != 0) && (strcasecmp(itemserver, config->gopherhostname) == 0) && (stringstartswith(itemselector, "URL:") == 0)) {
      computerelativepath(linebuff, sizeof(linebuff), directorytolist, itemselector);
      snprintf(itemselector, sizeof(itemselector), "%s", linebuff);
    }
    /* prepare the final line */
    snprintf(linebuff, sizeof(linebuff), "%c%s\t%s\t%s\t%ld", itemtype, itemdesc, itemselector, itemserver, itemport);
    /* send the final line */
    sendline(sock, linebuff);
  }
  fclose(gophermapfd);
}


static void outputdir(int sock, struct MotsognirConfig *config, char *localfile, char *directorytolist, char *remoteclientaddr, char **srvsideparams) {
  char gophermapfile[1024];
  syslog(LOG_INFO, "The resource is a directory");
  if (lastcharofstring(localfile) != '/') strcat(localfile, "/");
  if (lastcharofstring(directorytolist) != '/') strcat(directorytolist, "/");

  /* look around for a gophermap */
  for (;;) {
    /* do we have a static gophermap? */
    sprintf(gophermapfile, "%sgophermap", localfile);
    if (fexist(gophermapfile) != 0) {
      outputgophermap(sock, config, localfile, gophermapfile, directorytolist, remoteclientaddr, srvsideparams);
      break;
    }
    /* do we have a cgi gophermap? */
    if (config->cgisupport != 0) {
      sprintf(gophermapfile, "%sgophermap.cgi", localfile);
      if (fexist(gophermapfile) != 0) {
        execCgi(sock, gophermapfile, srvsideparams, config, pVer, directorytolist, remoteclientaddr, NULL);
        break;
      }
    }
    /* do we have a PHP gophermap? */
    if (config->phpsupport != 0) {
      sprintf(gophermapfile, "%sgophermap.php", localfile);
      if (fexist(gophermapfile) != 0) {
        execCgi(sock, gophermapfile, srvsideparams, config, pVer, directorytolist, remoteclientaddr, "php");
        break;
      }
    }
    /* is there a default gophermap we could use? */
    if (config->defaultgophermap != NULL) {  /* else use the default gophermap, if any is configured */
      outputgophermap(sock, config, localfile, config->defaultgophermap, directorytolist, remoteclientaddr, srvsideparams);
      break;
    }
    /* no gophermap found, simply list files & directories */
    syslog(LOG_INFO, "No gophermap found. Listing directory content");
    outputdircontent(sock, config, localfile, directorytolist);
    break;
  }

  /* send the 'end of list' terminator */
  sendline(sock, ".");
}


/* Shifts a string by n bytes left. */
static void lshiftstring(char *str, int n) {
  int x;
  /* check that n is lower than string's length */
  for (x = 0; str[x] != 0; x++);
  if (x <= n) {
    str[0] = 0;
    return;
  }
  /* perform the shift */
  for (x = 0; ; x++) {
    if (str[x] == 0) break;
    str[x] = str[x + n];
    if (str[x] == 0) break;
  }
}

/* Waits for a connection, forks when a client connection arrives, and
 * returns the forked socket. Fills clientipaddrstr and serveripaddrstr with
 * IP addresses (src and dst) */
static int waitforconn(int gopherport, char *clientipaddrstr, int clientipaddrstr_maxlen, char *serveripaddrstr, int serveripaddrstr_maxlen, struct MotsognirConfig *config) {
  int sockmaster, sockslave;
  int one = 1;  /* this is used by setsockopt() calls on the socket later */
  socklen_t clilen;
  struct sockaddr_in6 serv_addr, cli_addr;
  pid_t mypid;

  serv_addr.sin6_addr = in6addr_any;
  sockmaster = socket(AF_INET6, SOCK_STREAM, 0);
  if (sockmaster < 0) {
    syslog(LOG_WARNING, "FATAL ERROR: socket could not be open (%s)", strerror(errno));
    return(-2);
  }

  /* I set the socket to be reusable, to avoid having to wait for a longish time when the server is restarted */
  if (setsockopt(sockmaster, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one)) < 0) syslog(LOG_WARNING, "WARNING: failed to set REUSEADDR on main socket");

  /* Initialize socket structure */
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin6_family = AF_INET6;
  if (config->bind == NULL) {   /* bind all */
      serv_addr.sin6_addr = in6addr_any;
    } else {    /* bind on a user-specfied address only */
      if (inet_pton(AF_INET6, config->bind, &(serv_addr.sin6_addr)) != 1) {
        syslog(LOG_WARNING, "FATAL ERROR: failed to parse the IP address bind value. Please check your 'bind' configuration.");
        return(-2);
      }
  }
  serv_addr.sin6_port = htons(gopherport);

  /* Explicitely mark the socket as NOT being IPV6-only. This is needed on systems that have a system-wide sysctl net.inet6.ip6.v6only=1 (by default every *nix besides Linux...) */
  #ifdef IPV6_BINDV6ONLY  /* check if the BINDV6ONLY option exists at all */
    {
      int zero = 0;
      setsockopt(sockmaster, IPPROTO_IPV6, IPV6_BINDV6ONLY, (char *)&zero, (socklen_t)sizeof(zero));
    }
  #endif

  /* Now bind the host address using a bind() call */
  if (bind(sockmaster, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
     syslog(LOG_WARNING, "FATAL ERROR: binding failed (%s)", strerror(errno));
     return(-2);
  }

  /* Start listening for clients */
  listen(sockmaster, 10);
  clilen = sizeof(cli_addr);

  /* Ignore SIGCHLD - this way I don't have to worry about my children becoming little zombies */
  signal(SIGCHLD, SIG_IGN);

  /* I don't want to get notified about SIGHUP */
  signal(SIGHUP, SIG_IGN);

  syslog(LOG_INFO, "motsognir v" pVer " process started");

  /* fork off */
  mypid = fork();
  if (mypid == 0) { /* I'm the child, do nothing */
      /* nothing to do - just continue */
    } else if (mypid > 0) { /* I'm the parent - quit now */
      close(sockmaster);
      return(-1);
    } else {  /* error condition */
      close(sockmaster);
      syslog(LOG_WARNING, "Failed to dameonize the motsognir process (%s)", strerror(errno));
      return(-2);
  }

  /* Set the user file creation mask to zero. */
  umask(0);

  /* Redirect standard file descriptors to /dev/null - no reason for a daemon to write to stdout, stderr or stdin */
  freopen( "/dev/null", "r", stdin);
  freopen( "/dev/null", "w", stdout);
  freopen( "/dev/null", "w", stderr);

  /* I want to be the pack master now (aka session leader) */
  if (setsid() == -1) syslog(LOG_WARNING, "WARNING: setsid() failed (%s)", strerror(errno));

  /* if a chroot() is configured, execute it now */
  if (config->chroot != NULL) {
    chdir(config->chroot);
    if (chroot(config->chroot) != 0) {
      syslog(LOG_WARNING, "Failed to chroot(): %s", strerror(errno));
      return(-2);
    }
  }

  /* set the working directory to the root directory */
  if (chdir ("/") == -1) syslog(LOG_WARNING, "WARNING: failed to switch to / directory (%s)", strerror(errno));

  /* sanitize the environment (remove a few useless env variables) */
  sanitizeenv();

  /* drop root privileges, if configuration says so */
  if (config->runasuser != NULL) {
    if (getuid() != 0) {
        syslog(LOG_WARNING, "A 'RunAsUser' directive has been configured, but the process has not been launched under root account. The 'RunAsUser' directive is therefore ignored.");
      } else { /* if I'm root, drop off privileges */
        if (droproot(config) != 0) {
            return(-2);
          } else {
            syslog(LOG_WARNING, "Successfully dropped root privileges. Motsognir runs as user '%s' now.", config->runasuser);
        }
    }
  }

  for (;;) {
    /* Accept actual connection from the client - here process will go to sleep mode, waiting for incoming connections */
    sockslave = accept(sockmaster, (struct sockaddr *)&cli_addr, &clilen);
    if (sockslave < 0) {
      syslog(LOG_WARNING, "FATAL ERROR: accepting connection failed (%s)", strerror(errno));
      close(sockmaster);
      return(-2);
    }

    /* fork out, close the master socket and return the client socket */
    mypid = fork();
    if (mypid == 0) { /* I'm the child */
        static char logprefix[128];
        close(sockmaster);
        if (inet_ntop(cli_addr.sin6_family, &cli_addr.sin6_addr, clientipaddrstr, clientipaddrstr_maxlen) == NULL) {
          syslog(LOG_WARNING, "Failed to fetch client's IP address: %s", strerror(errno));
          sprintf(clientipaddrstr, "UNKNOWN");
        }
        /* now fetch the local address (useful esp. for multihomed systems) */
        clilen = sizeof(serv_addr);
        if ((getsockname(sockslave, (struct sockaddr *) &serv_addr, &clilen) < 0) || (inet_ntop(serv_addr.sin6_family, &serv_addr.sin6_addr, serveripaddrstr, serveripaddrstr_maxlen) == NULL)) {
          syslog(LOG_WARNING, "Failed to fetch server's IP address: %s", strerror(errno));
          sprintf(serveripaddrstr, "UNKNOWN");
        }
        /* convert IPv4 "IPV6MAPPED" addresses to "normal" IPv4 strings, if needed */
        if (stringstartswith(clientipaddrstr, "::ffff:") != 0) lshiftstring(clientipaddrstr, 7);
        if (stringstartswith(serveripaddrstr, "::ffff:") != 0) lshiftstring(serveripaddrstr, 7);
        /* set logprefix to contain the client's address */
        sprintf(logprefix, "motsognir [%s]", clientipaddrstr);
        openlog(logprefix, LOG_PID, LOG_DAEMON); /* set up the logging to log with PID and peer's IP address */
        syslog(LOG_INFO, "new connection to %s", serveripaddrstr);
        /* if no gopher hostname was set, use the server's address */
        if (config->gopherhostname == NULL) config->gopherhostname = strdup(serveripaddrstr);
        /* Restore the default SIGCHLD handler - we need this because we might call CGI scripts via popen() later, and need to know their exit status */
        signal(SIGCHLD, SIG_DFL);
        return(sockslave);
      } else if (mypid > 0) { /* I'm the parent */
        /* just close child's socket to avoid messing with it */
        close(sockslave);
      } else { /* error condition */
        syslog(LOG_WARNING, "FATAL ERROR: fork() failed!");
        close(sockslave);
        close(sockmaster);
        return(-2);
    }
  }
}


/* sends the content of a txt file to a socket, and escapes '.' lines, if present */
static void sendtxtfiletosock(int sock, char *filename) {
  FILE *fd;
  char *linebuff;
  int linebuff_len = 1024 * 1024;
  /* allocate a big buffer to read file's lines (1M) */
  linebuff = malloc(linebuff_len);
  if (linebuff == NULL) {
    syslog(LOG_WARNING, "ERROR: Out of memory while trying to allocate buffer for file");
    return;
  }
  fd = fopen(filename, "rb");
  if (fd == NULL) { /* file could not be opened */
    syslog(LOG_WARNING, "ERROR: File '%s' could not be opened", filename);
    free(linebuff);
    return;
  }
  for (;;) {
    if (sockreadline(fileno(fd), linebuff, linebuff_len - 1, NULL) < 0) break;
    if ((linebuff[0] == '.') && (linebuff[1] == 0)) sprintf(linebuff, ". "); /* if the line is a single dot, escape it */
    sendline(sock, linebuff);
  }
  fclose(fd);
  free(linebuff);
}


static void sendbinfiletosock(int sock, char *filename) {
  FILE *fd;
  unsigned char *buff;
  size_t bytesread;
  int buff_len = 1024 * 1024;  /* allocate a big buffer to read file's content (1M) */
  buff = malloc(buff_len);
  if (buff == NULL) {
    syslog(LOG_WARNING, "ERROR: Out of memory while trying to allocate buffer for file");
    return;
  }
  fd = fopen(filename, "rb");
  if (fd == NULL) { /* file could not be opened */
    syslog(LOG_WARNING, "ERROR: File '%s' could not be opened", filename);
    free(buff);
    return;
  }
  for (;;) {
    bytesread = fread(buff, 1, buff_len, fd);
    if (bytesread <= 0) break; /* end of file (I guess) */
    send(sock, buff, bytesread, 0);
  }
  free(buff);
  fclose(fd);
}


/* Looks at a requests to detect whether it might be HTTP. Returns 0 if no HTTP detected, non-zero otherwise. */
static int requestlookslikehttp(char *req) {
  if ((req[0] == 'G') && (req[1] == 'E') && (req[2] == 'T') && (req[3] == ' ') && (req[4] == '/')) { /* starts by 'GET /'... */
    if (strstr(req, " HTTP/") != NULL) return(1); /* contains ' HTTP/' -> it is a HTTP request */
  }
  return(0);
}


/* check for file evasion. Returns 0 if all is ok. non-zero otherwise. */
static int checkforevasion(char *gopherroot, char *localfile) {
  char resolvedpath[PATH_MAX];
  if (realpath(localfile, resolvedpath) == NULL) return(0);
  strcat(resolvedpath, "/"); /* add a trailing / char to avoid false positives on the root resource */
  if (stringstartswith(resolvedpath, gopherroot) == 0) {
    syslog(LOG_WARNING, "Evasion check: path '%s' (%s) do not seem to belong to '%s'", localfile, resolvedpath, gopherroot);
    return(1);
  }
  return(0);
}


/* performs various security checks on a gopher request. Returns NULL if all is ok, or a pointer to an error string otherwise. */
static char *gophersecuritycheck(char *GophRequest) {
  int x;
  if (strlen(GophRequest) > 512) return("The gopher request is longer than 512 bytes. RFC 1436 states that the selector shouldn't be longer than 256 bytes.");
  if (strstr(GophRequest, "\t\t") != NULL) return("Client's request contains two TAB characters, one after the other. It shouldn't ever happen.");
  if (lastcharofstring(GophRequest) == '\t') return("Client's request ends by a TAB character. There's no situation where that should happen.");
  /* check that the request contains valid text (possibly encoded as UTF-8) */
  for (x = 0; GophRequest[x] != 0; x++) {
    /* look for control chars */
    if ((GophRequest[x] > 0) && (GophRequest[x] < 32)) return("A control char (ASCII 1..31) has been found in the request. There's no reason for such char to be present there.");
    /* is it a low-ASCII char? */
    if ((GophRequest[x] & bx10000000) == 0) continue;
    /* it must be an UTF-8 sequence (or some invalid stuff) - check how many chars would be expected */
    if ((GophRequest[x] & bx11100000) == bx11000000) { /* two bytes sequence */
        if ((GophRequest[x+1] & bx11000000) == bx10000000) {
          x += 1;
          continue;
        }
      } else if ((GophRequest[x] & bx11110000) == bx11100000) { /* three bytes sequence */
        if (((GophRequest[x+1] & bx11000000) == bx10000000) && ((GophRequest[x+2] & bx11000000) == bx10000000)) {
          x += 2;
          continue;
        }
    }
    /* if I'm here, it means that the UTF-8 sequence was unrecognized */
    return("Detected an invalid UTF-8 sequence.");
  }
  /* Alles klar, proceed! */
  return(NULL);
}


/* checks whether the given element is a directory. Returns 0 if not, non-zero otherwise. */
static int is_it_a_directory(char *localfile) {
  DIR* dir;
  dir = opendir(localfile);
  if (dir != NULL) {  /* Directory exists. */
      closedir(dir);
      return(1);
    } else if (ENOENT == errno) {  /* Directory does not exist. */
      return(0);
    } else {  /* opendir() failed for some other reason. */
      return(0);
  }
}


static void BuildLocalFileAndRootDir(char *localfile, char *rootdir, struct MotsognirConfig *config, char *directorytolist) {
  /* if this is a username-like URL (/~user/file.dat) AND UserDir is defined, perform a substitution */
  if ((directorytolist[0] == '/') && (directorytolist[1] == '~') && (config->userdir != NULL)) {
      char username[1024];
      int x;
      /* extract the user name */
      for (x = 0;; x++) {
        username[x] = directorytolist[x + 2];
        if (username[x] == '/') {
          username[x] = 0;
          break;
        }
        if (username[x] == 0) break;
      }
      /* compute user's home directory and use as root directory */
      sprintf(rootdir, config->userdir, username);
      /* build the path all together */
      sprintf(localfile, "%s%s", rootdir, directorytolist + x + 2);
    } else { /* else it's a normal path to append to gopherroot */
      strcpy(rootdir, config->gopherroot);
      sprintf(localfile, "%s%s", config->gopherroot, directorytolist);
  }
}


/* returns non-zero if the filename looks like a gophermap, zero otherwise */
static int islocalfileagophermap(char *file) {
  if ((stringendswith(file, "/gophermap") != 0) ||
      (stringendswith(file, "/gophermap.cgi") != 0) ||
      (stringendswith(file, "/gophermap.php") != 0)) return(1);
  return(0);
}


int main(int argc, char **argv) {
  char *securitycheckresult;
  char directorytolist[4096];
  char localfile[4096];
  char rootdir[4096];
  char remoteclientaddr[64];
  char localserveraddr[64];
  char **srvsideparams;
  char gophertype;
  char *configfile = CONFIGFILE;
  int sock;
  struct MotsognirConfig config;
  time_t StartTime;

  if (argc > 1) {
    int x;
    for (x = 1; x < argc; x++) {
      if (strcmp(argv[x], "--config") == 0) {
          x++;
          if (x < argc) configfile = argv[x];
        } else { /* unknown command line */
          about(pVer, pDate, HOMEPAGE);
          return(1);
      }
    }
  }

  /* load motsognir's configuration from file */
  if (loadconfig(&config, configfile) != 0) {
    puts("ERROR: A configuration error has been detected. Check the logs for details.");
    return(9);
  }

  sock = waitforconn(config.gopherport, remoteclientaddr, sizeof(remoteclientaddr), localserveraddr, sizeof(localserveraddr), &config);
  if (sock == -1) return(0);
  if (sock < 0) {
    puts("ERROR: a fatal error occured. check the logs for details.");
    return(2);
  }

  StartTime = time(NULL);

  if (sockreadline(sock, directorytolist, sizeof(directorytolist), &StartTime) < 0) {
    syslog(LOG_WARNING, "Error during selector receiving phase. Connection aborted.");
    close(sock);
    return(0);
  }
  syslog(LOG_INFO, "Query='%s'", directorytolist);
  if (directorytolist[0] == 0) {   /* Empty request means "gimme the root listing" */
    directorytolist[0] = '/';
    directorytolist[1] = 0;
  }

  if (requestlookslikehttp(directorytolist) != 0) {
    char discarddatabuff[4096];
    sendbackhttperror(sock, &config);
    recv(sock, discarddatabuff, 4096, MSG_DONTWAIT); /* read whatever request the peer sent us, to drain the socket before closing it (otherwise the tcp stack would trigger a ugly RST) */
    close(sock);
    return(0);
  }

  /* detect requests for foreign URLs and return a simple html redirecting page */
  if ((directorytolist[0] == 'U') && (directorytolist[1] == 'R') && (directorytolist[2] == 'L') && (directorytolist[3] == ':')) {
    exturlredirector(sock, directorytolist);
    close(sock);
    return(0);
  }

  /* a request should start with a / (if not, prepend it with one) */
  if (directorytolist[0] != '/') {
    memmove(directorytolist+1, directorytolist, strlen(directorytolist)+1); /* first move the whole thing by one position to the right */
    directorytolist[0] = '/';
  }

  /* separate server side params from the 'real' query */
  srvsideparams = explode_serverside_params_from_query(directorytolist, &config);

  /* Decode percent-encoded data - note that this must be done AFTER we separated server side params, because QUERY_STRING must NOT be decoded in any way */
  if (percdecode(directorytolist) != 0) {
    syslog(LOG_WARNING, "Percent decoding on request failed. Query aborted.");
    return(0);
  }

  /* Once we decoded the request, check that it doesn't contain any nasty stuff */
  securitycheckresult = gophersecuritycheck(directorytolist);
  if (securitycheckresult != NULL) {
    syslog(LOG_INFO, "The gopher security module has detected a suspect condition. The query won't be processed. Reason: %s", securitycheckresult);
    close(sock);
    return(0);
  }

  /* build the localfile path, and the root directory (the latter is necessary for further evasion checks */
  BuildLocalFileAndRootDir(localfile, rootdir, &config, directorytolist);

  /* Remove double occurences of slashes in paths */
  RemoveDoubleChar(directorytolist, '/');
  RemoveDoubleChar(localfile, '/');

  syslog(LOG_INFO, "Requested resource: %s / Local resource: %s", directorytolist, localfile);

  if (checkforevasion(rootdir, localfile) != 0) {
    syslog(LOG_INFO, "Evasion attempt. Forbidden!");
    sendline(sock, "iForbidden!\tfake\tfake\t0");
    sendline(sock, ".");
    close(sock);
    return(0);
  }

  if (is_it_a_directory(localfile) != 0) {
    outputdir(sock, &config, localfile, directorytolist, remoteclientaddr, srvsideparams);
    close(sock);
    return(0);
  }

  /* if NOT a directory */
  if ((strcmp(directorytolist, "/caps.txt") == 0) && (config.capssupport != 0)) {  /* If asking for /caps.txt, return it. */
    syslog(LOG_INFO, "Returned caps.txt data");
    printcapstxt(sock, &config, pVer);
    sendline(sock, ".");
    close(sock);
    return(0);
  }

  /* the query is requesting a file - does it exist at all?
     if client asks for a gophermap, we fake a 'not found' message as well */
  if ((fexist(localfile) == 0) || (islocalfileagophermap(localfile) != 0)) {
    syslog(LOG_INFO, "FileExists check: the file doesn't exists");
    sendline(sock, "3The selected resource doesn't exist!\tfake\tfake\t0");
    sendline(sock, "iThe selected resource cannot be located.\tfake\tfake\t0");
    sendline(sock, ".");
    close(sock);
    return(0);
  }

  /* in 'paranoid' mode, only allow access to files that are world-readable */
  if (config.paranoidmode != 0) {
    struct stat statbuf;
    if (stat(localfile, &statbuf) != 0) {
      /* error while reading attributes */
      syslog(LOG_INFO, "stat() failed: %s", strerror(errno));
      sendline(sock, "3Internal error\tfake\tfake\t0");
      sendline(sock, "iInternal error\tfake\tfake\t0");
      sendline(sock, ".");
      close(sock);
      return(0);
    } else if ((statbuf.st_mode & S_IROTH) != S_IROTH) {
      /* not world-readable */
      syslog(LOG_INFO, "Paranoid mode check failed: file is not world-readable");
      sendline(sock, "3Permission denied\tfake\tfake\t0");
      sendline(sock, "iPermission denied\tfake\tfake\t0");
      sendline(sock, ".");
      close(sock);
      return(0);
    }
  }

  /* if the query is pointing to a CGI file, and CGI support is enabled - execute the query */
  if ((strcmp(getfileextension(localfile), "cgi") == 0) && (config.cgisupport != 0)) {
    execCgi(sock, localfile, srvsideparams, &config, pVer, directorytolist, remoteclientaddr, NULL);
    close(sock);
    return(0);
  }

  /* if the query is pointing to a PHP file, and PHP support is enabled - execute the query */
  if ((strcmp(getfileextension(localfile), "php") == 0) && (config.phpsupport != 0)) {
    execCgi(sock, localfile, srvsideparams, &config, pVer, directorytolist, remoteclientaddr, "php");
    close(sock);
    return(0);
  }

  /* we want a normal file's content */
  syslog(LOG_INFO, "Returning file '%s'", localfile);
  gophertype = DetectGopherType(localfile, config.extmap);
  switch (gophertype) {
    case '0':
    case '2':
    case '6':
      sendtxtfiletosock(sock, localfile);
      sendline(sock, ".");
      break;
    default:
      sendbinfiletosock(sock, localfile);
      break;
  }

  close(sock);
  syslog(LOG_INFO, "connection closed. duration: %us", (unsigned int)(time(NULL) - StartTime));
  return(0);
}
