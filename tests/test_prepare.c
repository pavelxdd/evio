#include "test.h"

TEST(test_evio_prepare)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, generic_cb);
    evio_prepare_start(loop, &prepare);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb_emask, EVIO_PREPARE);

    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}
