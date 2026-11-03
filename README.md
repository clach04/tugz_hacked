Hacked tugz amalgation

Small experiments with code changes, rather than the full code base.

Currently Windows only.

## unzip `-y` mode

    unzip -y file.zip

If the contents of a zip file are not a directory, it creates a directory with
the zip file name (without `.zip`) and extracts the files into that directory.

The point: some archives are "loose" — `a.txt`, `sub/b.txt` at the top — and
extracting one scatters files over the current directory. `-y` keeps those
tidy, while archives that already carry their own folder extract exactly as
plain unzip would.

### When it wraps

The archive counts as "a directory" when every extracted entry sits under one
common first path component (`proj/a.txt`, `proj/b.txt`, ...). Anything else
wraps:

- a file at the top level (`a.txt` beside `sub/b.txt`)
- entries under two different top folders (`x.txt` and `y/z.txt`)
- entries with no `/` at all, including under `-j` (junk paths), which strips
  the folders, so a junked run always wraps

The check uses each entry's mapped name — the same path normalization the
extractor applies (backslash names from FAT-made archives become `/`, `..` and
`.` are collapsed, Windows-invalid characters are replaced) — so what it
decides matches what would really be written. Entries excluded by the member
list, `-x`, or an unsupported method do not affect the decision.

### The directory name

Taken from the name of the file actually opened (so `unzip -y foo`, which opens
`foo.zip`, still yields `foo`): the basename after the last `/` or `\`, with a
trailing `.zip` or `.ZIP` removed, case-insensitively. If stripping would
leave nothing (`.zip`), the full basename is kept. It is created if missing, by
the same code that makes the `-d` directory.

### Interactions

- `-d exdir` always wins: with `-d`, `-y` does nothing.
- `-t`, `-l`, `-v`, `-p`, `-c`, `-z`: `-y` is accepted but inert.
- standard input (`unzip -y -`): no archive name, so no wrap.
- a wildcard (`unzip -y "*.zip"`): each archive gets its own directory; the
  root is reset per archive, so one archive cannot extract into another's.
- `--y` negates, as with any option.
- unless `-q` is given, it prints `note:  extracting into foo/` (after the
  `Archive:` line).

### Examples

    # proj.zip: proj/a.txt, proj/b.txt
    unzip -y proj.zip          -> proj/a.txt, proj/b.txt   (no proj/proj/)

    # foo.zip: a.txt, sub/b.txt
    unzip -y foo.zip           -> foo/a.txt, foo/sub/b.txt
    unzip -y foo.zip -d out    -> out/a.txt, out/sub/b.txt (-d wins)
    unzip foo.zip              -> a.txt, sub/b.txt         (no -y: as before)

### Implementation notes

Changes are all in `unzip.c`: a `yflag` field, a `case 'y'` in
`apply_options`, `y` in `late_option`'s letter list, one usage line, and a
`wrap_root()` helper called at the top of `extract_or_test` — before `plan()`,
which sizes its buffers from the extraction root's length, so the root must be
settled first. `wrap_root` reads the central directory once (the same
`next_entry`/`entry_name` loop `plan` uses), and on wrap sets `u->root` to
`<name>/` and clears `u->rooted` so `make_root` creates it. No other file or
mechanism is touched.

Tested: wrapped and loose archives, `-d` winning, extensionless and `.ZIP`
names, wildcards with mixed archives, `-y -l`/`-y -t` making nothing, stdin,
`-j`, a late `foo.zip -y`, and `--y`.


## Build

See https://github.com/skeeto/tugz#build
if that doesnt work, here are some substitutes.

### gcc

    gcc -O2 -nostartfiles -o unzip.exe unzip.c
