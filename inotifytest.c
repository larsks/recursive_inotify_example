#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <unistd.h>

#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + NAME_MAX + 1))

typedef struct _watched_dir {
  int wd;
  int depth;
  char path[PATH_MAX];

  LIST_ENTRY(_watched_dir) links;
} watched_dir;

LIST_HEAD(watched_dir_list, _watched_dir) watched_dirs;

void dump_watch_list() {
  printf("WATCH LIST\n");
  watched_dir *d;
  LIST_FOREACH(d, &watched_dirs, links) {
    printf("wd %d depth %d name %s\n", d->wd, d->depth, d->path);
  }
}

watched_dir *find_dir_from_path(char *path) {
  watched_dir *d;
  LIST_FOREACH(d, &watched_dirs, links) {
    if (strcmp(d->path, path) == 0) {
      return d;
    }
  }

  return NULL;
}

watched_dir *find_dir_from_wd(int wd) {
  watched_dir *d;
  LIST_FOREACH(d, &watched_dirs, links) {
    if (d->wd == wd)
      return d;
  }

  return NULL;
}

watched_dir *new_watched_dir(int wd, int depth, const char *path) {
  watched_dir *d = (watched_dir *)malloc(sizeof(watched_dir));
  d->wd = wd;
  d->depth = depth;
  strncpy(d->path, path, PATH_MAX);
  return d;
}

// Function to add inotify watch recursively up to a specified depth
void add_watch_recursive(int inotify_fd, const char *base_path, int depth,
                         int current_depth) {
  printf("add_watch_recursive %d %s\n", current_depth, base_path);
  if (current_depth > depth) {
    return;
  }

  printf("base_path %s \t", base_path);
  int watch_fd = inotify_add_watch(inotify_fd, base_path,
                                   IN_CREATE | IN_DELETE | IN_DELETE_SELF);
  printf("watch_fd %d \n", watch_fd);
  if (watch_fd < 0) {
    perror("inotify_add_watch");
    return;
  }

  watched_dir *wd = new_watched_dir(watch_fd, current_depth, base_path);

  LIST_INSERT_HEAD(&watched_dirs, wd, links);

  DIR *dir = opendir(base_path);
  if (!dir) {
    perror("opendir");
    return;
  }

  struct dirent *entry;
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
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <path> <depth>\n", argv[0]);
    return EXIT_FAILURE;
  }

  LIST_INIT(&watched_dirs);

  const char *path = argv[1];
  int depth = atoi(argv[2]);

  if (depth < 0) {
    fprintf(stderr, "Error: Depth must be a non-negative integer\n");
    return EXIT_FAILURE;
  }

  // Print the path to verify it
  printf("Attempting to open directory: %s\n", path);

  // Check if the directory exists
  struct stat statbuf;
  if (stat(path, &statbuf) != 0) {
    perror("stat");
    return EXIT_FAILURE;
  }

  // Check if it is a directory
  if (!S_ISDIR(statbuf.st_mode)) {
    fprintf(stderr, "Error: %s is not a directory\n", path);
    return EXIT_FAILURE;
  }

  // Initialize inotify
  int inotify_fd = inotify_init();
  if (inotify_fd < 0) {
    perror("inotify_init");
    return EXIT_FAILURE;
  }

  // Add watch on the specified directory and its subdirectories up to the
  // specified depth
  add_watch_recursive(inotify_fd, path, depth, 0);

  printf("Watching directory: %s and its subdirectories up to depth: %d\n",
         path, depth);

  // Buffer to store inotify events
  char buffer[EVENT_BUF_LEN];

  // Event loop
  while (!LIST_EMPTY(&watched_dirs)) {
    int length = read(inotify_fd, buffer, EVENT_BUF_LEN);

    if (length < 0) {
      perror("read");
      exit(EXIT_FAILURE);
    }

    int i = 0;
    while (i < length) {
      struct inotify_event *event = (struct inotify_event *)&buffer[i];
      i += EVENT_SIZE + event->len;
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
        watched_dir *remove = find_dir_from_wd(event->wd);
        if (remove != NULL) {
          printf("Watched directory deleted: %s\n", remove->path);
          inotify_rm_watch(inotify_fd, event->wd);
          LIST_REMOVE(remove, links);
          free(remove);
          dump_watch_list();
        } else {
          fprintf(stderr, "received delete event for unknown watch\n");
        }
      }
    }
  }

  // Clean up
  close(inotify_fd);

  return EXIT_SUCCESS;
}
