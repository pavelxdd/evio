#include "test.h"

TEST(test_version)
{
    assert_int_equal(evio_version_major(), EVIO_VERSION_MAJOR);
    assert_int_equal(evio_version_minor(), EVIO_VERSION_MINOR);
    assert_int_equal(evio_version_patch(), EVIO_VERSION_PATCH);
}
