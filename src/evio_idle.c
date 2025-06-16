#include "evio_core.h"
#include "evio_idle.h"

void evio_idle_start(evio_loop *loop, evio_idle *w)
{
    evio_list_start(loop, &w->base, &loop->idle, true);
}

void evio_idle_stop(evio_loop *loop, evio_idle *w)
{
    evio_list_stop(loop, &w->base, &loop->idle, true);
}
