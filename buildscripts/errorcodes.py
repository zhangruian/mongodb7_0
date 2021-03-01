#!/usr/bin/env python3
"""Produce a report of all assertions in the MongoDB server codebase.

Parses .cpp files for assertions and verifies assertion codes are distinct.
Optionally replaces zero codes in source code with new distinct values.
"""

import bisect
import os.path
import sys
from collections import defaultdict, namedtuple
from optparse import OptionParser
from functools import reduce

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts import utils  # pylint: disable=wrong-import-position

try:
    import regex as re
except ImportError:
    print("*** Run 'pip3 install --user regex' to speed up error code checking")
    import re  # type: ignore

ASSERT_NAMES = ["uassert", "massert", "fassert", "fassertFailed"]
MAXIMUM_CODE = 9999999  # JIRA Ticket + XX

# pylint: disable=invalid-name
codes = []  # type: ignore
# pylint: enable=invalid-name

# Each AssertLocation identifies the C++ source location of an assertion
AssertLocation = namedtuple("AssertLocation", ['sourceFile', 'byteOffset', 'lines', 'code'])

list_files = False  # pylint: disable=invalid-name


def parse_source_files(callback, src_root):
    """Walk MongoDB sourcefiles and invoke a callback for each AssertLocation found."""

    quick = [r"assert", r"Exception", r"ErrorCodes::Error", r"LOGV2", r"logAndBackoff"]

    patterns = [
        re.compile(r"(?:u|m(?:sg)?)asser(?:t|ted)(?:NoTrace)?\s*\(\s*(\d+)", re.MULTILINE),
        re.compile(r"(?:DB|Assertion)Exception\s*[({]\s*(\d+)", re.MULTILINE),
        re.compile(r"fassert(?:Failed)?(?:WithStatus)?(?:NoTrace)?(?:StatusOK)?\s*\(\s*(\d+)",
                   re.MULTILINE),
        re.compile(r"LOGV2(?:\w*)?\s*\(\s*(\d+)", re.MULTILINE),
        re.compile(r"logAndBackoff\(\s*(\d+)", re.MULTILINE),
        re.compile(r"ErrorCodes::Error\s*[({]\s*(\d+)", re.MULTILINE)
    ]

    for source_file in utils.get_all_source_files(prefix=src_root):
        if list_files:
            print('scanning file: ' + source_file)

        with open(source_file, 'r', encoding='utf-8') as fh:
            text = fh.read()

            if not any(zz in text for zz in quick):
                continue

            matchiters = [p.finditer(text) for p in patterns]
            for matchiter in matchiters:
                for match in matchiter:
                    code = match.group(1)
                    code_offset = match.start(1)

                    # Note that this will include the text of the full match but will report the
                    # position of the beginning of the code portion rather than the beginning of the
                    # match. This is to position editors on the spot that needs to change.
                    this_loc = AssertLocation(source_file, code_offset,
                                              text[match.start():match.end()], code)

                    callback(this_loc)


def get_line_and_column_for_position(loc, _file_cache=None):
    """Convert an absolute position in a file into a line number."""
    if _file_cache is None:
        _file_cache = {}
    if loc.sourceFile not in _file_cache:
        with open(loc.sourceFile) as fh:
            text = fh.read()
            line_offsets = [0]
            for line in text.splitlines(True):
                line_offsets.append(line_offsets[-1] + len(line))
            _file_cache[loc.sourceFile] = line_offsets

    # These are both 1-based, but line is handled by starting the list with 0.
    line = bisect.bisect(_file_cache[loc.sourceFile], loc.byteOffset)
    column = loc.byteOffset - _file_cache[loc.sourceFile][line - 1] + 1
    return (line, column)


def is_terminated(lines):
    """Determine if assert is terminated, from .cpp/.h source lines as text."""
    code_block = " ".join(lines)
    return ';' in code_block or code_block.count('(') - code_block.count(')') <= 0


def get_next_code(seen, server_ticket=0):
    """Find next unused assertion code.

    Called by: SConstruct and main()
    Since SConstruct calls us, codes[] must be global OR WE REPARSE EVERYTHING
    """
    if not codes:
        (_, _, seen) = read_error_codes()

    if server_ticket:
        # Each SERVER ticket is allocated 100 error codes ranging from TICKET_00 -> TICKET_99.
        def generator(seen, ticket):
            avail_codes = list(range(ticket * 100, (ticket + 1) * 100))
            avail_codes.reverse()
            while avail_codes:
                code = avail_codes.pop()
                if str(code) in seen:
                    continue
                yield code
            return "No more available codes for ticket. Ticket: {}".format(ticket)

        return generator(seen, server_ticket)

    # No server ticket. Return a generator that counts starting at highest + 1.
    highest = reduce(lambda x, y: max(int(x), int(y)), (loc.code for loc in codes))
    return iter(range(highest + 1, MAXIMUM_CODE))


def check_error_codes():
    """Check error codes as SConstruct expects a boolean response from this function."""
    (_, errors, _) = read_error_codes()
    return len(errors) == 0


def read_error_codes(src_root='src/mongo'):
    """Define callback, call parse_source_files() with callback, save matches to global codes list."""
    seen = {}
    errors = []
    dups = defaultdict(list)
    skips = []
    malformed = []  # type: ignore

    # define validation callbacks
    def check_dups(assert_loc):
        """Check for duplicates."""
        codes.append(assert_loc)
        code = assert_loc.code

        if not code in seen:
            seen[code] = assert_loc
        else:
            if not code in dups:
                # on first duplicate, add original to dups, errors
                dups[code].append(seen[code])
                errors.append(seen[code])

            dups[code].append(assert_loc)
            errors.append(assert_loc)

    def validate_code(assert_loc):
        """Check for malformed codes."""
        code = int(assert_loc.code)
        if code > MAXIMUM_CODE:
            malformed.append(assert_loc)
            errors.append(assert_loc)

    def callback(assert_loc):
        validate_code(assert_loc)
        check_dups(assert_loc)

    parse_source_files(callback, src_root)

    if "0" in seen:
        code = "0"
        bad = seen[code]
        errors.append(bad)
        line, col = get_line_and_column_for_position(bad)
        print("ZERO_CODE:")
        print("  %s:%d:%d:%s" % (bad.sourceFile, line, col, bad.lines))

    for loc in skips:
        line, col = get_line_and_column_for_position(loc)
        print("EXCESSIVE SKIPPING OF ERROR CODES:")
        print("  %s:%d:%d:%s" % (loc.sourceFile, line, col, loc.lines))

    for code, locations in list(dups.items()):
        print("DUPLICATE IDS: %s" % code)
        for loc in locations:
            line, col = get_line_and_column_for_position(loc)
            print("  %s:%d:%d:%s" % (loc.sourceFile, line, col, loc.lines))

    for loc in malformed:
        line, col = get_line_and_column_for_position(loc)
        print("MALFORMED ID: %s" % loc.code)
        print("  %s:%d:%d:%s" % (loc.sourceFile, line, col, loc.lines))

    return (codes, errors, seen)


def replace_bad_codes(errors, next_code_generator):  # pylint: disable=too-many-locals
    """Modify C++ source files to replace invalid assertion codes.

    For now, we only modify zero codes.

    Args:
        errors: list of AssertLocation
        next_code_generator: generator -> int, next non-conflicting assertion code
    """
    zero_errors = [e for e in errors if int(e.code) == 0]
    skip_errors = [e for e in errors if int(e.code) != 0]

    for loc in skip_errors:
        line, col = get_line_and_column_for_position(loc)
        print("SKIPPING NONZERO code=%s: %s:%d:%d" % (loc.code, loc.sourceFile, line, col))

    # Dedupe, sort, and reverse so we don't have to update offsets as we go.
    for assert_loc in reversed(sorted(set(zero_errors))):
        (source_file, byte_offset, _, _) = assert_loc
        line_num, _ = get_line_and_column_for_position(assert_loc)
        print("UPDATING_FILE: %s:%s" % (source_file, line_num))

        ln = line_num - 1

        with open(source_file, 'r+') as fh:
            print("LINE_%d_BEFORE:%s" % (line_num, fh.readlines()[ln].rstrip()))

            fh.seek(0)
            text = fh.read()
            assert text[byte_offset] == '0'
            fh.seek(0)
            fh.write(text[:byte_offset])
            fh.write(str(next(next_code_generator)))
            fh.write(text[byte_offset + 1:])
            fh.seek(0)

            print("LINE_%d_AFTER :%s" % (line_num, fh.readlines()[ln].rstrip()))


def coerce_to_number(ticket_value):
    """Coerce the input into a number.

    If the input is a number, return itself. Otherwise parses input strings of two forms.
    'SERVER-12345' and '12345' will both return 12345'.
    """
    if isinstance(ticket_value, int):
        return ticket_value

    ticket_re = re.compile(r'(?:SERVER-)?(\d+)', re.IGNORECASE)
    matches = ticket_re.fullmatch(ticket_value)
    if not matches:
        print("Unknown ticket number. Input: " + ticket_value)
        return -1

    return int(matches.group(1))


def main():
    """Main."""
    parser = OptionParser(description=__doc__.strip())
    parser.add_option("--fix", dest="replace", action="store_true", default=False,
                      help="Fix zero codes in source files [default: %default]")
    parser.add_option("-q", "--quiet", dest="quiet", action="store_true", default=False,
                      help="Suppress output on success [default: %default]")
    parser.add_option("--list-files", dest="list_files", action="store_true", default=False,
                      help="Print the name of each file as it is scanned [default: %default]")
    parser.add_option(
        "--ticket", dest="ticket", type="str", action="store", default=0,
        help="Generate error codes for a given SERVER ticket number. Inputs can be of"
        " the form: `--ticket=12345` or `--ticket=SERVER-12345`.")
    (options, _) = parser.parse_args()

    global list_files  # pylint: disable=global-statement,invalid-name
    list_files = options.list_files

    (_, errors, seen) = read_error_codes()
    ok = len(errors) == 0

    if ok and options.quiet:
        return

    next_code_gen = get_next_code(seen, coerce_to_number(options.ticket))

    print("ok: %s" % ok)
    if not options.replace:
        print("next: %s" % next(next_code_gen))

    if ok:
        sys.exit(0)
    elif options.replace:
        replace_bad_codes(errors, next_code_gen)
    else:
        print(ERROR_HELP)
        sys.exit(1)


ERROR_HELP = """
ERRORS DETECTED. To correct, run "buildscripts/errorcodes.py --fix" to replace zero codes.
Other errors require manual correction.
"""

if __name__ == "__main__":
    main()
