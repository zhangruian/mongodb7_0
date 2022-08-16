# About
This directory contains all the logic related to query optimization in the new
common query framework. It contains the models for representing a query and the
logic for implementing optimization via a cascades framework.

# Testing
Developers working on the new optimizer may wish to run a subset of the tests
which is exclusively focused on this codebase. This section details the relevant
tests. 
## Unit Tests
The following C++ unit tests exercise relevant parts of the codebase:

- algebra_test (src/mongo/db/query/optimizer/algebra/)
- db_pipeline_test (src/mongo/db/pipeline/)
- - This test suite includes many unrelated test cases, but
    'abt/pipeline_test.cpp' is the relevant one.
- optimizer_test (src/mongo/db/query/optimizer/)
- sbe_abt_test (src/mongo/db/exec/sbe/abt/)

These can be compiled with targets like 'build/install/bin/algebra_test',
although the exact name will depend on your 'installDir' which you have
configured with SCons. It may look more like
'build/opt/install/bin/algebra_test'. If you want to build and run at once, you
can use the '+' shortcut to ninja, like so:
```
ninja <FLAGS> +algebra_test +db_pipeline_test +optimizer_test +sbe_abt_test
```

## JS Integration Tests
In addition to the above unit tests, the following JS suites are helpful in
exercising this codebase:
- **cqf**: [buildscripts/resmokeconfig/suites/cqf.yml](/buildscripts/resmokeconfig/suites/cqf.yml)
- **cqf_disabled_pipeline_opt**:
    [buildscripts/resmokeconfig/suites/cqf_disabled_pipeline_opt.yml](/buildscripts/resmokeconfig/suites/cqf_disabled_pipeline_opt.yml)
- **cqf_parallel**: [buildscripts/resmokeconfig/suites/cqf_parallel.yml](/buildscripts/resmokeconfig/suites/cqf_parallel.yml)
- **cqf_passthrough**: [buildscripts/resmokeconfig/suites/cqf_passthrough.yml](/buildscripts/resmokeconfig/suites/cqf_passthrough.yml)

Desriptions of these suites can be found in
[buildscripts/resmokeconfig/evg_task_doc/evg_task_doc.yml](/buildscripts/resmokeconfig/evg_task_doc/evg_task_doc.yml).

You may run these like so, adjusting the `-j` flag for the appropriate level of
parallel execution for your machine.
```
./buildscripts/resmoke.py run -j4 \
 --suites=cqf,cqf_disabled_pipeline_opt,cqf_parallel,cqf_passthrough
```

cqf_passthrough takes the longest to run by far, so this command may be more
useful for a quicker signal:
```
./buildscripts/resmoke.py run --suites=cqf,cqf_disabled_pipeline_opt,cqf_parallel -j4
```

## Local Testing Recommendation
Something like this command may be helpful for local testing:
```
ninja <FLAGS> install-devcore build/install/bin/algebra_test \
build/install/bin/db_pipeline_test build/install/bin/optimizer_test \
build/install/bin/sbe_abt_test \
&& ./build/install/bin/algebra_test \
&& ./build/install/bin/db_pipeline_test --fileNameFilter=pipeline_test.cpp \
&& ./build/install/bin/optimizer_test \
&& ./build/install/bin/sbe_abt_test \
&& ./buildscripts/resmoke.py run --suites=cqf,cqf_parallel,cqf_disabled_pipeline_opt -j4
```
**Note:** You may need to adjust the path to the unit test binary targets if your
SCons install directory is something more like `build/opt/install/bin`.

**Note:** The 'cqf_passthrough' suite may also be useful for additional coverage,
but due to the number of tests it takes quite a while to run locally
(~15 minutes depending on parallelization and compile flags)
