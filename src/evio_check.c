#include "evio_core.h"
#include "evio_check.h"

void evio_check_start(evio_loop *loop, evio_check *w)
{
    evio_list_start(loop, &w->base, &loop->check, true);
}

void evio_check_stop(evio_loop *loop, evio_check *w)
{
    evio_list_stop(loop, &w->base, &loop->check, true);
}
