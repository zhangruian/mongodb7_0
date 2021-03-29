"""Extension to SCons providing advanced static library dependency tracking.

These modifications to a build environment, which can be attached to
StaticLibrary and Program builders via a call to setup_environment(env),
cause the build system to track library dependencies through static libraries,
and to add them to the link command executed when building programs.

For example, consider a program 'try' that depends on a lib 'tc', which in
turn uses a symbol from a lib 'tb' which in turn uses a library from 'ta'.

Without this package, the Program declaration for "try" looks like this:

Program('try', ['try.c', 'path/to/${LIBPREFIX}tc${LIBSUFFIX}',
                'path/to/${LIBPREFIX}tb${LIBSUFFIX}',
                'path/to/${LIBPREFIX}ta${LIBSUFFIX}',])

With this library, we can instead write the following

Program('try', ['try.c'], LIBDEPS=['path/to/tc'])
StaticLibrary('tc', ['c.c'], LIBDEPS=['path/to/tb'])
StaticLibrary('tb', ['b.c'], LIBDEPS=['path/to/ta'])
StaticLibrary('ta', ['a.c'])

And the build system will figure out that it needs to link libta.a and libtb.a
when building 'try'.

A StaticLibrary S may also declare programs or libraries, [L1, ...] to be dependent
upon S by setting LIBDEPS_DEPENDENTS=[L1, ...], using the same syntax as is used
for LIBDEPS, except that the libraries and programs will not have LIBPREFIX/LIBSUFFIX
automatically added when missing.
"""

# Copyright (c) 2010, Corensic Inc., All Rights Reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

from collections import OrderedDict
import copy
import os
import textwrap

import SCons.Errors
import SCons.Scanner
import SCons.Util


class Constants:
    Libdeps = "LIBDEPS"
    LibdepsCached = "LIBDEPS_cached"
    LibdepsDependents = "LIBDEPS_DEPENDENTS"
    LibdepsInterface ="LIBDEPS_INTERFACE"
    LibdepsPrivate = "LIBDEPS_PRIVATE"
    LibdepsTags = "LIBDEPS_TAGS"
    LibdepsTagExpansion = "LIBDEPS_TAG_EXPANSIONS"
    MissingLibdep = "MISSING_LIBDEP_"
    ProgdepsDependents = "PROGDEPS_DEPENDENTS"
    SysLibdeps = "SYSLIBDEPS"
    SysLibdepsCached = "SYSLIBDEPS_cached"
    SysLibdepsPrivate = "SYSLIBDEPS_PRIVATE"

class dependency:
    Public, Private, Interface = list(range(3))

    def __init__(self, value, deptype, listed_name):
        self.target_node = value
        self.dependency_type = deptype
        self.listed_name = listed_name

    def __str__(self):
        return str(self.target_node)

class FlaggedLibdep:
    """
    Utility class used for processing prefix and postfix flags on libdeps. The class
    can keep track of separate lists for prefix and postfix as well separators,
    allowing for modifications to the lists and then re-application of the flags with
    modifications to a larger list representing the link line.
    """

    def __init__(self, libnode=None, env=None, start_index=None):
        """
        The libnode should be a Libdep SCons node, and the env is the target env in
        which the target has a dependency on the libdep. The start_index is important as
        it determines where this FlaggedLibdep starts in the larger list of libdeps.

        The start_index will cut the larger list, and then re-apply this libdep with flags
        at that location. This class will exract the prefix and postfix flags
        from the Libdep nodes env.
        """
        self.libnode = libnode
        self.env = env

        # We need to maintain our own copy so as not to disrupt the env's original list.
        try:
            self.prefix_flags = copy.copy(getattr(libnode.attributes, 'libdeps_prefix_flags', []))
            self.postfix_flags = copy.copy(getattr(libnode.attributes, 'libdeps_postfix_flags', []))
        except AttributeError:
            self.prefix_flags = []
            self.postfix_flags = []

        self.start_index = start_index

    def __str__(self):
        return str(self.libnode)

    def add_lib_to_result_list(self, result):
        """
        This function takes in the current list of libdeps for a given target, and will
        apply the libdep taking care of the prefix, postfix and any required separators when
        adding to the list.
        """
        if self.start_index != None:
            result[:] = result[:self.start_index]
        self._add_lib_and_flags(result)

    def _get_separators(self, flags):

        separated_list = []

        for flag in flags:
            separators = self.env.get('LIBDEPS_FLAG_SEPARATORS', {}).get(flag, {})
            separated_list.append(separators.get('prefix', ' '))
            separated_list.append(flag)
            separated_list.append(separators.get('suffix', ' '))

        return separated_list

    def _get_lib_with_flags(self):

        lib_and_flags = []

        lib_and_flags += self._get_separators(self.prefix_flags)
        lib_and_flags += [str(self)]
        lib_and_flags += self._get_separators(self.postfix_flags)

        return lib_and_flags

    def _add_lib_and_flags(self, result):
        """
        This function will clean up the flags for the link line after extracting everything
        from the environment. This will mostly look for separators that are just a space, and
        remove them from the list, as the final link line will add spaces back for each item
        in the list. It will take to concat flags where the separators don't allow for a space.
        """
        next_contig_str = ''

        for item in self._get_lib_with_flags():
            if item != ' ':
                next_contig_str += item
            else:
                if next_contig_str:
                    result.append(next_contig_str)
                next_contig_str = ''

        if next_contig_str:
            result.append(next_contig_str)



class LibdepLinter:
    """
    This class stores the rules for linting the libdeps. Using a decorator,
    new rules can easily be added to the class, and will be called when
    linting occurs. Each rule is run on each libdep.

    When a rule is broken, a LibdepLinterError exception will be raised.
    Optionally the class can be configured to print the error message and
    keep going with the build.

    Each rule should provide a method to skip that rule on a given node,
    by supplying the correct flag in the LIBDEPS_TAG environment var for
    that node.

    """

    skip_linting = False
    print_linter_errors = False

    linting_time = 0
    linting_infractions = 0
    linting_rules_run = 0
    registered_linting_time = False

    @staticmethod
    def _make_linter_decorator():
        """
        This is used for gathering the functions
        by decorator that will be used for linting a given libdep.
        """

        funcs = {}
        def linter_rule_func(func):
            funcs[func.__name__] = func
            return func

        linter_rule_func.all = funcs
        return linter_rule_func
    linter_rule = _make_linter_decorator.__func__()

    def __init__(self, env, target):
        self.env = env
        self.target = target
        self.unique_libs = set()
        self._libdeps_types_previous = dict()


        # If we are in print mode, we will record some linting metrics,
        # and print the results at the end of the build.
        if self.__class__.print_linter_errors and not self.__class__.registered_linting_time:
            import atexit
            def print_linting_time():
                print(f"Spent {self.__class__.linting_time} seconds linting libdeps.")
                print(f"Found {self.__class__.linting_infractions} issues out of {self.__class__.linting_rules_run} libdeps rules checked.")
            atexit.register(print_linting_time)
            self.__class__.registered_linting_time = True

    def lint_libdeps(self, libdeps):
        """
        Lint the given list of libdeps for all
        rules.
        """

        # Build performance optimization if you
        # are sure your build is clean.
        if self.__class__.skip_linting:
            return

        # Record time spent linting if we are in print mode.
        if self.__class__.print_linter_errors:
            from timeit import default_timer as timer
            start = timer()

        linter_rules = [
            getattr(self, linter_rule)
            for linter_rule in self.linter_rule.all
        ]

        for libdep in libdeps:
            for linter_rule in linter_rules:
                linter_rule(libdep)

        if self.__class__.print_linter_errors:
            self.__class__.linting_time += timer() - start
            self.__class__.linting_rules_run += (len(linter_rules)*len(libdeps))

    def _raise_libdep_lint_exception(self, message):
        """
        Raises the LibdepLinterError exception or if configure
        to do so, just prints the error.
        """
        prefix = "LibdepLinter: \n\t"
        message = prefix + message.replace('\n', '\n\t') + '\n'
        if self.__class__.print_linter_errors:
            self.__class__.linting_infractions += 1
            print(message)
        else:
            raise LibdepLinterError(message)

    def _check_for_lint_tags(self, lint_tag, env=None, inclusive_tag=False):
        """
        Used to get the lint tag from the environment,
        and if printing instead of raising exceptions,
        will ignore the tags.
        """

        # If print mode is on, we want to make sure to bypass checking
        # exclusive tags so we can make sure the exceptions are not excluded
        # and are printed. If it's an inclusive tag, we want to ignore this
        # early return completely, because we want to make sure the node
        # gets included for checking, and the exception gets printed.
        if not inclusive_tag and self.__class__.print_linter_errors:
            return False

        target_env = env if env else self.env

        if lint_tag in target_env.get(Constants.LibdepsTags, []):
            return True

    def _get_deps_dependents(self, env=None):
        """ util function to get all types of DEPS_DEPENDENTS"""
        target_env = env if env else self.env
        deps_dependents = target_env.get(Constants.LibdepsDependents, [])
        deps_dependents += target_env.get(Constants.ProgdepsDependents, [])
        return deps_dependents

    @linter_rule
    def linter_rule_leaf_node_no_deps(self, libdep):
        """
        LIBDEP RULE:
            Nodes marked explicitly as a leaf node should not have any dependencies,
            unless those dependencies are explicitly marked as allowed as leaf node
            dependencies.
        """
        if not self._check_for_lint_tags('lint-leaf-node-no-deps', inclusive_tag=True):
            return

        # Ignore dependencies that explicitly exempt themselves.
        if self._check_for_lint_tags('lint-leaf-node-allowed-dep', libdep.target_node.env):
            return

        target_type = self.target[0].builder.get_name(self.env)
        lib = os.path.basename(str(libdep))
        self._raise_libdep_lint_exception(
            textwrap.dedent(f"""\
                {target_type} '{self.target[0]}' has dependency '{lib}' and is marked explicitly as a leaf node,
                and '{lib}' does not exempt itself as an exception to the rule."""
            ))

    @linter_rule
    def linter_rule_no_public_deps(self, libdep):
        """
        LIBDEP RULE:
            Nodes explicitly marked as not allowed to have public dependencies, should not
            have public dependencies, unless the dependency is explicitly marked as allowed.
        """
        if not self._check_for_lint_tags('lint-no-public-deps', inclusive_tag=True):
            return

        if libdep.dependency_type != dependency.Private:
            # Check if the libdep exempts itself from this rule.
            if self._check_for_lint_tags('lint-public-dep-allowed', libdep.target_node.env):
                return

            target_type = self.target[0].builder.get_name(self.env)
            lib = os.path.basename(str(libdep))
            self._raise_libdep_lint_exception(
                textwrap.dedent(f"""\
                    {target_type} '{self.target[0]}' has public dependency '{lib}'
                    while being marked as not allowed to have public dependencies
                    and '{lib}' does not exempt itself."""
                ))

    @linter_rule
    def linter_rule_no_dups(self, libdep):
        """
        LIBDEP RULE:
            A given node shall not link the same LIBDEP across public, private
            or interface dependency types because it is ambiguous and unnecessary.
        """
        if self._check_for_lint_tags('lint-allow-dup-libdeps'):
            return

        if str(libdep) in self.unique_libs:
            target_type = self.target[0].builder.get_name(self.env)
            lib = os.path.basename(str(libdep))
            self._raise_libdep_lint_exception(
                f"{target_type} '{self.target[0]}' links '{lib}' multiple times."
            )

        self.unique_libs.add(str(libdep))

    @linter_rule
    def linter_rule_alphabetic_deps(self, libdep):
        """
        LIBDEP RULE:
            Libdeps shall be listed alphabetically by type in the SCons files.
        """

        if self._check_for_lint_tags('lint-allow-non-alphabetic'):
            return

        # Start checking order after the first item in the list is recorded to compare with.
        if libdep.dependency_type in self._libdeps_types_previous:
            if self._libdeps_types_previous[libdep.dependency_type] > libdep.listed_name:
                target_type = self.target[0].builder.get_name(self.env)
                self._raise_libdep_lint_exception(
                    f"{target_type} '{self.target[0]}' has '{libdep.listed_name}' listed in {dep_type_to_env_var[libdep.dependency_type]} out of alphabetical order."
                )

        self._libdeps_types_previous[libdep.dependency_type] = libdep.listed_name

    @linter_rule
    def linter_rule_programs_link_private(self, libdep):
        """
        LIBDEP RULE:
            All Programs shall only have public dependency's
            because a Program will never be a dependency of another Program
            or Library, and LIBDEPS transitiveness does not apply. Public
            transitiveness has no meaning in this case and is used just as default.
        """
        if self._check_for_lint_tags('lint-allow-program-links-private'):
            return

        if (self.target[0].builder.get_name(self.env) == "Program"
            and libdep.dependency_type != dependency.Public):

            lib = os.path.basename(str(libdep))
            self._raise_libdep_lint_exception(
                textwrap.dedent(f"""\
                    Program '{self.target[0]}' links non-public library '{lib}'
                    A 'Program' can only have {Constants.Libdeps} libs,
                    not {Constants.LibdepsPrivate} or {Constants.LibdepsInterface}."""
                ))

    @linter_rule
    def linter_rule_no_bidirectional_deps(self, libdep):
        """
        LIBDEP RULE:
            And Library which issues reverse dependencies, shall not be directly
            linked to by another node, to prevent forward and reverse linkages existing
            at the same node. Instead the content of the library that needs to issue reverse
            dependency needs to be separated from content that needs direct linkage into two
            separate libraries, which can be linked correctly respectively.
        """

        if not libdep.target_node.env:
            return
        elif self._check_for_lint_tags('lint-allow-bidirectional-edges', libdep.target_node.env):
            return
        elif len(self._get_deps_dependents(libdep.target_node.env)) > 0:

                target_type = self.target[0].builder.get_name(self.env)
                lib = os.path.basename(str(libdep))
                self._raise_libdep_lint_exception(textwrap.dedent(f"""\
                    {target_type} '{self.target[0]}' links directly to a reverse dependency node '{lib}'
                    No node can link directly to a node that has {Constants.LibdepsDependents} or {Constants.ProgdepsDependents}."""
                ))

    @linter_rule
    def linter_rule_nonprivate_on_deps_dependents(self, libdep):
        """
        LIBDEP RULE:
            A Library that issues reverse dependencies, shall not link libraries
            with any kind of transitiveness, and will only link libraries privately.
            This is because functionality that requires reverse dependencies should
            not be transitive.
        """
        if self._check_for_lint_tags('lint-allow-nonprivate-on-deps-dependents'):
            return

        if (libdep.dependency_type != dependency.Private
            and len(self._get_deps_dependents()) > 0):

            target_type = self.target[0].builder.get_name(self.env)
            lib = os.path.basename(str(libdep))
            self._raise_libdep_lint_exception(textwrap.dedent(f"""\
                {target_type} '{self.target[0]}' links non-private libdep '{lib}' and has a reverse dependency.
                A {target_type} can only have {Constants.LibdepsPrivate} depends if it has {Constants.LibdepsDependents} or {Constants.ProgdepsDependents}."""
            ))

    @linter_rule
    def linter_rule_libdeps_must_be_list(self, libdep):
        """
        LIBDEP RULE:
            LIBDEPS, LIBDEPS_PRIVATE, and LIBDEPS_INTERFACE must be set as lists in the
            environment.
        """
        if self._check_for_lint_tags('lint-allow-nonlist-libdeps'):
            return

        libdeps_vars = list(dep_type_to_env_var.values()) + [
            Constants.LibdepsDependents,
            Constants.ProgdepsDependents]

        for dep_type_val in libdeps_vars:

            libdeps_list = self.env.get(dep_type_val, [])
            if not SCons.Util.is_List(libdeps_list):

                target_type = self.target[0].builder.get_name(self.env)
                self._raise_libdep_lint_exception(textwrap.dedent(f"""\
                    Found non-list type '{libdeps_list}' while evaluating {dep_type_val} for {target_type} '{self.target[0]}'
                    {dep_type_val} must be setup as a list."""
                ))

dependency_visibility_ignored = {
    dependency.Public: dependency.Public,
    dependency.Private: dependency.Public,
    dependency.Interface: dependency.Public,
}

dependency_visibility_honored = {
    dependency.Public: dependency.Public,
    dependency.Private: dependency.Private,
    dependency.Interface: dependency.Interface,
}

dep_type_to_env_var = {
    dependency.Public: Constants.Libdeps,
    dependency.Private: Constants.LibdepsPrivate,
    dependency.Interface: Constants.LibdepsInterface,
}

class DependencyCycleError(SCons.Errors.UserError):
    """Exception representing a cycle discovered in library dependencies."""

    def __init__(self, first_node):
        super(DependencyCycleError, self).__init__()
        self.cycle_nodes = [first_node]

    def __str__(self):
        return "Library dependency cycle detected: " + " => ".join(
            str(n) for n in self.cycle_nodes
        )

class LibdepLinterError(SCons.Errors.UserError):
    """Exception representing a discongruent usages of libdeps"""

class MissingSyslibdepError(SCons.Errors.UserError):
    """Exception representing a discongruent usages of libdeps"""

def __get_sorted_direct_libdeps(node):
    direct_sorted = getattr(node.attributes, "libdeps_direct_sorted", False)
    if not direct_sorted:
        direct = getattr(node.attributes, "libdeps_direct", [])
        direct_sorted = sorted(direct, key=lambda t: str(t.target_node))
        setattr(node.attributes, "libdeps_direct_sorted", direct_sorted)
    return direct_sorted


def __libdeps_visit(n, marked, tsorted, walking):
    if n.target_node in marked:
        return

    if n.target_node in walking:
        raise DependencyCycleError(n.target_node)

    walking.add(n.target_node)

    try:
        for child in __get_sorted_direct_libdeps(n.target_node):
            if child.dependency_type != dependency.Private:
                __libdeps_visit(child, marked, tsorted, walking=walking)

        marked.add(n.target_node)
        tsorted.append(n.target_node)

    except DependencyCycleError as e:
        if len(e.cycle_nodes) == 1 or e.cycle_nodes[0] != e.cycle_nodes[-1]:
            e.cycle_nodes.insert(0, n.target_node)
        raise


def __get_libdeps(node):
    """Given a SCons Node, return its library dependencies, topologically sorted.

    Computes the dependencies if they're not already cached.
    """

    cache = getattr(node.attributes, Constants.LibdepsCached, None)
    if cache is not None:
        return cache

    tsorted = []
    marked = set()
    walking = set()

    for child in __get_sorted_direct_libdeps(node):
        if child.dependency_type != dependency.Interface:
            __libdeps_visit(child, marked, tsorted, walking)

    tsorted.reverse()
    setattr(node.attributes, Constants.LibdepsCached, tsorted)

    return tsorted


def __missing_syslib(name):
    return Constants.MissingLibdep + name


def update_scanner(builder):
    """Update the scanner for "builder" to also scan library dependencies."""

    old_scanner = builder.target_scanner

    if old_scanner:
        path_function = old_scanner.path_function

        def new_scanner(node, env, path=()):
            result = old_scanner.function(node, env, path)
            result.extend(__get_libdeps(node))
            return result

    else:
        path_function = None

        def new_scanner(node, env, path=()):
            return __get_libdeps(node)

    builder.target_scanner = SCons.Scanner.Scanner(
        function=new_scanner, path_function=path_function
    )


def get_libdeps(source, target, env, for_signature):
    """Implementation of the special _LIBDEPS environment variable.

    Expands to the library dependencies for a target.
    """

    target = env.Flatten([target])
    return __get_libdeps(target[0])


def get_libdeps_objs(source, target, env, for_signature):
    objs = []
    for lib in get_libdeps(source, target, env, for_signature):
        # This relies on Node.sources being order stable build-to-build.
        objs.extend(lib.sources)
    return objs

def make_get_syslibdeps_callable(shared):

    def get_syslibdeps(source, target, env, for_signature):
        """ Given a SCons Node, return its system library dependencies.

        These are the dependencies listed with SYSLIBDEPS, and are linked using -l.
        """

        deps = getattr(target[0].attributes, Constants.SysLibdepsCached, None)
        if deps is None:

            # Get the sys libdeps for the current node
            deps = target[0].get_env().Flatten(target[0].get_env().get(Constants.SysLibdepsPrivate) or [])
            deps += target[0].get_env().Flatten(target[0].get_env().get(Constants.SysLibdeps) or [])

            for lib in __get_libdeps(target[0]):

                # For each libdep get its syslibdeps, and then check to see if we can
                # add it to the deps list. For static build we will also include private
                # syslibdeps to be transitive. For a dynamic build we will only make
                # public libdeps transitive.
                syslibs = []
                if not shared:
                    syslibs += lib.get_env().get(Constants.SysLibdepsPrivate) or []
                syslibs += lib.get_env().get(Constants.SysLibdeps) or []

                # Validate the libdeps, a configure check has already checked what
                # syslibdeps are available so we can hard fail here if a syslibdep
                # is being attempted to be linked with.
                for syslib in syslibs:
                    if not syslib:
                        continue

                    if isinstance(syslib, str) and syslib.startswith(Constants.MissingLibdep):
                        MissingSyslibdepError(textwrap.dedent(f"""\
                            Target '{str(target[0])}' depends on the availability of a
                            system provided library for '{syslib[len(Constants.MissingLibdep):]}',
                            but no suitable library was found during configuration."""
                        ))

                    deps.append(syslib)

            setattr(target[0].attributes, Constants.SysLibdepsCached, deps)

        lib_link_prefix = env.subst("$LIBLINKPREFIX")
        lib_link_suffix = env.subst("$LIBLINKSUFFIX")
        # Elements of syslibdeps are either strings (str or unicode), or they're File objects.
        # If they're File objects, they can be passed straight through.  If they're strings,
        # they're believed to represent library short names, that should be prefixed with -l
        # or the compiler-specific equivalent.  I.e., 'm' becomes '-lm', but 'File("m.a") is passed
        # through whole cloth.
        return [f"{lib_link_prefix}{d}{lib_link_suffix}" if isinstance(d, str) else d for d in deps]

    return get_syslibdeps

def __append_direct_libdeps(node, prereq_nodes):
    # We do not bother to decorate nodes that are not actual Objects
    if type(node) == str:
        return
    if getattr(node.attributes, "libdeps_direct", None) is None:
        node.attributes.libdeps_direct = []
    node.attributes.libdeps_direct.extend(prereq_nodes)


def __get_flagged_libdeps(source, target, env, for_signature):
    for lib in get_libdeps(source, target, env, for_signature):
        # Make sure lib is a Node so we can get the env to check for flags.
        libnode = lib
        if not isinstance(lib, (str, SCons.Node.FS.File, SCons.Node.FS.Entry)):
            libnode = env.File(lib)

        # Create a libdep and parse the prefix and postfix (and separators if any)
        # flags from the environment.
        cur_lib = FlaggedLibdep(libnode, env)
        yield cur_lib


def __get_node_with_ixes(env, node, node_builder_type):
    """
    Gets the node passed in node with the correct ixes applied
    for the given builder type.
    """

    if not node:
        return node

    node_builder = env["BUILDERS"][node_builder_type]
    node_factory = node_builder.target_factory or env.File

    # Cache the 'ixes' in a function scope global so we don't need
    # to run SCons performance intensive 'subst' each time
    cache_key = (id(env), node_builder_type)
    try:
        prefix, suffix = __get_node_with_ixes.node_type_ixes[cache_key]
    except KeyError:
        prefix = node_builder.get_prefix(env)
        suffix = node_builder.get_suffix(env)

        # TODO(SERVER-50681): Find a way to do this that doesn't hard
        # code these extensions. See the code review for SERVER-27507
        # for additional discussion.
        if suffix == ".dll":
            suffix = ".lib"

        __get_node_with_ixes.node_type_ixes[cache_key] = (prefix, suffix)

    node_with_ixes = SCons.Util.adjustixes(node, prefix, suffix)
    return node_factory(node_with_ixes)

__get_node_with_ixes.node_type_ixes = dict()

def make_libdeps_emitter(
    dependency_builder,
    dependency_map=dependency_visibility_ignored,
    ignore_progdeps=False,
):
    def libdeps_emitter(target, source, env):
        """SCons emitter that takes values from the LIBDEPS environment variable and
        converts them to File node objects, binding correct path information into
        those File objects.

        Emitters run on a particular "target" node during the initial execution of
        the SConscript file, rather than during the later build phase.  When they
        run, the "env" environment's working directory information is what you
        expect it to be -- that is, the working directory is considered to be the
        one that contains the SConscript file.  This allows specification of
        relative paths to LIBDEPS elements.

        This emitter also adds LIBSUFFIX and LIBPREFIX appropriately.

        NOTE: For purposes of LIBDEPS_DEPENDENTS propagation, only the first member
        of the "target" list is made a prerequisite of the elements of LIBDEPS_DEPENDENTS.
        """

        # Get all the libdeps from the env so we can
        # can append them to the current target_node.
        libdeps = []
        for dep_type in sorted(dependency_map.keys()):

            # Libraries may not be stored as a list in the env,
            # so we must convert single library strings to a list.
            libs = env.get(dep_type_to_env_var[dep_type])
            if not SCons.Util.is_List(libs):
                libs = [libs]

            for lib in libs:
                if not lib:
                    continue
                lib_with_ixes = __get_node_with_ixes(env, lib, dependency_builder)
                libdeps.append(dependency(lib_with_ixes, dep_type, lib))

        # Lint the libdeps to make sure they are following the rules.
        # This will skip some or all of the checks depending on the options
        # and LIBDEPS_TAGS used.
        if not any("conftest" in str(t) for t in target):
            LibdepLinter(env, target).lint_libdeps(libdeps)

        # We ignored the dependency_map until now because we needed to use
        # original dependency value for linting. Now go back through and
        # use the map to convert to the desired dependencies, for example
        # all Public in the static linking case.
        for libdep in libdeps:
            libdep.dependency_type = dependency_map[libdep.dependency_type]

        for t in target:
            # target[0] must be a Node and not a string, or else libdeps will fail to
            # work properly.
            __append_direct_libdeps(t, libdeps)

        for dependent in env.get(Constants.LibdepsDependents, []):
            if dependent is None:
                continue

            visibility = dependency.Private
            if isinstance(dependent, tuple):
                visibility = dependent[1]
                dependent = dependent[0]

            dependentNode = __get_node_with_ixes(
                env, dependent, dependency_builder
            )
            __append_direct_libdeps(
                dependentNode, [dependency(target[0], dependency_map[visibility], dependent)]
            )

        if not ignore_progdeps:
            for dependent in env.get(Constants.ProgdepsDependents, []):
                if dependent is None:
                    continue

                visibility = dependency.Public
                if isinstance(dependent, tuple):
                    # TODO: Error here? Non-public PROGDEPS_DEPENDENTS probably are meaningless
                    visibility = dependent[1]
                    dependent = dependent[0]

                dependentNode = __get_node_with_ixes(
                    env, dependent, "Program"
                )
                __append_direct_libdeps(
                    dependentNode, [dependency(target[0], dependency_map[visibility], dependent)]
                )

        return target, source

    return libdeps_emitter


def expand_libdeps_tags(source, target, env, for_signature):
    results = []
    for expansion in env.get(Constants.LibdepsTagExpansion, []):
        results.append(expansion(source, target, env, for_signature))
    return results


def expand_libdeps_with_flags(source, target, env, for_signature):

    libdeps_with_flags = []

    # Used to make modifications to the previous libdep on the link line
    # if needed. An empty class here will make the switch_flag conditionals
    # below a bit cleaner.
    prev_libdep = None

    for flagged_libdep in __get_flagged_libdeps(source, target, env, for_signature):

        # If there are no flags to process we can move on to the next lib.
        # start_index wont mater in the case because if there are no flags
        # on the previous lib, then we will never need to do the chopping
        # mechanism on the next iteration.
        if not flagged_libdep.prefix_flags and not flagged_libdep.postfix_flags:
            libdeps_with_flags.append(str(flagged_libdep))
            prev_libdep = flagged_libdep
            continue

        # This for loop will go through the previous results and remove the 'off'
        # flag as well as removing the new 'on' flag. For example, let libA and libB
        # both use on and off flags which would normally generate on the link line as:
        #   -Wl--on-flag libA.a -Wl--off-flag -Wl--on-flag libA.a -Wl--off-flag
        # This loop below will spot the cases were the flag was turned off and then
        # immediately turned back on
        for switch_flag in getattr(flagged_libdep.libnode.attributes, 'libdeps_switch_flags', []):
            if (prev_libdep and switch_flag['on'] in flagged_libdep.prefix_flags
                and switch_flag['off'] in prev_libdep.postfix_flags):

                flagged_libdep.prefix_flags.remove(switch_flag['on'])
                prev_libdep.postfix_flags.remove(switch_flag['off'])

                # prev_lib has had its list modified, and it has a start index
                # from the last iteration, so it will chop of the end the current
                # list and reapply the end with the new flags.
                prev_libdep.add_lib_to_result_list(libdeps_with_flags)

        # Store the information of the len of the current list before adding
        # the next set of flags as that will be the start index for the previous
        # lib next time around in case there are any switch flags to chop off.
        start_index = len(libdeps_with_flags)
        flagged_libdep.add_lib_to_result_list(libdeps_with_flags)

        # Done processing the current lib, so set it to previous for the next iteration.
        prev_libdep = flagged_libdep
        prev_libdep.start_index = start_index

    return libdeps_with_flags


def setup_environment(env, emitting_shared=False, linting='on'):
    """Set up the given build environment to do LIBDEPS tracking."""

    LibdepLinter.skip_linting = linting == 'off'
    LibdepLinter.print_linter_errors = linting == 'print'

    try:
        env["_LIBDEPS"]
    except KeyError:
        env["_LIBDEPS"] = "$_LIBDEPS_LIBS"

    env["_LIBDEPS_TAGS"] = expand_libdeps_tags
    env["_LIBDEPS_GET_LIBS"] = get_libdeps
    env["_LIBDEPS_OBJS"] = get_libdeps_objs
    env["_SYSLIBDEPS"] = make_get_syslibdeps_callable(emitting_shared)

    env[Constants.Libdeps] = SCons.Util.CLVar()
    env[Constants.SysLibdeps] = SCons.Util.CLVar()

    # We need a way for environments to alter just which libdeps
    # emitter they want, without altering the overall program or
    # library emitter which may have important effects. The
    # substitution rules for emitters are a little strange, so build
    # ourselves a little trampoline to use below so we don't have to
    # deal with it.
    def make_indirect_emitter(variable):
        def indirect_emitter(target, source, env):
            return env[variable](target, source, env)

        return indirect_emitter

    env.Append(
        LIBDEPS_LIBEMITTER=make_libdeps_emitter("StaticLibrary"),
        LIBEMITTER=make_indirect_emitter("LIBDEPS_LIBEMITTER"),
        LIBDEPS_SHAREMITTER=make_libdeps_emitter("SharedArchive", ignore_progdeps=True),
        SHAREMITTER=make_indirect_emitter("LIBDEPS_SHAREMITTER"),
        LIBDEPS_SHLIBEMITTER=make_libdeps_emitter(
            "SharedLibrary", dependency_visibility_honored
        ),
        SHLIBEMITTER=make_indirect_emitter("LIBDEPS_SHLIBEMITTER"),
        LIBDEPS_PROGEMITTER=make_libdeps_emitter(
            "SharedLibrary" if emitting_shared else "StaticLibrary"
        ),
        PROGEMITTER=make_indirect_emitter("LIBDEPS_PROGEMITTER"),
    )

    env["_LIBDEPS_LIBS_WITH_TAGS"] = expand_libdeps_with_flags

    env["_LIBDEPS_LIBS"] = (
        "$LINK_LIBGROUP_START "
        "$_LIBDEPS_LIBS_WITH_TAGS "
        "$LINK_LIBGROUP_END "
    )

    env.Prepend(_LIBFLAGS="$_LIBDEPS_TAGS $_LIBDEPS $_SYSLIBDEPS ")
    for builder_name in ("Program", "SharedLibrary", "LoadableModule", "SharedArchive"):
        try:
            update_scanner(env["BUILDERS"][builder_name])
        except KeyError:
            pass


def setup_conftests(conf):
    def FindSysLibDep(context, name, libs, **kwargs):
        var = "LIBDEPS_" + name.upper() + "_SYSLIBDEP"
        kwargs["autoadd"] = False
        for lib in libs:
            result = context.sconf.CheckLib(lib, **kwargs)
            context.did_show_result = 1
            if result:
                context.env[var] = lib
                return context.Result(result)
        context.env[var] = __missing_syslib(name)
        return context.Result(result)

    conf.AddTest("FindSysLibDep", FindSysLibDep)
