#define _GNU_SOURCE
#include <argp.h>
#include <ctype.h>
#include <json-c/json.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/**
 * @file shrill.c
 * @brief A monitor management utility for Sway and X11.
 *
 * Provides an interface for querying connected displays and controlling
 * which outputs are active. Supports both Sway (via swaymsg JSON IPC)
 * and X11 (via xrandr) backends, selected automatically based on
 * environment detection.
 */

static char doc[] = "Manage monitor outputs on Sway and X11.\v"
                    "If no command is given, the interactive menu is shown.";

static char args_doc[] = "[COMMAND [OUTPUT]]";

/**
 * @brief Represents a single display output.
 */
typedef struct {
  char *name_;  /**< Output identifier as reported by the backend. */
  int focused_; /**< Non-zero if this output is currently active. */
} Output;

/**
 * @brief Dynamic array of output descriptors.
 */
typedef struct {
  Output *data_; /**< Pointer to array of Output structures. */
  size_t len_;   /**< Number of elements in the array. */
} OutputList;

/**
 * @brief Available command modes.
 */
typedef enum {
  CmdNone,   /**< No command specified (defaults to menu). */
  CmdList,   /**< List all connected outputs. */
  CmdActive, /**< Re-activate the currently focused output. */
  CmdSwitch, /**< Switch to a specific output. */
  CmdMenu    /**< Show interactive selection menu. */
} Command;

/**
 * @brief Parsed command-line arguments.
 */
struct Args {
  Command cmd_;  /**< Selected command mode. */
  char *output_; /**< Target output name for switch command. */
  char *rate_;   /**< Optional refresh rate specification. */
};

/**
 * @brief Backend function pointer for retrieving outputs.
 *
 * Populates an OutputList with all currently connected displays.
 */
static OutputList (*get_outputs)(void);

/**
 * @brief Backend function pointer for activating an output.
 *
 * Enables the target output and disables all others. The rate parameter
 * may be NULL for automatic mode selection.
 */
static void (*switch_to_output)(const char *target, const char *rate,
                                OutputList *outs);

/**
 * @brief Releases all resources associated with an OutputList.
 */
static void outputListFree(OutputList *out_list) {
  for (size_t idx = 0; idx < out_list->len_; idx++) {
    free(out_list->data_[idx].name_);
  }
  free(out_list->data_);
  out_list->data_ = NULL;
  out_list->len_ = 0;
}

/**
 * @brief Executes a shell command and captures its stdout.
 *
 * Uses a growing buffer to handle arbitrary output sizes. The caller
 * is responsible for freeing the returned string.
 *
 * @param cmd Shell command to execute.
 * @return Newly allocated string containing command output.
 */
static char *readPipe(const char *cmd) {
  FILE *file_ptr = popen(cmd, "r");
  if (!file_ptr) {
    perror("popen");
    exit(EXIT_FAILURE);
  }

  char *buf = NULL;
  size_t cap = 0;
  size_t len = 0;
  char chunk[4096];
  size_t bytes_read;

  while ((bytes_read = fread(chunk, 1, sizeof(chunk), file_ptr)) > 0) {
    if (len + bytes_read + 1 > cap) {
      cap = (len + bytes_read + 1) * 2;
      char *tmp = realloc(buf, cap);
      if (!tmp) {
        free(buf);
        perror("realloc");
        exit(EXIT_FAILURE);
      }
      buf = tmp;
    }
    memcpy(buf + len, chunk, bytes_read);
    len += bytes_read;
  }

  pclose(file_ptr);

  if (!buf) {
    buf = malloc(1);
    if (!buf) {
      perror("malloc");
      exit(EXIT_FAILURE);
    }
  }
  buf[len] = '\0';
  return buf;
}

/**
 * @brief Forks and executes an argv array, waiting for completion.
 */
static void run(const char *const *argv) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    exit(EXIT_FAILURE);
  }
  if (pid == 0) {
    execvp(argv[0], (char *const *)argv);
    perror(argv[0]);
    _exit(127);
  }
  waitpid(pid, NULL, 0);
}

/*
 * SWAY BACKEND IMPLEMENTATION
 *
 * Communicates with Sway via swaymsg(1), parsing JSON output using
 * json-c. The focused field indicates which output currently has
 * keyboard focus.
 */

/**
 * @brief Fetches raw output data from swaymsg as a JSON array.
 */
static json_object *swayFetchOutputs(void) {
  char *raw = readPipe("swaymsg -t get_outputs -r");
  json_object *arr = json_tokener_parse(raw);
  free(raw);

  if (!arr || !json_object_is_type(arr, json_type_array)) {
    (void)fprintf(stderr, "Error: failed to parse swaymsg output\n");
    exit(EXIT_FAILURE);
  }
  return arr;
}

/**
 * @brief Builds an OutputList from Sway's JSON response.
 */
static OutputList swayGetOutputs(void) {
  json_object *arr = swayFetchOutputs();
  size_t len = json_object_array_length(arr);

  OutputList out_list = {.data_ = malloc(len * sizeof(Output)), .len_ = len};
  if (!out_list.data_) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }

  for (size_t idx = 0; idx < len; idx++) {
    json_object *obj = json_object_array_get_idx(arr, idx);
    json_object *name_obj;
    json_object *focused_obj;

    json_object_object_get_ex(obj, "name", &name_obj);
    json_object_object_get_ex(obj, "focused", &focused_obj);

    out_list.data_[idx].name_ = strdup(json_object_get_string(name_obj));
    out_list.data_[idx].focused_ = json_object_get_boolean(focused_obj);
  }

  json_object_put(arr);
  return out_list;
}

/**
 * @brief Enables the target output and disables all others in Sway.
 *
 * When a refresh rate is specified, constructs a mode string using
 * the output's preferred resolution with the custom rate.
 */
static void swaySwitchToOutput(const char *target, const char *rate,
                               OutputList *outs) {
  char *mode_str = NULL;

  if (rate) {
    json_object *arr = swayFetchOutputs();
    size_t count = json_object_array_length(arr);

    for (size_t idx = 0; idx < count; idx++) {
      json_object *obj = json_object_array_get_idx(arr, idx);
      json_object *name_obj;
      json_object_object_get_ex(obj, "name", &name_obj);

      if (strcmp(json_object_get_string(name_obj), target) != 0) {
        continue;
      }

      json_object *modes;
      json_object_object_get_ex(obj, "modes", &modes);

      if (modes && json_object_array_length(modes) > 0) {
        json_object *mode_obj = json_object_array_get_idx(modes, 0);
        json_object *width_obj;
        json_object *height_obj;
        json_object_object_get_ex(mode_obj, "width", &width_obj);
        json_object_object_get_ex(mode_obj, "height", &height_obj);

        if (asprintf(&mode_str, "%dx%d@%sHz", json_object_get_int(width_obj),
                     json_object_get_int(height_obj), rate) < 0) {
          mode_str = NULL;
        }
      }
      break;
    }
    json_object_put(arr);
  }

  if (mode_str) {
    const char *cmd[] = {"swaymsg", "--",   "output", target,
                         "enable",  "mode", mode_str, NULL};
    run(cmd);
    free(mode_str);
  } else {
    const char *cmd[] = {"swaymsg", "--", "output", target, "enable", NULL};
    run(cmd);
  }

  for (size_t idx = 0; idx < outs->len_; idx++) {
    if (strcmp(outs->data_[idx].name_, target) == 0) {
      continue;
    }
    const char *cmd[] = {"swaymsg", "--", "output", outs->data_[idx].name_,
                         "disable", NULL};
    run(cmd);
  }
}

/*
 * X11 BACKEND IMPLEMENTATION
 *
 * Parses xrandr(1) output using regex to identify connected displays.
 * The "primary" designation is used to determine the active output.
 */

/**
 * @brief Returns a compiled regex for parsing xrandr connected lines.
 *
 * The regex is compiled once and reused on subsequent calls.
 */
static regex_t *x11GetOutputRegex(void) {
  static regex_t regex;
  static int compiled = 0;

  if (!compiled) {
    static const char Pat[] = "^([A-Za-z0-9_-]+) connected( primary)?";
    int err = regcomp(&regex, Pat, REG_EXTENDED);
    if (err != 0) {
      char msg[256];
      regerror(err, &regex, msg, sizeof(msg));
      (void)fprintf(stderr, "regex error: %s\n", msg);
      exit(EXIT_FAILURE);
    }
    compiled = 1;
  }
  return &regex;
}

/**
 * @brief Checks if a line matches the specified output name.
 *
 * Used to locate the beginning of an output's mode list in xrandr output.
 */
static int checkOutputMatch(const char *line, const char *output,
                            regex_t *regex) {
  regmatch_t matches[3];
  if (regexec(regex, line, 3, matches, 0) != 0) {
    return 0;
  }

  size_t name_len = matches[1].rm_eo - matches[1].rm_so;
  return (strncmp(line + matches[1].rm_so, output, name_len) == 0 &&
          output[name_len] == '\0');
}

/**
 * @brief Extracts a mode string from an indented xrandr mode line.
 *
 * Returns non-zero and populates *result if a mode was found.
 */
static int extractMode(const char *line, char **result) {
  if (line[0] != ' ' && line[0] != '\t') {
    return 0;
  }

  const char *ptr = line;
  while (*ptr && isspace((unsigned char)*ptr)) {
    ptr++;
  }

  const char *end_ptr = ptr;
  while (*end_ptr && !isspace((unsigned char)*end_ptr)) {
    end_ptr++;
  }

  *result = strndup(ptr, end_ptr - ptr);
  return 1;
}

/**
 * @brief Retrieves the first (preferred) mode for an output from xrandr.
 *
 * Scans xrandr query output to find the output, then extracts the
 * first mode from its mode list.
 */
static char *preferredMode(const char *output) {
  FILE *file_ptr = popen("xrandr --query", "r");
  if (!file_ptr) {
    perror("popen");
    exit(EXIT_FAILURE);
  }

  regex_t *regex = x11GetOutputRegex();
  char *line = NULL;
  size_t line_cap = 0;
  ssize_t line_len;
  int found = 0;
  char *result = NULL;

  while ((line_len = getline(&line, &line_cap, file_ptr)) > 0) {
    if (line_len > 0 && line[line_len - 1] == '\n') {
      line[--line_len] = '\0';
    }

    if (!found) {
      if (checkOutputMatch(line, output, regex)) {
        found = 1;
      }
    } else if (line_len > 0) {
      if (extractMode(line, &result)) {
        break;
      }
      break;
    }
  }

  free(line);
  pclose(file_ptr);
  return result;
}

/**
 * @brief Builds an OutputList from xrandr query output.
 *
 * Parses connected outputs, marking the primary output as focused.
 */
static OutputList x11GetOutputs(void) {
  FILE *file_ptr = popen("xrandr --query", "r");
  if (!file_ptr) {
    perror("popen");
    exit(EXIT_FAILURE);
  }

  regex_t *regex = x11GetOutputRegex();
  OutputList out_list = {.data_ = NULL, .len_ = 0};
  size_t cap = 0;
  char *line = NULL;
  size_t line_cap = 0;
  ssize_t line_len;

  while ((line_len = getline(&line, &line_cap, file_ptr)) > 0) {
    if (line[line_len - 1] == '\n') {
      line[line_len - 1] = '\0';
    }

    regmatch_t matches[3];
    if (regexec(regex, line, 3, matches, 0) != 0) {
      continue;
    }

    size_t name_len = matches[1].rm_eo - matches[1].rm_so;
    char *name = strndup(line + matches[1].rm_so, name_len);
    if (!name) {
      perror("strndup");
      exit(EXIT_FAILURE);
    }

    if (out_list.len_ == cap) {
      cap = cap ? cap * 2 : 4;
      Output *tmp = realloc(out_list.data_, cap * sizeof(Output));
      if (!tmp) {
        free(name);
        outputListFree(&out_list);
        perror("realloc");
        exit(EXIT_FAILURE);
      }
      out_list.data_ = tmp;
    }

    out_list.data_[out_list.len_].name_ = name;
    out_list.data_[out_list.len_].focused_ = (matches[2].rm_so != -1);
    out_list.len_++;
  }

  free(line);
  pclose(file_ptr);
  return out_list;
}

/**
 * @brief Enables the target output and disables all others via xrandr.
 *
 * Uses the output's preferred mode with optional custom refresh rate.
 * When no rate is specified, uses --auto for mode selection.
 */
static void x11SwitchToOutput(const char *target, const char *rate,
                              OutputList *outs) {
  for (size_t idx = 0; idx < outs->len_; idx++) {
    const char *name = outs->data_[idx].name_;
    if (strcmp(name, target) == 0) {
      if (rate) {
        char *mode = preferredMode(name);
        const char *cmd[] = {"xrandr", "--output",           name,
                             "--mode", mode ? mode : "auto", "--rate",
                             rate,     "--primary",          NULL};
        run(cmd);
        free(mode);
      } else {
        const char *cmd[] = {"xrandr", "--output",  name,
                             "--auto", "--primary", NULL};
        run(cmd);
      }
    } else {
      const char *cmd[] = {"xrandr", "--output", name, "--off", NULL};
      run(cmd);
    }
  }
}

/*
 * COMMAND IMPLEMENTATIONS
 */

/**
 * @brief Prints a formatted list of connected monitors.
 */
static void cmdList(const OutputList *out_list) {
  printf("Connected monitors:\n");
  for (size_t idx = 0; idx < out_list->len_; idx++) {
    printf("  %s%s\n", out_list->data_[idx].name_,
           out_list->data_[idx].focused_ ? " (active)" : "");
  }
}

/**
 * @brief Returns the name of the currently active output, or NULL if none.
 */
static const char *findActive(const OutputList *out_list) {
  for (size_t idx = 0; idx < out_list->len_; idx++) {
    if (out_list->data_[idx].focused_) {
      return out_list->data_[idx].name_;
    }
  }
  return NULL;
}

/**
 * @brief Presents an interactive menu for output selection.
 *
 * Prompts the user to select an output by number, then switches to it.
 * If only one monitor is connected, exits immediately.
 */
static void cmdMenu(OutputList *out_list, const char *rate) {
  if (out_list->len_ < 2) {
    printf("Only one monitor connected: %s\n", out_list->data_[0].name_);
    exit(EXIT_SUCCESS);
  }

  printf("Select monitor to use (others will be disabled):\n\n");
  for (size_t idx = 0; idx < out_list->len_; idx++) {
    printf("  %zu) %s%s\n", idx + 1, out_list->data_[idx].name_,
           out_list->data_[idx].focused_ ? " (active)" : "");
  }
  putchar('\n');

  printf("Choice [1-%zu]: ", out_list->len_);
  (void)fflush(stdout);

  char buf[64];
  if (!fgets(buf, sizeof(buf), stdin)) {
    (void)fprintf(stderr, "Invalid selection\n");
    exit(EXIT_FAILURE);
  }

  char *end_ptr;
  long choice = strtol(buf, &end_ptr, 10);
  if (end_ptr == buf || choice < 1 || (size_t)choice > out_list->len_) {
    (void)fprintf(stderr, "Invalid selection\n");
    exit(EXIT_FAILURE);
  }

  const char *selected = out_list->data_[choice - 1].name_;
  char *user_rate = NULL;

  if (!rate) {
    printf("Refresh rate (empty = auto): ");
    (void)fflush(stdout);

    char rbuf[64];
    if (fgets(rbuf, sizeof(rbuf), stdin)) {
      rbuf[strcspn(rbuf, "\n")] = '\0';
      if (rbuf[0] != '\0') {
        user_rate = strdup(rbuf);
      }
    }
  }

  const char *use_rate = rate ? rate : user_rate;
  switch_to_output(selected, use_rate, out_list);

  if (use_rate) {
    printf("Switched to: %s @ %sHz\n", selected, use_rate);
  } else {
    printf("Switched to: %s\n", selected);
  }

  free(user_rate);
}

/*
 * Argument parsing using argp.
 */

static const struct argp_option Options[] = {
    {"list", 'l', 0, 0, "List connected monitors", 0},
    {"active", 'a', 0, 0, "Switch to active monitor only", 0},
    {"switch", 's', "OUT", 0, "Switch to specified monitor", 0},
    {"menu", 'm', 0, 0, "Interactive selection menu (default)", 0},
    {"rate", 'r', "HZ", 0, "Set refresh rate (e.g. 100)", 0},
    {0}};

/**
 * @brief argp parser function for command-line options and arguments.
 *
 * Supports both positional commands (list, active, menu, switch OUTPUT)
 * and short options (-l, -a, -m, -s OUT, -r HZ).
 */
static error_t parseOpt(int key, char *arg, struct argp_state *state) {
  struct Args *args = state->input;

  switch (key) {
  case 'l':
    args->cmd_ = CmdList;
    break;
  case 'a':
    args->cmd_ = CmdActive;
    break;
  case 'm':
    args->cmd_ = CmdMenu;
    break;
  case 'r':
    args->rate_ = arg;
    break;
  case 's':
    args->cmd_ = CmdSwitch;
    args->output_ = arg;
    break;

  case ARGP_KEY_ARG:
    if (state->arg_num == 0) {
      if (strcmp(arg, "list") == 0) {
        args->cmd_ = CmdList;
      } else if (strcmp(arg, "active") == 0) {
        args->cmd_ = CmdActive;
      } else if (strcmp(arg, "menu") == 0) {
        args->cmd_ = CmdMenu;
      } else if (strcmp(arg, "switch") == 0) {
        args->cmd_ = CmdSwitch;
      } else if (strcmp(arg, "help") == 0) {
        argp_state_help(state, stdout, ARGP_HELP_STD_HELP);
      } else {
        argp_error(state, "unknown command: %s", arg);
      }
    } else if (state->arg_num == 1) {
      if (args->cmd_ == CmdSwitch) {
        args->output_ = arg;
      } else {
        argp_error(state, "unexpected argument: %s", arg);
      }
    } else {
      argp_error(state, "too many arguments");
    }
    break;

  case ARGP_KEY_END:
    if (args->cmd_ == CmdNone) {
      args->cmd_ = CmdMenu;
    }
    if (args->cmd_ == CmdSwitch && !args->output_) {
      argp_error(state, "switch requires an output name");
    }
    break;

  default:
    return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

static const struct argp Argp = {Options, parseOpt, args_doc, doc,
                                 NULL,    NULL,     NULL};

/**
 * @brief Entry point: detects backend, parses arguments, and executes the
 * requested command.
 *
 * Selects Sway or X11 backend based on environment variables (SWAYSOCK or
 * DISPLAY). Default action is to show the interactive menu.
 */
int main(int argc, char *argv[]) {
  if (getenv("SWAYSOCK")) {
    get_outputs = swayGetOutputs;
    switch_to_output = swaySwitchToOutput;
  } else if (getenv("DISPLAY")) {
    get_outputs = x11GetOutputs;
    switch_to_output = x11SwitchToOutput;
  } else {
    (void)fprintf(stderr, "Error: No supported display server detected.\n");
    return EXIT_FAILURE;
  }

  struct Args args = {.cmd_ = CmdNone, .output_ = NULL, .rate_ = NULL};
  argp_parse(&Argp, argc, argv, 0, NULL, &args);

  OutputList out_list = get_outputs();
  if (out_list.len_ == 0) {
    (void)fprintf(stderr, "Error: no connected monitors found\n");
    return EXIT_FAILURE;
  }

  switch (args.cmd_) {
  case CmdList:
    cmdList(&out_list);
    break;

  case CmdActive: {
    const char *active = findActive(&out_list);
    if (!active) {
      (void)fprintf(stderr, "No active monitor found\n");
      outputListFree(&out_list);
      return EXIT_FAILURE;
    }
    switch_to_output(active, args.rate_, &out_list);
    break;
  }

  case CmdSwitch: {
    int found = 0;
    for (size_t idx = 0; idx < out_list.len_; idx++) {
      if (strcmp(out_list.data_[idx].name_, args.output_) == 0) {
        found = 1;
        break;
      }
    }
    if (!found) {
      (void)fprintf(stderr, "Error: '%s' is not connected\n", args.output_);
      cmdList(&out_list);
      outputListFree(&out_list);
      return EXIT_FAILURE;
    }
    switch_to_output(args.output_, args.rate_, &out_list);
    break;
  }

  case CmdMenu:
    cmdMenu(&out_list, args.rate_);
    break;

  case CmdNone:
    break;
  }

  outputListFree(&out_list);
  return EXIT_SUCCESS;
}
