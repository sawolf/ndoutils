
/* if you want to see the expanded source, you can either:

    `make expanded`

    or:

    `gcc -E src/ndo-startup-queue.c | clang-format`
*/


int ndo_empty_startup_queues()
{
    ndo_empty_queue_timed_event();
}


/* le sigh... this just makes it way easier */
#define EMTPY_QUEUE_FUNCTION(_type, _callback) \
int ndo_empty_## _type() \
{ \
    trace_func_void(); \
\
    nebstruct_## _type ##_data * data = NULL; \
    int type = 0; \
\
    /* first we deregister our queue callback and make sure that the */ \
    /* data goes straight into the database so that our queue doesn't get */ \
    /* backed up */ \
    neb_deregister_callback(_callback, ndo_handle_queue_## _type); \
    neb_register_callback(_callback, ndo_handle, 0, ndo_handle_## _type); \
\
    while (TRUE) { \
\
        /* we probably don't need the lock and unlock because of the unreg/reg */ \
        /* prior to here, but i'm not willing to take that chance without */ \
        /* testing */ \
        pthread_mutex_lock(&queue_## _type ##_mutex); \
        data = dequeue(&nebstruct_queue_## _type, &type); \
        pthread_mutex_unlock(&queue_## _type ##_mutex); \
\
        /* the queue is empty */ \
        if (data == NULL || type == -1) { \
            break; \
        } \
\
        ndo_handle_timed_event(type, data); \
        free(data); \
        data = NULL; \
    } \
\
    trace_return_ok(); \
}


EMTPY_QUEUE_FUNCTION(timed_event, NEBCALLBACK_TIMED_EVENT_DATA)


EMTPY_QUEUE_FUNCTION(event_handler, NEBCALLBACK_EVENT_HANDLER_DATA)


EMTPY_QUEUE_FUNCTION(host_check, NEBCALLBACK_HOST_CHECK_DATA)


EMTPY_QUEUE_FUNCTION(service_check, NEBCALLBACK_SERVICE_CHECK_DATA)


EMTPY_QUEUE_FUNCTION(comment, NEBCALLBACK_COMMENT_DATA)


EMTPY_QUEUE_FUNCTION(downtime, NEBCALLBACK_DOWNTIME_DATA)


EMTPY_QUEUE_FUNCTION(flapping, NEBCALLBACK_FLAPPING_DATA)


EMTPY_QUEUE_FUNCTION(host_status, NEBCALLBACK_HOST_STATUS_DATA)


EMTPY_QUEUE_FUNCTION(service_status, NEBCALLBACK_SERVICE_STATUS_DATA)


EMTPY_QUEUE_FUNCTION(contact_status, NEBCALLBACK_CONTACT_STATUS_DATA)


EMTPY_QUEUE_FUNCTION(acknowledgement, NEBCALLBACK_ACKNOWLEDGEMENT_DATA)


EMTPY_QUEUE_FUNCTION(state_change, NEBCALLBACK_STATE_CHANGE_DATA)
