"""Pseudo-builders for building and registering libfuzzer tests.
"""
from SCons.Script import Action


def exists(env):
    return True


_libfuzzer_tests = []


def register_libfuzzer_test(env, test):
    _libfuzzer_tests.append(test.path)
    env.Alias('$LIBFUZZER_TEST_ALIAS', test)


def libfuzzer_test_list_builder_action(env, target, source):
    with open(str(target[0]), 'w') as ofile:
        for s in _libfuzzer_tests:
            print('\t' + str(s))
            ofile.write('%s\n' % s)



def build_cpp_libfuzzer_test(env, target, source, **kwargs):
    myenv = env.Clone()

    libdeps = kwargs.get('LIBDEPS', [])
    kwargs['LIBDEPS'] = libdeps
    kwargs['INSTALL_ALIAS'] = ['tests']
    sanitizer_option = '-fsanitize=fuzzer'
    myenv.Prepend(LINKFLAGS=[sanitizer_option])

    result = myenv.Program(target, source, **kwargs)
    myenv.RegisterLibfuzzerTest(result[0])
    hygienic = myenv.GetOption('install-mode') == 'hygienic'
    if not hygienic:
        myenv.Install("#/build/libfuzzer_tests/", result[0])
    return result


def generate(env):
    env.Command('$LIBFUZZER_TEST_LIST', env.Value(_libfuzzer_tests),
                Action(libfuzzer_test_list_builder_action, "Generating $TARGET"))
    env.AddMethod(register_libfuzzer_test, 'RegisterLibfuzzerTest')
    env.AddMethod(build_cpp_libfuzzer_test, 'CppLibfuzzerTest')
    env.Alias('$LIBFUZZER_TEST_ALIAS', '$LIBFUZZER_TEST_LIST')
