#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <unistd.h>

#include "must.h"
#include "watchdir.h"

#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + NAME_MAX + 1))

/**
 * Remove watch descriptor wd from the list of watched directories.
 *
 * Returns 1 if the watch descriptor was found in the list of watched
 * directories, 0 otherwise. If the watch descriptor is found and
 * path is non-NULL, copy the watched path into the path variable.
 */
int remove_watch(int inotify_fd, int wd, char *path, size_t pathlen) {
  watched_dir *d = find_dir_from_wd(wd);

  if (d != NULL) {
    if (path != NULL) {
      strncpy(path, d->path, pathlen);
    }
    inotify_rm_watch(inotify_fd, wd);
    LIST_REMOVE(d, links);
    del_watched_dir(d);
    return 1;
  }
  return 0;
}

/**
 * Add inotify watch recursively up to a specified depth.
 */
void add_watch_recursive(int inotify_fd, const char *base_path, int depth,
                         int current_depth) {
  int watch_fd;
  watched_dir *wd;
  DIR *dir;
  struct dirent *entry;

  if (current_depth > depth) {
    return;
  }

  MUST(watch_fd = inotify_add_watch(inotify_fd, base_path,
                                    IN_CREATE | IN_DELETE | IN_DELETE_SELF),
       "failed to add inotify watch");

  wd = new_watched_dir(watch_fd, current_depth, base_path);

  LIST_INSERT_HEAD(&watched_dirs, wd, links);

  dir = opendir(base_path);
  if (!dir) {
    perror("opendir");
    return;
  }

  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char path[PATH_MAX];
    snprintf(path, PATH_MAX, "%s/%s", base_path, entry->d_name);

    struct stat statbuf;
    if (stat(path, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
      add_watch_recursive(inotify_fd, path, depth, current_depth + 1);
    }
  }

  closedir(dir);
}

int main(int argc, char *argv[]) {
  int inotify_fd;
  int depth;

  if (argc < 3) {
    fprintf(stderr, "Usage: %s <path> <depth>\n", argv[0]);
    return EXIT_FAILURE;
  }

  LIST_INIT(&watched_dirs);

  const char *path = argv[1];
  depth = atoi(argv[2]);

  if (depth < 0) {
    fprintf(stderr, "Error: Depth must be a non-negative integer\n");
    return EXIT_FAILURE;
  }

  // Print the path to verify it
  printf("Attempting to open directory: %s\n", path);

  // Check if the directory exists
  struct stat statbuf;
  MUST(stat(path, &statbuf), "failed to check if directory exists");

  // Check if it is a directory
  if (!S_ISDIR(statbuf.st_mode)) {
    fprintf(stderr, "Error: %s is not a directory\n", path);
    return EXIT_FAILURE;
  }

  // Initialize inotify
  MUST(inotify_fd = inotify_init(), "failed to allocate inotify fd");

  // Add watch on the specified directory and its subdirectories up to the
  // specified depth
  add_watch_recursive(inotify_fd, path, depth, 0);

  printf("Watching directory: %s and its subdirectories up to depth: %d\n",
         path, depth);

  // Buffer to store inotify events
  char buffer[EVENT_BUF_LEN];

  // Event loop
  while (!LIST_EMPTY(&watched_dirs)) {
    int length;

    MUST(length = read(inotify_fd, buffer, EVENT_BUF_LEN),
         "failed to read inotify events");

    int i = 0;
    while (i < length) {
      struct inotify_event *event = (struct inotify_event *)&buffer[i];
      i += EVENT_SIZE + event->len;

      // we get this after a watched directory has been deleted
      if (event->mask & IN_IGNORED)
        continue;

      watched_dir *d = find_dir_from_wd(event->wd);

      if (d == NULL) {
        fprintf(stderr, "unknown watch descriptor %d\n", event->wd);
      }

      if (event->len) {
        char event_path[PATH_MAX];
        snprintf(event_path, PATH_MAX, "%s/%s", d->path, event->name);

        if (event->mask & IN_CREATE) {
          if (event->mask & IN_ISDIR) {
            printf("Directory created: %s\n", event_path);
            // Add watch to the new directory if within the specified depth
            add_watch_recursive(inotify_fd, event_path, depth, d->depth + 1);
            dump_watch_list();
          } else {
            printf("File created: %s\n", event_path);
          }
        } else if (event->mask & IN_DELETE) {
          if (event->mask & IN_ISDIR) {
            printf("Directory deleted: %s\n", event_path);
          } else {
            printf("File deleted: %s\n", event_path);
          }
        }
      } else if (event->mask & IN_DELETE_SELF) {
        char path[PATH_MAX];
        if (remove_watch(inotify_fd, event->wd, path, PATH_MAX)) {
          printf("Remove watch on directory: %s\n", path);
        }
      }
    }
  }

  // Clean up
  close(inotify_fd);

  return EXIT_SUCCESS;
}
