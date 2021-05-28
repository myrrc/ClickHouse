from typing import TextIO
from time import time
from collections import Counter
from pygments import highlight
from pygments.formatters import HtmlFormatter
from pygments.lexers import CppLexer
from jinja2 import Environment, FileSystemLoader
from tqdm import tqdm

import sys
import argparse
import os.path

bounds = {
    "lines": [90, 75],
    "funcs": [90, 75],
    "tests": [50, 15]
    }

files = []
tests = []
tests_names = []

class FileEntry:
    def __init__(self, path, lines_with_hits, funcs_with_hits, tests_with_hits):
        self.full_path = path

        self.name = path.split("/")[-1]
        self.url = self.name + ".html"

        a, b, c, d = get_hit(lines_with_hits, funcs_with_hits)

        self.lines = a, b, percent(a, b)
        self.funcs = c, d, percent(c, d)

        self.tests = len(tests_with_hits), percent(len(tests_with_hits), len(tests))

        self.lines_with_hits = lines_with_hits
        self.funcs_with_hits = funcs_with_hits
        self.tests_with_hits = tests_with_hits

class DirEntry:
    def __init__(self, path):
        self.name = path
        self.url = path

        self.files = []

        self.lines = 0, 0, 0
        self.funcs = 0, 0, 0
        self.tests = 0, 0

        self.tests_with_hits = set()

    def add_file(self, f):
        self.files.append(f)

        line_hit, line_total = self.lines[0] + f.lines[0], self.lines[1] + f.lines[1]
        func_hit, func_total = self.funcs[0] + f.funcs[0], self.funcs[1] + f.funcs[1]

        test_hit = max(self.tests[0], f.tests[0])

        self.lines = line_hit, line_total, percent(line_hit, line_total)
        self.funcs = func_hit, func_total, percent(func_hit, func_total)
        self.tests = test_hit, percent(test_hit, len(tests))

        self.tests_with_hits.update(f.tests_with_hits)

def percent(a, b):
  return 0 if b == 0 else int(a * 100 / b)

def get_hit(lines, funcs):
    return len([e for e in lines if e[1]]), len(lines), \
           len([e for e in funcs if e[2]]), len(funcs)

def accumulate_data():
    data = {}

    for test in tests:
        for source_id, (test_funcs_hit, test_lines_hit) in test.items():
            if source_id not in data:
                data[source_id] = set(test_funcs_hit), set(test_lines_hit)
            else:
                funcs_hit, lines_hit = data[source_id]
                funcs_hit.update(test_funcs_hit)
                lines_hit.update(test_lines_hit)

    return data

def _prepare_data():
    out = []
    data = accumulate_data()
    prefix = ""

    for i, (path, funcs_instrumented, lines_instrumented) in enumerate(files):
        if "contrib/" in path or "base/" in path:
            continue

        index = path.find("src") + 4
        prefix = path[:index]
        path = path[index:]

        dirs, file_name = os.path.split(path)

        funcs_hit, lines_hit = data[i] if i in data else (set(), set())

        lines_with_hits = [(line, line in lines_hit) for line in lines_instrumented]
        funcs_with_hits = [(name, line, index in funcs_hit) for index, (name, line) in funcs_instrumented.items()]
        tests_with_hits = [j for j, test in enumerate(tests) if i in test.keys()]

        test_file = FileEntry(path, lines_with_hits, funcs_with_hits, tests_with_hits)

        found = False

        for d in out:
            if d.name == dirs:
                d.add_file(test_file)
                found = True
                break

        if not found:
            out.append(DirEntry(dirs))
            out[-1].add_file(test_file)

    return prefix, sorted(out, key=lambda x: x.name)

def generate_dir_page(out_dir, entry: DirEntry, env, upper_level=False):
    special_entries = []

    if not upper_level:
        special_entries.append(DirEntry("../"))

    special_entries.append(entry)
    special_entries[-1].url=""

    template = env.get_template('directory.html')

    entries = sorted(entry.files, key=lambda x: x.name)

    if upper_level:
        for e in entries:
            e.url = e.name

    output = template.render(
        entries=entries,
        special_entries=special_entries,
        bounds=bounds,
        tests_total=len(tests))

    path = os.path.join(out_dir, entry.name)

    os.makedirs(path, exist_ok=True)

    with open(os.path.join(path, "index.html"), "w") as f:
        f.write(output)

def generate_file_page(out_dir, prefix, entry: FileEntry, env):
    template = env.get_template('file.html')

    src_file_path = prefix + entry.full_path

    if not os.path.exists(src_file_path):
        print("No file", src_file_path)
        return

    with open(src_file_path, "r") as src_file:
        lines = highlight(src_file.read(), CppLexer(),
            HtmlFormatter(linenos=True, hl_lines=[e[0] for e in entry.lines_with_hits]))

        output = template.render(
            lines=lines,
            entry=entry,
            bounds=bounds,
            tests_total=len(tests))

        with open(os.path.join(out_dir, entry.full_path) + ".html", "w") as f:
            f.write(output)

def generate_html(out_dir, files, tests, tests_names):
    file_loader = FileSystemLoader(os.path.abspath(os.path.dirname(__file__)) + '/templates', encoding='utf8')

    env = Environment(loader=file_loader)
    env.trim_blocks = True
    env.lstrip_blocks = True
    env.rstrip_blocks = True

    prefix, entries = _prepare_data()

    root_entry = DirEntry("")

    for dir_entry in tqdm(entries):
        root_entry.add_file(dir_entry)

        generate_dir_page(out_dir, dir_entry, env)

        for sf_entry in tqdm(dir_entry.files):
            generate_file_page(out_dir, prefix, sf_entry, env)

    generate_dir_page(out_dir, root_entry, env, upper_level=True)

def read_report(f: TextIO):
    global files
    global tests
    global tests_names

    elapsed = time()

    for i in range(int(f.readline().split()[1])): # files
        rel_path, funcs_count, lines_count = f.readline().split()

        funcs = {}

        for j in range(int(funcs_count)):
            mangled_name, start_line, edge_index = f.readline().split()
            funcs[int(edge_index)] = mangled_name, int(start_line)

        lines = [int(f.readline()) for j in range(int(lines_count))]

        files.append((rel_path, funcs, lines))

    tests_sources = {}

    while True:
        token = f.readline()

        if token == "TEST\n":
            if len(tests_sources) > 0:
                tests.append(tests_sources)
                tests_sources = {}
            token = f.readline()
        elif not token.startswith("SOURCE"):
            if len(tests_sources) > 0:
                tests.append(tests_sources)
            break

        source_id = int(token.split()[1])
        funcs = list(map(int, next(f).split()))
        lines = list(map(int, next(f).split()))

        tests_sources[source_id] = funcs, lines

    tests_names = f.readlines()

    # assert len(tests_names) == len(tests), f"{len(tests_names)} != {len(tests)}"

    print("Read the report, took {}s. {} tests, {} source files".format(
        int(time() - elapsed), len(tests), len(files)))

def main():
    parser = argparse.ArgumentParser(prog='CCR converter')
    parser.add_argument('ccr_report_file')
    parser.add_argument('html_dir')

    args = parser.parse_args()

    with open(args.ccr_report_file, "r") as f:
        read_report(f)

    generate_html(args.html_dir, files, tests, tests_names)

if __name__ == '__main__':
    main()
