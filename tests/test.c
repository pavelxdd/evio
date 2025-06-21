#include "test.h"

#include <getopt.h>

enum {
    OPT_START = 0xff,
    OPT_OUTPUT,
    OPT_TEST,
    OPT_SKIP,
    OPT_LIST,
    OPT_FILE,
    OPT_HELP = 'h',
};

static const struct option options[] = {
    { "output",     2, 0, OPT_OUTPUT },
    { "test",       2, 0, OPT_TEST },
    { "skip",       2, 0, OPT_SKIP },
    { "list",       0, 0, OPT_LIST },
    { "file",       0, 0, OPT_FILE },
    { "help",       0, 0, OPT_HELP },
    {}
};

// GCOVR_EXCL_START
static void tests_usage(void)
{
    FILE *f = stdout;
    flockfile(f);

#define OUT(...) fprintf(f, __VA_ARGS__)
#define ARG(name, fmt, ...) OUT("  %-32s " fmt "\n", name, ##__VA_ARGS__)

    OUT("Usage: %s [OPTION]...\n", program_invocation_short_name);

    OUT("\nOptions:\n");
    {
        ARG("--output=FORMAT",
            "Tests output format (standard, stdout, subunit, tap, xml)");

        ARG("--test=PATTERN",
            "Run tests matching the PATTERN");

        ARG("--skip=PATTERN",
            "Skip tests matching the PATTERN");

        ARG("--list",
            "Print all unit test names and exit");

        ARG("--file",
            "Print source location when listing test names");

        ARG("--help, -h",
            "Print this help and exit");
    }

#undef ARG
#undef OUT

    fflush_unlocked(f);
    funlockfile(f);
}
// GCOVR_EXCL_STOP

int main(int argc, char *argv[])
{
    bool list = false;
    bool file = false;

    // GCOVR_EXCL_START
    for (opterr = optind = 0;;) {
        int arg = optind;
        int opt = getopt_long(argc, argv, "+h", options, NULL);
        if (opt < 0) {
            break;
        }

        switch (opt) {
            case OPT_OUTPUT: {
                if (optarg) {
                    setenv("CMOCKA_MESSAGE_OUTPUT", optarg, true);
                } else {
                    unsetenv("CMOCKA_MESSAGE_OUTPUT");
                }
                continue;
            }
            case OPT_TEST: {
                cmocka_set_test_filter(optarg);
                continue;
            }
            case OPT_SKIP: {
                cmocka_set_skip_filter(optarg);
                continue;
            }
            case OPT_LIST: {
                list = true;
                continue;
            }
            case OPT_FILE: {
                file = true;
                continue;
            }
            case OPT_HELP: {
                tests_usage();
                return EXIT_SUCCESS;
            }
            default:
                break;
        }

        fprintf(stderr, "Invalid argument: %s\n", argv[arg ? arg : 1]);
        fprintf(stderr, "Try '--help' for more information\n");
        return EXIT_FAILURE;
    }
    // GCOVR_EXCL_STOP

    extern const struct CMUnitTest __start_unit_test_section;
    extern const struct CMUnitTest __stop_unit_test_section;

    // GCOVR_EXCL_START
    if (list) {
        const struct CMUnitTest *test;
        for (test = &__start_unit_test_section;
             test < &__stop_unit_test_section; ++test) {
            if (file) {
                fprintf(stdout, "%-40s %s\n",
                        test->name + strlen(test->name) + 1,
                        test->name);
            } else {
                fprintf(stdout, "%s\n", test->name);
            }
        }
        return EXIT_SUCCESS;
    }
    // GCOVR_EXCL_STOP

    const struct CMUnitTest *const tests = &__start_unit_test_section;
    const size_t tests_count = ((uintptr_t)&__stop_unit_test_section -
                                (uintptr_t)&__start_unit_test_section) / sizeof(struct CMUnitTest);
    return _cmocka_run_group_tests(program_invocation_short_name,
                                   tests, tests_count, NULL, NULL);
}

TEST(test_cmocka)
{
    assert_non_null(state);
    assert_null(*state);
    assert_true(true);
}

TEST(test_cmocka_initial_state, .initial_state = (void *)"")
{
    assert_non_null(state);
    assert_ptr_equal(*state, "");
}

static int test_teardown(void **state)
{
    assert_non_null(state);
    assert_null(*state);
    return 0;
}

static int test_setup(void **state)
{
    assert_int_equal(0, test_teardown(state));
    *state = (void *)"";
    return 0;
}

TEST(test_cmocka_setup_teardown,
     .setup_func    = test_setup,
     .teardown_func = test_teardown)
{
    test_cmocka_initial_state(state);
    *state = NULL;
}

TEST(test_assert)
{
    expect_assert_failure({
        EVIO_ASSERT(false);
    });
}
