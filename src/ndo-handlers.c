
// todo: all of the mysql bindings need to be gone through and compared against the schema to ensure doubles aren't being cast as ints (won't error, but the data won't be right either)

int ndo_handle_process(int type, void * d)
{
    nebstruct_process_data * data = d;
    char * program_version = get_program_version();
    char * program_mod_date = get_program_modification_date();
    int program_pid = (int) getpid();

    switch (data->type) {

    case NEBTYPE_PROCESS_PRELAUNCH:

        if (ndo_startup_skip_writing_objects != TRUE) {
            ndo_table_genocide();
            ndo_set_all_objects_inactive();
        }

        break;


    case NEBTYPE_PROCESS_START:

        if (ndo_startup_skip_writing_objects != TRUE) {

            ndo_begin_active_objects(active_objects_run);
            ndo_write_object_config(NDO_CONFIG_DUMP_ORIGINAL);
            ndo_end_active_objects();

            ndo_write_config_files();
            ndo_write_config(NDO_CONFIG_DUMP_ORIGINAL);
        }

        break;


    case NEBTYPE_PROCESS_EVENTLOOPSTART:

        if (ndo_startup_skip_writing_objects != TRUE) {
            ndo_write_runtime_variables();
        }

        break;


    case NEBTYPE_PROCESS_SHUTDOWN:
    case NEBTYPE_PROCESS_RESTART:

        MYSQL_RESET_SQL();
        MYSQL_RESET_BIND();

        MYSQL_SET_SQL("UPDATE nagios_programstatus SET program_end_time = FROM_UNIXTIME(?), is_currently_running = 0");
        MYSQL_PREPARE();

        MYSQL_BIND_INT(data->timestamp.tv_sec);

        MYSQL_BIND();
        MYSQL_EXECUTE();

        break;

    }

    MYSQL_RESET_SQL();
    MYSQL_RESET_BIND();

    MYSQL_SET_SQL("INSERT INTO nagios_processevents SET instance_id = 1, event_type = ?, event_time = FROM_UNIXTIME(?), event_time_usec = ?, process_id = ?, program_name = 'Nagios', program_version = ?, program_date = ?");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(data->type);
    MYSQL_BIND_INT(data->timestamp.tv_sec);
    MYSQL_BIND_INT(data->timestamp.tv_usec);
    MYSQL_BIND_INT(program_pid);
    MYSQL_BIND_STR(program_version);
    MYSQL_BIND_STR(program_mod_date);

    MYSQL_BIND();
    MYSQL_EXECUTE();
}


int ndo_handle_timed_event(int type, void * d)
{
    nebstruct_timed_event_data * data = d;

    int object_id = 0;

    if (data->type == NEBTYPE_TIMEDEVENT_SLEEP) {
        return NDO_OK;
    }

    if (data->event_type == EVENT_SERVICE_CHECK) {

        service * svc = data->event_data;
        object_id = ndo_get_object_id_name2(TRUE, NDO_OBJECTTYPE_SERVICE, svc->host_name, svc->description);
    }
    else if (data->event_type == EVENT_HOST_CHECK) {

        host * hst = data->event_data;
        object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_HOST, hst->name);
    }
    else if (data->event_type == EVENT_SCHEDULED_DOWNTIME) {

        scheduled_downtime * dwn = find_downtime(ANY_DOWNTIME, (unsigned long) data->event_data);

        char * host_name = dwn->host_name;
        char * service_description = dwn->service_description;

        if (service_description != NULL && strlen(service_description) > 0) {
            object_id = ndo_get_object_id_name2(TRUE, NDO_OBJECTTYPE_SERVICE, host_name, service_description);
        }
        else {
            object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_HOST, host_name);
        }
    }

    if (data->type == NEBTYPE_TIMEDEVENT_ADD) {

        MYSQL_RESET_SQL();
        MYSQL_RESET_BIND();

        MYSQL_SET_SQL("INSERT INTO nagios_timedeventqueue (instance_id, event_type, queued_time, queued_time_usec, scheduled_time, recurring_event, object_id) VALUES (1,?,FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), event_type = VALUES(event_type), queued_time = VALUES(queued_time), queued_time_usec = VALUES(queued_time_usec), scheduled_time = VALUES(scheduled_time), recurring_event = VALUES(recurring_event), object_id = VALUES(object_id)");
        MYSQL_PREPARE();

        MYSQL_BIND_INT(data->event_type);
        MYSQL_BIND_INT(data->timestamp.tv_sec);
        MYSQL_BIND_INT(data->timestamp.tv_usec);
        MYSQL_BIND_INT(data->run_time);
        MYSQL_BIND_INT(data->recurring);
        MYSQL_BIND_INT(object_id);

        MYSQL_BIND();
        MYSQL_EXECUTE();
    }

    else if (data->type == NEBTYPE_TIMEDEVENT_REMOVE || data->type == NEBTYPE_TIMEDEVENT_EXECUTE) {

        MYSQL_RESET_SQL();
        MYSQL_RESET_BIND();

        MYSQL_SET_SQL("DELETE FROM nagios_timedeventqueue WHERE instance_id = 1 AND event_type = ? AND scheduled_time = FROM_UNIXTIME(?) AND recurring_event = ? AND object_id = ?");
        MYSQL_PREPARE();

        MYSQL_BIND_INT(data->event_type);
        MYSQL_BIND_INT(data->timestamp.tv_sec);
        MYSQL_BIND_INT(data->recurring);
        MYSQL_BIND_INT(object_id);

        MYSQL_BIND();
        MYSQL_EXECUTE();
    }

    if (    data->type == NEBTYPE_TIMEDEVENT_EXECUTE
        && (data->event_type == EVENT_SERVICE_CHECK || data->event_type == EVENT_HOST_CHECK)) {

        MYSQL_RESET_SQL();
        MYSQL_RESET_BIND();

        MYSQL_SET_SQL("DELETE FROM nagios_timedeventqueue WHERE instance_id = 1 AND scheduled_time < FROM_UNIXTIME(?)");
        MYSQL_PREPARE();

        MYSQL_BIND_INT(data->run_time);

        MYSQL_BIND();
        MYSQL_EXECUTE();
    }

    return NDO_OK;
}


int ndo_handle_log(int type, void * d)
{
    nebstruct_log_data * data = d;

    /* this particular function is a bit weird because it starts passing
       logs to the neb module before the database initialization has occured,
       so if the db hasn't been initialized, we just return */
    if (mysql_connection == NULL || ndo_database_connected != TRUE) {
        return NDO_OK;
    }

    if (data->data == NULL || strlen(data->data) == 0) {
        return NDO_OK;
    }

    ndo_tmp_str_len[0] = strlen(data->data);

    ndo_log_data_bind[0].buffer_type = MYSQL_TYPE_LONG;
    ndo_log_data_bind[0].buffer = &(data->entry_time);

    ndo_log_data_bind[1].buffer_type = MYSQL_TYPE_LONG;
    ndo_log_data_bind[1].buffer = &(data->timestamp.tv_sec);

    ndo_log_data_bind[2].buffer_type = MYSQL_TYPE_LONG;
    ndo_log_data_bind[2].buffer = &(data->timestamp.tv_usec);

    ndo_log_data_bind[3].buffer_type = MYSQL_TYPE_LONG;
    ndo_log_data_bind[3].buffer = &(data->data_type);

    ndo_log_data_bind[4].buffer_type = MYSQL_TYPE_STRING;
    ndo_log_data_bind[4].buffer_length = MAX_BIND_BUFFER;
    ndo_log_data_bind[4].buffer = data->data;
    ndo_log_data_bind[4].length = &(ndo_tmp_str_len[0]);

    ndo_log_data_return = mysql_stmt_bind_param(ndo_stmt_log_data, ndo_log_data_bind);
    if (ndo_log_data_return != 0) {
        ndo_log("Log Data: Unable to bind parameters");
        snprintf(ndo_error_msg, 1023, "ndo_log_data_return = %d (%s)", ndo_log_data_return, mysql_stmt_error(ndo_stmt_log_data));
        ndo_log(ndo_error_msg);
        return NDO_ERROR;
    }

    ndo_return = mysql_stmt_execute(ndo_stmt_log_data);
    if (ndo_log_data_return != 0) {
        ndo_log("Log Data: Unable to execute statement");
        snprintf(ndo_error_msg, 1023, "ndo_log_data_return = %d (%s)", ndo_log_data_return, mysql_stmt_error(ndo_stmt_log_data));
        ndo_log(ndo_error_msg);
        return NDO_ERROR;
    }

    return NDO_OK;
}


int ndo_handle_system_command(int type, void * d)
{
    nebstruct_system_command_data * data = d;

    MYSQL_RESET_SQL();
    MYSQL_RESET_BIND();

    MYSQL_SET_SQL("INSERT INTO nagios_systemcommands (instance_id, start_time, start_time_usec, end_time, end_time_usec, command_line, timeout, early_timeout, execution_time, return_code, output, long_output) VALUES (1,FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), start_time = VALUES(start_time), start_time_usec = VALUES(start_time_usec), end_time = VALUES(end_time), end_time_usec = VALUES(end_time_usec), command_line = VALUES(command_line), timeout = VALUES(timeout), early_timeout = VALUES(early_timeout), execution_time = VALUES(execution_time), return_code = VALUES(return_code), output = VALUES(output), long_output = VALUES(long_output)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(data->start_time.tv_sec);
    MYSQL_BIND_INT(data->start_time.tv_usec);
    MYSQL_BIND_INT(data->end_time.tv_sec);
    MYSQL_BIND_INT(data->end_time.tv_usec);
    MYSQL_BIND_STR(data->command_line);
    MYSQL_BIND_INT(data->timeout);
    MYSQL_BIND_INT(data->early_timeout);
    MYSQL_BIND_INT(data->execution_time);
    MYSQL_BIND_INT(data->return_code);
    MYSQL_BIND_STR(data->output);
    MYSQL_BIND_STR(data->output);

    MYSQL_BIND();
    MYSQL_EXECUTE();

    return NDO_OK;
}


int ndo_handle_event_handler(int type, void * d)
{
    nebstruct_event_handler_data * data = d;
    int object_id = 0;

    int command_object_id = 0;

    if (data->eventhandler_type == SERVICE_EVENTHANDLER || data->eventhandler_type == GLOBAL_SERVICE_EVENTHANDLER) {
        object_id = ndo_get_object_id_name2(TRUE, NDO_OBJECTTYPE_SERVICE, data->host_name, data->service_description);
    }
    else {
        object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_HOST, data->host_name);
    }

    command_object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_COMMAND, data->command_name);

    MYSQL_RESET_SQL();
    MYSQL_RESET_BIND();

    MYSQL_SET_SQL("INSERT INTO nagios_eventhandlers (instance_id, start_time, start_time_usec, end_time, end_time_usec, eventhandler_type, object_id, state, state_type, command_object_id, command_args, command_line, timeout, early_timeout, execution_time, return_code, output, long_output) VALUES (1,FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), start_time = VALUES(start_time), start_time_usec = VALUES(start_time_usec), end_time = VALUES(end_time), end_time_usec = VALUES(end_time_usec), eventhandler_type = VALUES(eventhandler_type), object_id = VALUES(object_id), state = VALUES(state), state_type = VALUES(state_type), command_object_id = VALUES(command_object_id), command_args = VALUES(command_args), command_line = VALUES(command_line), timeout = VALUES(timeout), early_timeout = VALUES(early_timeout), execution_time = VALUES(execution_time), return_code = VALUES(return_code), output = VALUES(output), long_output = VALUES(long_output)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(data->start_time.tv_sec);
    MYSQL_BIND_INT(data->start_time.tv_usec);
    MYSQL_BIND_INT(data->end_time.tv_sec);
    MYSQL_BIND_INT(data->end_time.tv_usec);
    MYSQL_BIND_INT(data->eventhandler_type);
    MYSQL_BIND_INT(object_id);
    MYSQL_BIND_INT(data->state);
    MYSQL_BIND_INT(data->state_type);
    MYSQL_BIND_INT(command_object_id);
    MYSQL_BIND_STR(data->command_args);
    MYSQL_BIND_STR(data->command_line);
    MYSQL_BIND_INT(data->timeout);
    MYSQL_BIND_INT(data->early_timeout);
    MYSQL_BIND_INT(data->execution_time);
    MYSQL_BIND_INT(data->return_code);
    MYSQL_BIND_STR(data->output);
    MYSQL_BIND_STR(data->output);

    MYSQL_BIND();
    MYSQL_EXECUTE();

    return NDO_OK;
}


int ndo_handle_host_check(int type, void * d)
{
    nebstruct_host_check_data * data = d;
    int object_id = 0;

    int command_object_id = 0;

    /* this is the only data we care about / need */
    if (type != NEBTYPE_HOSTCHECK_PROCESSED) {
        return NDO_OK;
    }

    object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_HOST, data->host_name);

    command_object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_COMMAND, data->command_name);

    MYSQL_RESET_SQL();
    MYSQL_RESET_BIND();

    MYSQL_SET_SQL("INSERT INTO nagios_hostchecks (instance_id, start_time, start_time_usec, end_time, end_time_usec, host_object_id, check_type, current_check_attempt, max_check_attempts, state, state_type, timeout, early_timeout, execution_time, latency, return_code, output, long_output, perfdata, command_object_id, command_args, command_line) VALUES (1,FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), start_time = VALUES(start_time), start_time_usec = VALUES(start_time_usec), end_time = VALUES(end_time), end_time_usec = VALUES(end_time_usec), host_object_id = VALUES(host_object_id), check_type = VALUES(check_type), current_check_attempt = VALUES(current_check_attempt), max_check_attempts = VALUES(max_check_attempts), state = VALUES(state), state_type = VALUES(state_type), timeout = VALUES(timeout), early_timeout = VALUES(early_timeout), execution_time = VALUES(execution_time), latency = VALUES(latency), return_code = VALUES(return_code), output = VALUES(output), long_output = VALUES(long_output), perfdata = VALUES(perfdata), command_object_id = VALUES(command_object_id), command_args = VALUES(command_args), command_line = VALUES(command_line)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(data->start_time.tv_sec);
    MYSQL_BIND_INT(data->start_time.tv_usec);
    MYSQL_BIND_INT(data->end_time.tv_sec);
    MYSQL_BIND_INT(data->end_time.tv_usec);
    MYSQL_BIND_INT(object_id);
    MYSQL_BIND_INT(data->check_type);
    MYSQL_BIND_INT(data->current_attempt);
    MYSQL_BIND_INT(data->max_attempts);
    MYSQL_BIND_INT(data->state);
    MYSQL_BIND_INT(data->state_type);
    MYSQL_BIND_INT(data->timeout);
    MYSQL_BIND_INT(data->early_timeout);
    MYSQL_BIND_DOUBLE(data->execution_time);
    MYSQL_BIND_DOUBLE(data->latency);
    MYSQL_BIND_INT(data->return_code);
    MYSQL_BIND_STR(data->output);
    MYSQL_BIND_STR(data->long_output);
    MYSQL_BIND_STR(data->perf_data);
    MYSQL_BIND_INT(command_object_id);
    MYSQL_BIND_STR(data->command_args);
    MYSQL_BIND_STR(data->command_line);

    MYSQL_BIND();
    MYSQL_EXECUTE();

    return NDO_OK;
}

int ndo_handle_service_check(int type, void * d)
{
    nebstruct_service_check_data * data = d;
    int object_id = 0;

    int command_object_id = 0;

    /* this is the only data we care about / need */
    if (type != NEBTYPE_SERVICECHECK_PROCESSED) {
        return NDO_OK;
    }

    object_id = ndo_get_object_id_name2(TRUE, NDO_OBJECTTYPE_SERVICE, data->host_name, data->service_description);

    if (data->command_name != NULL) {
        /* Nagios Core appears to always pass NULL for its command arguments when brokering NEBTYPE_SERVICECHECK_PROCESSED.
         * It's not clear why this was done, so we're working around it here for now.
         */
        command_object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_COMMAND, data->command_name);
    }

    MYSQL_RESET_SQL();
    MYSQL_RESET_BIND();

    MYSQL_SET_SQL("INSERT INTO nagios_servicechecks (instance_id, start_time, start_time_usec, end_time, end_time_usec, service_object_id, check_type, current_check_attempt, max_check_attempts, state, state_type, timeout, early_timeout, execution_time, latency, return_code, output, long_output, perfdata, command_object_id, command_args, command_line) VALUES (1,FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), start_time = VALUES(start_time), start_time_usec = VALUES(start_time_usec), end_time = VALUES(end_time), end_time_usec = VALUES(end_time_usec), service_object_id = VALUES(service_object_id), check_type = VALUES(check_type), current_check_attempt = VALUES(current_check_attempt), max_check_attempts = VALUES(max_check_attempts), state = VALUES(state), state_type = VALUES(state_type), timeout = VALUES(timeout), early_timeout = VALUES(early_timeout), execution_time = VALUES(execution_time), latency = VALUES(latency), return_code = VALUES(return_code), output = VALUES(output), long_output = VALUES(long_output), perfdata = VALUES(perfdata), command_object_id = VALUES(command_object_id), command_args = VALUES(command_args), command_line = VALUES(command_line)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(data->start_time.tv_sec);
    MYSQL_BIND_INT(data->start_time.tv_usec);
    MYSQL_BIND_INT(data->end_time.tv_sec);
    MYSQL_BIND_INT(data->end_time.tv_usec);
    MYSQL_BIND_INT(object_id);
    MYSQL_BIND_INT(data->check_type);
    MYSQL_BIND_INT(data->current_attempt);
    MYSQL_BIND_INT(data->max_attempts);
    MYSQL_BIND_INT(data->state);
    MYSQL_BIND_INT(data->state_type);
    MYSQL_BIND_INT(data->timeout);
    MYSQL_BIND_INT(data->early_timeout);
    MYSQL_BIND_DOUBLE(data->execution_time);
    MYSQL_BIND_DOUBLE(data->latency);
    MYSQL_BIND_INT(data->return_code);
    MYSQL_BIND_STR(data->output);
    MYSQL_BIND_STR(data->long_output);
    MYSQL_BIND_STR(data->perf_data);
    MYSQL_BIND_INT(command_object_id);
    MYSQL_BIND_STR(data->command_args);
    MYSQL_BIND_STR(data->command_line);

    MYSQL_BIND();
    MYSQL_EXECUTE();

    return NDO_OK;
}


int ndo_handle_comment(int type, void * d)
{
    nebstruct_comment_data * data = d;
    int object_id = 0;

    if (data->comment_type == SERVICE_COMMENT) {
        object_id = ndo_get_object_id_name2(TRUE, NDO_OBJECTTYPE_SERVICE, data->host_name, data->service_description);
    }
    else {
        object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_HOST, data->host_name);
    }

    if (data->type == NEBTYPE_COMMENT_ADD || data->type == NEBTYPE_COMMENT_LOAD) {

        MYSQL_RESET_SQL();
        MYSQL_RESET_BIND();

        MYSQL_SET_SQL("INSERT INTO nagios_commenthistory (instance_id, comment_type, entry_type, object_id, comment_time, internal_comment_id, author_name, comment_data, is_persistent, comment_source, expires, expiration_time, entry_time, entry_time_usec) VALUES (1,?,?,?,FROM_UNIXTIME(?),?,?,?,?,?,?,FROM_UNIXTIME(?),FROM_UNIXTIME(?),?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), comment_type = VALUES(comment_type), entry_type = VALUES(entry_type), object_id = VALUES(object_id), comment_time = VALUES(comment_time), internal_comment_id = VALUES(internal_comment_id), author_name = VALUES(author_name), comment_data = VALUES(comment_data), is_persistent = VALUES(is_persistent), comment_source = VALUES(comment_source), expires = VALUES(expires), expiration_time = VALUES(expiration_time), entry_time = VALUES(entry_time), entry_time_usec = VALUES(entry_time_usec)");
        MYSQL_PREPARE();

        MYSQL_BIND_INT(data->comment_type);
        MYSQL_BIND_INT(data->entry_type);
        MYSQL_BIND_INT(object_id);
        MYSQL_BIND_INT(data->entry_time);
        MYSQL_BIND_INT(data->comment_id);
        MYSQL_BIND_STR(data->author_name);
        MYSQL_BIND_STR(data->comment_data);
        MYSQL_BIND_INT(data->persistent);
        MYSQL_BIND_INT(data->source);
        MYSQL_BIND_INT(data->expires);
        MYSQL_BIND_INT(data->expire_time);
        MYSQL_BIND_INT(data->timestamp.tv_sec);
        MYSQL_BIND_INT(data->timestamp.tv_usec);

        MYSQL_BIND();
        MYSQL_EXECUTE();

        /* we just have to rename "commenthistory" to "comments"

            strlen("INSERT INTO nagios_comments      ") = 33

                           "INSERT INTO nagios_commenthistory" */
        strncpy(ndo_query, "INSERT INTO nagios_comments      ", 33);

        MYSQL_PREPARE();

        MYSQL_BIND();
        MYSQL_EXECUTE();
    }

    if (data->type == NEBTYPE_COMMENT_DELETE) {

        MYSQL_RESET_SQL();
        MYSQL_RESET_BIND();

        MYSQL_SET_SQL("UPDATE nagios_commenthistory SET deletion_time = FROM_UNIXTIME(?), deletion_time_usec = ? WHERE comment_time = FROM_UNIXTIME(?) AND internal_comment_id = ?");
        MYSQL_PREPARE();

        MYSQL_BIND_INT(data->timestamp.tv_sec);
        MYSQL_BIND_INT(data->timestamp.tv_usec);
        MYSQL_BIND_INT(data->entry_time);
        MYSQL_BIND_INT(data->comment_id);

        MYSQL_BIND();
        MYSQL_EXECUTE();

        /** now we actually delete it **/

        MYSQL_RESET_SQL();
        MYSQL_RESET_BIND();

        MYSQL_SET_SQL("DELETE FROM nagios_comments WHERE comment_time = FROM_UNIXTIME(?) AND internal_comment_id = ?");
        MYSQL_PREPARE();

        MYSQL_BIND_INT(data->entry_time);
        MYSQL_BIND_INT(data->comment_id);

        MYSQL_BIND();
        MYSQL_EXECUTE();
    }

    return NDO_OK;
}


int ndo_handle_downtime(int type, void * d)
{
    nebstruct_downtime_data * data = d;
    int object_id = 0;

    if (data->downtime_type == SERVICE_DOWNTIME) {
        object_id = ndo_get_object_id_name2(TRUE, NDO_OBJECTTYPE_SERVICE, data->host_name, data->service_description);
    }
    else {
        object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_HOST, data->host_name);
    }

    if (data->type == NEBTYPE_DOWNTIME_ADD || data->type == NEBTYPE_DOWNTIME_LOAD) {

        MYSQL_RESET_SQL();
        MYSQL_RESET_BIND();

        MYSQL_SET_SQL("INSERT INTO nagios_scheduleddowntime (instance_id, downtime_type, object_id, entry_time, author_name, comment_data, internal_downtime_id, triggered_by_id, is_fixed, duration, scheduled_start_time, scheduled_end_time) VALUES (1,?,?,FROM_UNIXTIME(?),?,?,?,?,?,?,FROM_UNIXTIME(?),FROM_UNIXTIME(?)) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), downtime_type = VALUES(downtime_type), object_id = VALUES(object_id), entry_time = VALUES(entry_time), author_name = VALUES(author_name), comment_data = VALUES(comment_data), internal_downtime_id = VALUES(internal_downtime_id), triggered_by_id = VALUES(triggered_by_id), is_fixed = VALUES(is_fixed), duration = VALUES(duration), scheduled_start_time = VALUES(scheduled_start_time), scheduled_end_time = VALUES(scheduled_end_time)");
        MYSQL_PREPARE();

        MYSQL_BIND_INT(data->downtime_type);
        MYSQL_BIND_INT(object_id);
        MYSQL_BIND_INT(data->entry_time);
        MYSQL_BIND_STR(data->author_name);
        MYSQL_BIND_STR(data->comment_data);
        MYSQL_BIND_INT(data->downtime_id);
        MYSQL_BIND_INT(data->triggered_by);
        MYSQL_BIND_INT(data->fixed);
        MYSQL_BIND_INT(data->duration);
        MYSQL_BIND_INT(data->start_time);
        MYSQL_BIND_INT(data->end_time);

        MYSQL_BIND();
        MYSQL_EXECUTE();

        /* we just have to rename "scheduleddowntime" to "downtimehistory"

           strlen("INSERT INTO nagios_downtimehistory      ") = 36

                           "INSERT INTO nagios_scheduleddowntime" */
        strncpy(ndo_query, "INSERT INTO nagios_downtimehistory  ", 36);

        MYSQL_PREPARE();

        MYSQL_BIND();
        MYSQL_EXECUTE();
    }

    if (data->type == NEBTYPE_DOWNTIME_START) {

        MYSQL_RESET_SQL();
        MYSQL_RESET_BIND();

        MYSQL_SET_SQL("UPDATE nagios_scheduleddowntime SET actual_start_time = FROM_UNIXTIME(?), actual_start_time_usec = ?, was_started = 1 WHERE object_id = ? AND downtime_type = ? AND entry_time = FROM_UNIXTIME(?) AND scheduled_start_time = FROM_UNIXTIME(?) AND scheduled_end_time = FROM_UNIXTIME(?)");
        MYSQL_PREPARE();

        MYSQL_BIND_INT(data->timestamp.tv_sec);
        MYSQL_BIND_INT(data->timestamp.tv_usec);
        MYSQL_BIND_INT(object_id);
        MYSQL_BIND_INT(data->downtime_type);
        MYSQL_BIND_INT(data->entry_time);
        MYSQL_BIND_INT(data->start_time);
        MYSQL_BIND_INT(data->end_time);

        MYSQL_BIND();
        MYSQL_EXECUTE();

        /* we just have to rename "scheduleddowntime" to "downtimehistory"

            strlen("UPDATE nagios_downtimehistory  ") = 31

                           "UPDATE nagios_scheduleddowntime" */
        strncpy(ndo_query, "UPDATE nagios_downtimehistory  ", 31);

        MYSQL_PREPARE();

        MYSQL_BIND();
        MYSQL_EXECUTE();
    }

    if (data->type == NEBTYPE_DOWNTIME_STOP) {

        int cancelled = 0;

        if (data->attr == NEBATTR_DOWNTIME_STOP_CANCELLED) {
            cancelled = 1;
        }

        MYSQL_RESET_SQL();
        MYSQL_RESET_BIND();

        MYSQL_SET_SQL("UPDATE nagios_downtimehistory SET actual_end_time = FROM_UNIXTIME(?), actual_end_time_usec = ?, was_cancelled = ? WHERE object_id = ? AND downtime_type = ? AND entry_time = FROM_UNIXTIME(?) AND scheduled_start_time = FROM_UNIXTIME(?) AND scheduled_end_time = FROM_UNIXTIME(?)");
        MYSQL_PREPARE();

        MYSQL_BIND_INT(data->timestamp.tv_sec);
        MYSQL_BIND_INT(data->timestamp.tv_usec);
        MYSQL_BIND_INT(cancelled);
        MYSQL_BIND_INT(object_id);
        MYSQL_BIND_INT(data->downtime_type);
        MYSQL_BIND_INT(data->entry_time);
        MYSQL_BIND_INT(data->start_time);
        MYSQL_BIND_INT(data->end_time);

        MYSQL_BIND();
        MYSQL_EXECUTE();
    }

    if (data->type == NEBTYPE_DOWNTIME_STOP || data->type == NEBTYPE_DOWNTIME_DELETE) {

        MYSQL_RESET_SQL();
        MYSQL_RESET_BIND();

        MYSQL_SET_SQL("DELETE FROM nagios_scheduleddowntime WHERE downtime_type = ? AND object_id = ? AND entry_time = FROM_UNIXTIME(?) AND scheduled_start_time = FROM_UNIXTIME(?) AND scheduled_end_time = FROM_UNIXTIME(?)");
        MYSQL_PREPARE();

        MYSQL_BIND_INT(data->downtime_type);
        MYSQL_BIND_INT(object_id);
        MYSQL_BIND_INT(data->entry_time);
        MYSQL_BIND_INT(data->start_time);
        MYSQL_BIND_INT(data->end_time);

        MYSQL_BIND();
        MYSQL_EXECUTE();
    }
}


int ndo_handle_flapping(int type, void * d)
{
    nebstruct_flapping_data * data = d;
    int object_id = 0;
    nagios_comment * comment = NULL;
    time_t comment_time = 0;

    if (data->flapping_type == SERVICE_FLAPPING) {
        object_id = ndo_get_object_id_name2(TRUE, NDO_OBJECTTYPE_SERVICE, data->host_name, data->service_description);
        comment = find_service_comment(data->comment_id);
    }
    else {
        object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_HOST, data->host_name);
        comment = find_host_comment(data->comment_id);
    }

    if (data->comment_id != 0L && comment != NULL) {
        comment_time = comment->entry_time;
    }

    MYSQL_RESET_SQL();
    MYSQL_RESET_BIND();

    MYSQL_SET_SQL("INSERT INTO nagios_flappinghistory (instance_id, event_time, event_time_usec, event_type, reason_type, flapping_type, object_id, percent_state_change, low_threshold, high_threshold, comment_time, internal_comment_id) VALUES (1,FROM_UNIXTIME(?),?,?,?,?,?,?,?,?,FROM_UNIXTIME(?),?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), event_time = VALUES(event_time), event_time_usec = VALUES(event_time_usec), event_type = VALUES(event_type), reason_type = VALUES(reason_type), flapping_type = VALUES(flapping_type), object_id = VALUES(object_id), percent_state_change = VALUES(percent_state_change), low_threshold = VALUES(low_threshold), high_threshold = VALUES(high_threshold), comment_time = VALUES(comment_time), internal_comment_id = VALUES(internal_comment_id)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(data->timestamp.tv_sec);
    MYSQL_BIND_INT(data->timestamp.tv_usec);
    MYSQL_BIND_INT(data->type);
    MYSQL_BIND_INT(data->attr);
    MYSQL_BIND_INT(data->flapping_type);
    MYSQL_BIND_INT(object_id);
    MYSQL_BIND_DOUBLE(data->percent_change);
    MYSQL_BIND_DOUBLE(data->low_threshold);
    MYSQL_BIND_DOUBLE(data->high_threshold);
    MYSQL_BIND_INT(comment_time);
    MYSQL_BIND_INT(data->comment_id);

    MYSQL_BIND();
    MYSQL_EXECUTE();
}


int ndo_handle_program_status(int type, void * d)
{
    nebstruct_program_status_data * data = d;

    MYSQL_RESET_SQL();
    MYSQL_RESET_BIND();

    MYSQL_SET_SQL("INSERT INTO nagios_programstatus (instance_id, status_update_time, program_start_time, is_currently_running, process_id, daemon_mode, last_command_check, last_log_rotation, notifications_enabled, active_service_checks_enabled, passive_service_checks_enabled, active_host_checks_enabled, passive_host_checks_enabled, event_handlers_enabled, flap_detection_enabled, failure_prediction_enabled, process_performance_data, obsess_over_hosts, obsess_over_services, modified_host_attributes, modified_service_attributes, global_host_event_handler, global_service_event_handler) VALUES (1,FROM_UNIXTIME(?),FROM_UNIXTIME(?),1,?,?,FROM_UNIXTIME(0),FROM_UNIXTIME(?),?,?,?,?,?,?,?,0,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), status_update_time = VALUES(status_update_time), program_start_time = VALUES(program_start_time), is_currently_running = VALUES(is_currently_running), process_id = VALUES(process_id), daemon_mode = VALUES(daemon_mode), last_command_check = VALUES(last_command_check), last_log_rotation = VALUES(last_log_rotation), notifications_enabled = VALUES(notifications_enabled), active_service_checks_enabled = VALUES(active_service_checks_enabled), passive_service_checks_enabled = VALUES(passive_service_checks_enabled), active_host_checks_enabled = VALUES(active_host_checks_enabled), passive_host_checks_enabled = VALUES(passive_host_checks_enabled), event_handlers_enabled = VALUES(event_handlers_enabled), flap_detection_enabled = VALUES(flap_detection_enabled), failure_prediction_enabled = VALUES(failure_prediction_enabled), process_performance_data = VALUES(process_performance_data), obsess_over_hosts = VALUES(obsess_over_hosts), obsess_over_services = VALUES(obsess_over_services), modified_host_attributes = VALUES(modified_host_attributes), modified_service_attributes = VALUES(modified_service_attributes), global_host_event_handler = VALUES(global_host_event_handler), global_service_event_handler = VALUES(global_service_event_handler)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(data->timestamp.tv_sec);
    MYSQL_BIND_INT(data->program_start);
    MYSQL_BIND_INT(data->pid);
    MYSQL_BIND_INT(data->daemon_mode);
    MYSQL_BIND_INT(data->last_log_rotation);
    MYSQL_BIND_INT(data->notifications_enabled);
    MYSQL_BIND_INT(data->active_service_checks_enabled);
    MYSQL_BIND_INT(data->passive_service_checks_enabled);
    MYSQL_BIND_INT(data->active_host_checks_enabled);
    MYSQL_BIND_INT(data->passive_host_checks_enabled);
    MYSQL_BIND_INT(data->event_handlers_enabled);
    MYSQL_BIND_INT(data->flap_detection_enabled);
    MYSQL_BIND_INT(data->process_performance_data);
    MYSQL_BIND_INT(data->obsess_over_hosts);
    MYSQL_BIND_INT(data->obsess_over_services);
    MYSQL_BIND_INT(data->modified_host_attributes);
    MYSQL_BIND_INT(data->modified_service_attributes);
    MYSQL_BIND_STR(data->global_host_event_handler);
    MYSQL_BIND_STR(data->global_service_event_handler);

    MYSQL_BIND();
    MYSQL_EXECUTE();
}


int ndo_handle_host_status(int type, void * d)
{
    nebstruct_host_status_data * data = d;
    int host_object_id        = 0;
    int timeperiod_object_id  = 0;
    host * hst                = NULL;
    timeperiod * tm           = NULL;

    if (data->object_ptr == NULL) {
        NDO_REPORT_ERROR("Broker data pointer(s) is/are null");
        return NDO_OK;
    }

    hst = data->object_ptr;
    tm = hst->check_period_ptr;

    host_object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_HOST, hst->name);
    timeperiod_object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_TIMEPERIOD, tm->name);

    MYSQL_RESET_SQL();
    MYSQL_RESET_BIND();

    MYSQL_SET_SQL("INSERT INTO nagios_hoststatus (instance_id, host_object_id, status_update_time, output, long_output, perfdata, current_state, has_been_checked, should_be_scheduled, current_check_attempt, max_check_attempts, last_check, next_check, check_type, last_state_change, last_hard_state_change, last_hard_state, last_time_up, last_time_down, last_time_unreachable, state_type, last_notification, next_notification, no_more_notifications, notifications_enabled, problem_has_been_acknowledged, acknowledgement_type, current_notification_number, passive_checks_enabled, active_checks_enabled, event_handler_enabled, flap_detection_enabled, is_flapping, percent_state_change, latency, execution_time, scheduled_downtime_depth, failure_prediction_enabled, process_performance_data, obsess_over_host, modified_host_attributes, event_handler, check_command, normal_check_interval, retry_check_interval, check_timeperiod_object_id) VALUES (1,?,FROM_UNIXTIME(?),?,?,?,?,?,?,?,?,FROM_UNIXTIME(?),FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),FROM_UNIXTIME(?),FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),FROM_UNIXTIME(?),?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), host_object_id = VALUES(host_object_id), status_update_time = VALUES(status_update_time), output = VALUES(output), long_output = VALUES(long_output), perfdata = VALUES(perfdata), current_state = VALUES(current_state), has_been_checked = VALUES(has_been_checked), should_be_scheduled = VALUES(should_be_scheduled), current_check_attempt = VALUES(current_check_attempt), max_check_attempts = VALUES(max_check_attempts), last_check = VALUES(last_check), next_check = VALUES(next_check), check_type = VALUES(check_type), last_state_change = VALUES(last_state_change), last_hard_state_change = VALUES(last_hard_state_change), last_hard_state = VALUES(last_hard_state), last_time_up = VALUES(last_time_up), last_time_down = VALUES(last_time_down), last_time_unreachable = VALUES(last_time_unreachable), state_type = VALUES(state_type), last_notification = VALUES(last_notification), next_notification = VALUES(next_notification), no_more_notifications = VALUES(no_more_notifications), notifications_enabled = VALUES(notifications_enabled), problem_has_been_acknowledged = VALUES(problem_has_been_acknowledged), acknowledgement_type = VALUES(acknowledgement_type), current_notification_number = VALUES(current_notification_number), passive_checks_enabled = VALUES(passive_checks_enabled), active_checks_enabled = VALUES(active_checks_enabled), event_handler_enabled = VALUES(event_handler_enabled), flap_detection_enabled = VALUES(flap_detection_enabled), is_flapping = VALUES(is_flapping), percent_state_change = VALUES(percent_state_change), latency = VALUES(latency), execution_time = VALUES(execution_time), scheduled_downtime_depth = VALUES(scheduled_downtime_depth), failure_prediction_enabled = VALUES(failure_prediction_enabled), process_performance_data = VALUES(process_performance_data), obsess_over_host = VALUES(obsess_over_host), modified_host_attributes = VALUES(modified_host_attributes), event_handler = VALUES(event_handler), check_command = VALUES(check_command), normal_check_interval = VALUES(normal_check_interval), retry_check_interval = VALUES(retry_check_interval), check_timeperiod_object_id = VALUES(check_timeperiod_object_id)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(host_object_id);
    MYSQL_BIND_INT(data->timestamp.tv_sec);
    MYSQL_BIND_STR(hst->plugin_output);
    MYSQL_BIND_STR(hst->long_plugin_output);
    MYSQL_BIND_STR(hst->perf_data);
    MYSQL_BIND_INT(hst->current_state);
    MYSQL_BIND_INT(hst->has_been_checked);
    MYSQL_BIND_INT(hst->should_be_scheduled);
    MYSQL_BIND_INT(hst->current_attempt);
    MYSQL_BIND_INT(hst->max_attempts);
    MYSQL_BIND_INT(hst->last_check);
    MYSQL_BIND_INT(hst->next_check);
    MYSQL_BIND_INT(hst->check_type);
    MYSQL_BIND_INT(hst->last_state_change);
    MYSQL_BIND_INT(hst->last_hard_state_change);
    MYSQL_BIND_INT(hst->last_hard_state);
    MYSQL_BIND_INT(hst->last_time_up);
    MYSQL_BIND_INT(hst->last_time_down);
    MYSQL_BIND_INT(hst->last_time_unreachable);
    MYSQL_BIND_INT(hst->state_type);
    MYSQL_BIND_INT(hst->last_notification);
    MYSQL_BIND_INT(hst->next_notification);
    MYSQL_BIND_INT(hst->no_more_notifications);
    MYSQL_BIND_INT(hst->notifications_enabled);
    MYSQL_BIND_INT(hst->problem_has_been_acknowledged);
    MYSQL_BIND_INT(hst->acknowledgement_type);
    MYSQL_BIND_INT(hst->current_notification_number);
    MYSQL_BIND_INT(hst->accept_passive_checks);
    MYSQL_BIND_INT(hst->checks_enabled);
    MYSQL_BIND_INT(hst->event_handler_enabled);
    MYSQL_BIND_INT(hst->flap_detection_enabled);
    MYSQL_BIND_INT(hst->is_flapping);
    MYSQL_BIND_DOUBLE(hst->percent_state_change);
    MYSQL_BIND_DOUBLE(hst->latency);
    MYSQL_BIND_DOUBLE(hst->execution_time);
    MYSQL_BIND_INT(hst->scheduled_downtime_depth);
    MYSQL_BIND_INT(hst->process_performance_data);
    MYSQL_BIND_INT(hst->obsess);
    MYSQL_BIND_INT(hst->modified_attributes);
    MYSQL_BIND_STR(hst->event_handler);
    MYSQL_BIND_STR(hst->check_command);
    MYSQL_BIND_DOUBLE(hst->check_interval);
    MYSQL_BIND_DOUBLE(hst->retry_interval);
    MYSQL_BIND_INT(timeperiod_object_id);

    MYSQL_BIND();
    MYSQL_EXECUTE();
}


int ndo_handle_service_status(int type, void * d)
{
    nebstruct_service_status_data * data = d;

    int service_object_id     = 0;
    int timeperiod_object_id  = 0;
    service * svc             = NULL;
    timeperiod * tm           = NULL;

    if (data->object_ptr == NULL) {
        NDO_REPORT_ERROR("Broker data pointer(s) is/are null");
        return NDO_OK;
    }

    svc = data->object_ptr;
    tm = svc->check_period_ptr;

    service_object_id = ndo_get_object_id_name2(TRUE, NDO_OBJECTTYPE_SERVICE, svc->host_name, svc->description);
    timeperiod_object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_TIMEPERIOD, tm->name);

    MYSQL_RESET_SQL();
    MYSQL_RESET_BIND();

    MYSQL_SET_SQL("INSERT INTO nagios_servicestatus (instance_id, service_object_id, status_update_time, output, long_output, perfdata, current_state, has_been_checked, should_be_scheduled, current_check_attempt, max_check_attempts, last_check, next_check, check_type, last_state_change, last_hard_state_change, last_hard_state, last_time_ok, last_time_warning, last_time_unknown, last_time_critical, state_type, last_notification, next_notification, no_more_notifications, notifications_enabled, problem_has_been_acknowledged, acknowledgement_type, current_notification_number, passive_checks_enabled, active_checks_enabled, event_handler_enabled, flap_detection_enabled, is_flapping, percent_state_change, latency, execution_time, scheduled_downtime_depth, failure_prediction_enabled, process_performance_data, obsess_over_service, modified_service_attributes, event_handler, check_command, normal_check_interval, retry_check_interval, check_timeperiod_object_id) VALUES (1,?,FROM_UNIXTIME(?),?,?,?,?,?,?,?,?,FROM_UNIXTIME(?),FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),FROM_UNIXTIME(?),FROM_UNIXTIME(?),FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),FROM_UNIXTIME(?),?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), service_object_id = VALUES(service_object_id), status_update_time = VALUES(status_update_time), output = VALUES(output), long_output = VALUES(long_output), perfdata = VALUES(perfdata), current_state = VALUES(current_state), has_been_checked = VALUES(has_been_checked), should_be_scheduled = VALUES(should_be_scheduled), current_check_attempt = VALUES(current_check_attempt), max_check_attempts = VALUES(max_check_attempts), last_check = VALUES(last_check), next_check = VALUES(next_check), check_type = VALUES(check_type), last_state_change = VALUES(last_state_change), last_hard_state_change = VALUES(last_hard_state_change), last_hard_state = VALUES(last_hard_state), last_time_ok = VALUES(last_time_ok), last_time_warning = VALUES(last_time_warning), last_time_unknown = VALUES(last_time_unknown), last_time_critical = VALUES(last_time_critical), state_type = VALUES(state_type), last_notification = VALUES(last_notification), next_notification = VALUES(next_notification), no_more_notifications = VALUES(no_more_notifications), notifications_enabled = VALUES(notifications_enabled), problem_has_been_acknowledged = VALUES(problem_has_been_acknowledged), acknowledgement_type = VALUES(acknowledgement_type), current_notification_number = VALUES(current_notification_number), passive_checks_enabled = VALUES(passive_checks_enabled), active_checks_enabled = VALUES(active_checks_enabled), event_handler_enabled = VALUES(event_handler_enabled), flap_detection_enabled = VALUES(flap_detection_enabled), is_flapping = VALUES(is_flapping), percent_state_change = VALUES(percent_state_change), latency = VALUES(latency), execution_time = VALUES(execution_time), scheduled_downtime_depth = VALUES(scheduled_downtime_depth), failure_prediction_enabled = VALUES(failure_prediction_enabled), process_performance_data = VALUES(process_performance_data), obsess_over_service = VALUES(obsess_over_service), modified_service_attributes = VALUES(modified_service_attributes), event_handler = VALUES(event_handler), check_command = VALUES(check_command), normal_check_interval = VALUES(normal_check_interval), retry_check_interval = VALUES(retry_check_interval), check_timeperiod_object_id = VALUES(check_timeperiod_object_id)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(service_object_id);
    MYSQL_BIND_INT(data->timestamp.tv_sec);
    MYSQL_BIND_STR(svc->plugin_output);
    MYSQL_BIND_STR(svc->long_plugin_output);
    MYSQL_BIND_STR(svc->perf_data);
    MYSQL_BIND_INT(svc->current_state);
    MYSQL_BIND_INT(svc->has_been_checked);
    MYSQL_BIND_INT(svc->should_be_scheduled);
    MYSQL_BIND_INT(svc->current_attempt);
    MYSQL_BIND_INT(svc->max_attempts);
    MYSQL_BIND_INT(svc->last_check);
    MYSQL_BIND_INT(svc->next_check);
    MYSQL_BIND_INT(svc->check_type);
    MYSQL_BIND_INT(svc->last_state_change);
    MYSQL_BIND_INT(svc->last_hard_state_change);
    MYSQL_BIND_INT(svc->last_hard_state);
    MYSQL_BIND_INT(svc->last_time_ok);
    MYSQL_BIND_INT(svc->last_time_warning);
    MYSQL_BIND_INT(svc->last_time_unknown);
    MYSQL_BIND_INT(svc->last_time_critical);
    MYSQL_BIND_INT(svc->state_type);
    MYSQL_BIND_INT(svc->last_notification);
    MYSQL_BIND_INT(svc->next_notification);
    MYSQL_BIND_INT(svc->no_more_notifications);
    MYSQL_BIND_INT(svc->notifications_enabled);
    MYSQL_BIND_INT(svc->problem_has_been_acknowledged);
    MYSQL_BIND_INT(svc->acknowledgement_type);
    MYSQL_BIND_INT(svc->current_notification_number);
    MYSQL_BIND_INT(svc->accept_passive_checks);
    MYSQL_BIND_INT(svc->checks_enabled);
    MYSQL_BIND_INT(svc->event_handler_enabled);
    MYSQL_BIND_INT(svc->flap_detection_enabled);
    MYSQL_BIND_INT(svc->is_flapping);
    MYSQL_BIND_DOUBLE(svc->percent_state_change);
    MYSQL_BIND_DOUBLE(svc->latency);
    MYSQL_BIND_DOUBLE(svc->execution_time);
    MYSQL_BIND_INT(svc->scheduled_downtime_depth);
    MYSQL_BIND_INT(svc->process_performance_data);
    MYSQL_BIND_INT(svc->obsess);
    MYSQL_BIND_INT(svc->modified_attributes);
    MYSQL_BIND_STR(svc->event_handler);
    MYSQL_BIND_STR(svc->check_command);
    MYSQL_BIND_DOUBLE(svc->check_interval);
    MYSQL_BIND_DOUBLE(svc->retry_interval);
    MYSQL_BIND_INT(timeperiod_object_id);

    MYSQL_BIND();
    MYSQL_EXECUTE();
}


int ndo_handle_contact_status(int type, void * d)
{
    nebstruct_contact_status_data * data = d;

    int contact_object_id     = 0;
    contact * cnt             = NULL;

    if (data->object_ptr == NULL) {
        NDO_REPORT_ERROR("Broker data pointer(s) is/are null");
        return NDO_OK;
    }

    cnt = data->object_ptr;

    contact_object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_CONTACT, cnt->name);

    MYSQL_RESET_BIND();
    MYSQL_RESET_SQL();

    MYSQL_SET_SQL("INSERT INTO nagios_contactstatus (instance_id, contact_object_id, status_update_time, host_notifications_enabled, service_notifications_enabled, last_host_notification, last_service_notification, modified_attributes, modified_host_attributes, modified_service_attributes) VALUES (1,?,FROM_UNIXTIME(?),?,?,FROM_UNIXTIME(?),FROM_UNIXTIME(?),?,?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), contact_object_id = VALUES(contact_object_id), status_update_time = VALUES(status_update_time), host_notifications_enabled = VALUES(host_notifications_enabled), service_notifications_enabled = VALUES(service_notifications_enabled), last_host_notification = VALUES(last_host_notification), last_service_notification = VALUES(last_service_notification), modified_attributes = VALUES(modified_attributes), modified_host_attributes = VALUES(modified_host_attributes), modified_service_attributes = VALUES(modified_service_attributes)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(contact_object_id);
    MYSQL_BIND_INT(data->timestamp.tv_sec);
    MYSQL_BIND_INT(cnt->host_notifications_enabled);
    MYSQL_BIND_INT(cnt->service_notifications_enabled);
    MYSQL_BIND_INT(cnt->last_host_notification);
    MYSQL_BIND_INT(cnt->last_service_notification);
    MYSQL_BIND_INT(cnt->modified_attributes);
    MYSQL_BIND_INT(cnt->modified_host_attributes);
    MYSQL_BIND_INT(cnt->modified_service_attributes);

    MYSQL_BIND();
    MYSQL_EXECUTE();
}


int ndo_handle_notification(int type, void * d)
{
    nebstruct_notification_data * data = d;
    int object_id = 0;

    if (data->notification_type == SERVICE_NOTIFICATION) {
        object_id = ndo_get_object_id_name2(TRUE, NDO_OBJECTTYPE_SERVICE, data->host_name, data->service_description);
    }
    else {
        object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_HOST, data->host_name);
    }

    MYSQL_RESET_SQL();
    MYSQL_RESET_BIND();

    MYSQL_SET_SQL("INSERT INTO nagios_notifications (instance_id, start_time, start_time_usec, end_time, end_time_usec, notification_type, notification_reason, object_id, state, output, long_output, escalated, contacts_notified) VALUES (1,FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),?,?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), start_time = VALUES(start_time), start_time_usec = VALUES(start_time_usec), end_time = VALUES(end_time), end_time_usec = VALUES(end_time_usec), notification_type = VALUES(notification_type), notification_reason = VALUES(notification_reason), object_id = VALUES(object_id), state = VALUES(state), output = VALUES(output), long_output = VALUES(long_output), escalated = VALUES(escalated), contacts_notified = VALUES(contacts_notified)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(data->start_time.tv_sec);
    MYSQL_BIND_INT(data->start_time.tv_usec);
    MYSQL_BIND_INT(data->end_time.tv_sec);
    MYSQL_BIND_INT(data->end_time.tv_usec);
    MYSQL_BIND_INT(data->notification_type);
    MYSQL_BIND_INT(data->reason_type);
    MYSQL_BIND_INT(object_id);
    MYSQL_BIND_INT(data->state);
    MYSQL_BIND_STR(data->output);
    MYSQL_BIND_STR(data->output);
    MYSQL_BIND_INT(data->escalated);
    MYSQL_BIND_INT(data->contacts_notified);

    MYSQL_BIND();
    MYSQL_EXECUTE();

    /* because of the way we've changed ndo, we can no longer keep passing this
       around inside the database object.

       we can be sure that this will be the appropriate id, as the brokered
       event data for notification happens directly before each contact is
       then notified (and that data also brokered) */
    ndo_last_notification_id = mysql_insert_id(mysql_connection);

    return NDO_OK;
}


int ndo_handle_contact_notification(int type, void * d)
{
    nebstruct_contact_notification_data * data = d;

    int contact_object_id     = 0;
    contact * cnt             = NULL;

    if (data->contact_ptr == NULL) {
        NDO_REPORT_ERROR("Broker data pointer is null");
        return NDO_OK;
    }

    cnt = data->contact_ptr;

    contact_object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_CONTACT, cnt->name);

    MYSQL_RESET_BIND();
    MYSQL_RESET_SQL();

    MYSQL_SET_SQL("INSERT INTO nagios_contactnotifications (instance_id, notification_id, start_time, start_time_usec, end_time, end_time_usec, contact_object_id) VALUES (1,?,FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), notification_id = VALUES(notification_id), start_time = VALUES(start_time), start_time_usec = VALUES(start_time_usec), end_time = VALUES(end_time), end_time_usec = VALUES(end_time_usec), contact_object_id = VALUES(contact_object_id)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(ndo_last_notification_id);
    MYSQL_BIND_INT(data->start_time.tv_sec);
    MYSQL_BIND_INT(data->start_time.tv_usec);
    MYSQL_BIND_INT(data->end_time.tv_sec);
    MYSQL_BIND_INT(data->end_time.tv_usec);
    MYSQL_BIND_INT(contact_object_id);

    /* see notes regarding `ndo_last_notification_id` - those apply here too */
    ndo_last_contact_notification_id = mysql_insert_id(mysql_connection);

    MYSQL_BIND();
    MYSQL_EXECUTE();
}


int ndo_handle_contact_notification_method(int type, void * d)
{
    nebstruct_contact_notification_method_data * data = d;

    int command_object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_COMMAND, data->command_name);

    MYSQL_RESET_BIND();
    MYSQL_RESET_SQL();

    MYSQL_SET_SQL("INSERT INTO nagios_contactnotificationmethods (instance_id, contactnotification_id, start_time, start_time_usec, end_time, end_time_usec, command_object_id, command_args) VALUES (1,?,FROM_UNIXTIME(?),?,FROM_UNIXTIME(?),?,?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), contactnotification_id = VALUES(contactnotification_id), start_time = VALUES(start_time), start_time_usec = VALUES(start_time_usec), end_time = VALUES(end_time), end_time_usec = VALUES(end_time_usec), command_object_id = VALUES(command_object_id), command_args = VALUES(command_args)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(ndo_last_contact_notification_id);
    MYSQL_BIND_INT(data->start_time.tv_sec);
    MYSQL_BIND_INT(data->start_time.tv_usec);
    MYSQL_BIND_INT(data->end_time.tv_sec);
    MYSQL_BIND_INT(data->end_time.tv_usec);
    MYSQL_BIND_INT(command_object_id);
    MYSQL_BIND_STR(data->command_args);

    MYSQL_BIND();
    MYSQL_EXECUTE();
}


int ndo_handle_external_command(int type, void * d)
{
    nebstruct_external_command_data * data = d;

    if (data->type != NEBTYPE_EXTERNALCOMMAND_START) {
        return NDO_OK;
    }

    MYSQL_RESET_BIND();
    MYSQL_RESET_SQL();

    MYSQL_SET_SQL("INSERT INTO nagios_externalcommands (instance_id, command_type, entry_time, command_name, command_args) VALUES (1,?,FROM_UNIXTIME(?),?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), command_type = VALUES(command_type), entry_time = VALUES(entry_time), command_name = VALUES(command_name), command_args = VALUES(command_args)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(data->command_type);
    MYSQL_BIND_INT(data->entry_time);
    MYSQL_BIND_STR(data->command_string);
    MYSQL_BIND_STR(data->command_args);

    MYSQL_BIND();
    MYSQL_EXECUTE();
}


int ndo_handle_acknowledgement(int type, void * d)
{
    nebstruct_acknowledgement_data * data = d;

    int object_id = 0;

    if (data->acknowledgement_type == SERVICE_ACKNOWLEDGEMENT) {
        object_id = ndo_get_object_id_name2(TRUE, NDO_OBJECTTYPE_SERVICE, data->host_name, data->service_description);
    }
    else {
        object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_HOST, data->host_name);
    }

    MYSQL_RESET_BIND();
    MYSQL_RESET_SQL();

    MYSQL_SET_SQL("INSERT INTO nagios_acknowledgements (instance_id, entry_time, entry_time_usec, acknowledgement_type, object_id, state, author_name, comment_data, is_sticky, persistent_comment, notify_contacts) VALUES (1,FROM_UNIXTIME(?),?,?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE instance_id = VALUES(instance_id), entry_time = VALUES(entry_time), entry_time_usec = VALUES(entry_time_usec), acknowledgement_type = VALUES(acknowledgement_type), object_id = VALUES(object_id), state = VALUES(state), author_name = VALUES(author_name), comment_data = VALUES(comment_data), is_sticky = VALUES(is_sticky), persistent_comment = VALUES(persistent_comment), notify_contacts = VALUES(notify_contacts)");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(data->timestamp.tv_sec);
    MYSQL_BIND_INT(data->timestamp.tv_usec);
    MYSQL_BIND_INT(data->acknowledgement_type);
    MYSQL_BIND_INT(object_id);
    MYSQL_BIND_INT(data->state);
    MYSQL_BIND_STR(data->author_name);
    MYSQL_BIND_STR(data->comment_data);
    MYSQL_BIND_INT(data->is_sticky);
    MYSQL_BIND_INT(data->persistent_comment);
    MYSQL_BIND_INT(data->notify_contacts);

    MYSQL_BIND();
    MYSQL_EXECUTE();
}


int ndo_handle_state_change(int type, void * d)
{
    nebstruct_statechange_data * data = d;

    int object_id = 0;
    int last_state = 0;
    int last_hard_state = 0;

    if (data->type != NEBTYPE_STATECHANGE_END) {
        return NDO_OK;
    }

    if (data->statechange_type == SERVICE_STATECHANGE) {

        service * svc = data->object_ptr;

        object_id = ndo_get_object_id_name2(TRUE, NDO_OBJECTTYPE_SERVICE, svc->host_name, svc->description);

        last_state = svc->last_state;
        last_hard_state = svc->last_hard_state;
    }
    else {

        host * hst = data->object_ptr;

        object_id = ndo_get_object_id_name1(TRUE, NDO_OBJECTTYPE_HOST, hst->name);

        last_state = hst->last_state;
        last_hard_state = hst->last_hard_state;
    }

    MYSQL_RESET_BIND();
    MYSQL_RESET_SQL();

    MYSQL_SET_SQL("INSERT INTO nagios_statehistory SET instance_id = 1, state_time = FROM_UNIXTIME(?), state_time_usec = ?, object_id = ?, state_change = 1, state = ?, state_type = ?, current_check_attempt = ?, max_check_attempts = ?, last_state = ?, last_hard_state = ?, output = ?, long_output = ?");
    MYSQL_PREPARE();

    MYSQL_BIND_INT(data->timestamp.tv_sec);
    MYSQL_BIND_INT(data->timestamp.tv_usec);
    MYSQL_BIND_INT(object_id);
    MYSQL_BIND_INT(data->state);
    MYSQL_BIND_INT(data->state_type);
    MYSQL_BIND_INT(data->current_attempt);
    MYSQL_BIND_INT(data->max_attempts);
    MYSQL_BIND_INT(last_state);
    MYSQL_BIND_INT(last_hard_state);
    MYSQL_BIND_STR(data->output);
    MYSQL_BIND_STR(data->longoutput);

    MYSQL_BIND();
    MYSQL_EXECUTE();
}


int ndo_handle_retention(int type, void * d)
{
    nebstruct_retention_data * data = d;

    if (data->type == NEBTYPE_RETENTIONDATA_ENDLOAD) {
        ndo_write_config(NDO_CONFIG_DUMP_RETAINED);
    }

    return NDO_OK;
}
