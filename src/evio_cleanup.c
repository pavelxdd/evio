#include "evio_core.h"
#include "evio_cleanup.h"

void evio_cleanup_start(evio_loop *loop, evio_cleanup *w)
{
    evio_list_start(loop, &w->base, &loop->cleanup, false);
}

void evio_cleanup_stop(evio_loop *loop, evio_cleanup *w)
{
    evio_list_stop(loop, &w->base, &loop->cleanup, false);
}
