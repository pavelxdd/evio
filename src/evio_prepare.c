#include "evio_core.h"
#include "evio_prepare.h"

void evio_prepare_start(evio_loop *loop, evio_prepare *w)
{
    evio_list_start(loop, &w->base, &loop->prepare, true);
}

void evio_prepare_stop(evio_loop *loop, evio_prepare *w)
{
    evio_list_stop(loop, &w->base, &loop->prepare, true);
}
