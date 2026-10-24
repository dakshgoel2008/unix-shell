# Unix Shell in C

A command-line shell, like bash, built from scratch in C.

```
$ echo "hello world" | tr a-z A-Z
HELLO WORLD
$ ls > files.txt && echo saved
saved
$ sleep 10 &
[1] 4821
```

## Features

- Runs any program, e.g. `ls`, `grep`, `python`
- Pipes: `cat file | sort | uniq`
- Saving and reading files: `>`, `>>`, `<`, `2>`
- Running commands in the background with `&`
- Chaining commands with `;`, `&&`, `||`
- Quotes, variables (`$HOME`) and `~`
- Command history with `history` and `!!`
- Built-in commands: `cd`, `pwd`, `export`, `jobs`, `exit`
- Ctrl+C stops the running command, not the shell

## Run it

```bash
make
./shell
```

Run the tests with `make test`.

## Code layout

| File | What it does |
|---|---|
| `main.c` | Reads each line and runs it |
| `parser.c` | Breaks the line into commands |
| `executor.c` | Starts the programs and connects pipes |
| `builtins.c` | `cd`, `pwd`, `export` and other built-ins |
| `jobs.c` | Keeps track of background commands |
| `history.c` | Remembers past commands |
