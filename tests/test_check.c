#include "test.h"

TEST(test_evio_check)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_check check;
    evio_check_init(&check, generic_cb);
    evio_check_start(loop, &check);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb_emask, EVIO_CHECK);

    evio_check_stop(loop, &check);
    evio_loop_free(loop);
}
