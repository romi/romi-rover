/*
  romi-rover

  Copyright (C) 2019 Sony Computer Science Laboratories
  Author(s) Peter Hanappe

  romi-rover is collection of applications for the Romi Rover.

  romi-rover is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see
  <http://www.gnu.org/licenses/>.

 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>

#include <romi.h>

#include "interface.h"

typedef struct _script_t script_t;

static int interface_initialized = 0;
static list_t *scripts = NULL;
static char *server_dir = NULL;
static char *script_path = NULL;
static int idle = 1;
static int recording = 0;
static int progress = 0;
static const char *status = "idle";
static script_t *current_script = NULL;
static double position = 0.0;
static mutex_t *mutex;
thread_t *thread = NULL;

service_t *get_service_interface();
messagelink_t *get_messagelink_motorcontroller();
messagelink_t *get_messagelink_weeder();
messagelink_t *get_messagelink_camera_recorder();
messagelink_t *get_messagelink_navigation();
messagelink_t *get_messagelink_watchdog();

json_object_t global_env;

static void broadcast_log(void *userdata, const char* s)
{
        messagelink_t *get_messagelink_logger();
        static int _inside_log_writer = 0;
        messagelink_t *log = get_messagelink_logger();
        if (log != NULL && _inside_log_writer == 0) {
                _inside_log_writer = 1;
                messagelink_send_str(log, s);
                _inside_log_writer = 0;
        }
}

////////////////////////////////////////////////////////

static int status_error(json_object_t reply, membuf_t *message)
{
        int ret = -1;
        const char *status = json_object_getstr(reply, "status");
        if (status == NULL) {
                r_err("invalid status");
                membuf_printf(message, "invalid status");
                
        } else if (rstreq(status, "ok")) {
                ret = 0;
                
        } else if (rstreq(status, "error")) {
                const char *s = json_object_getstr(reply, "message");
                r_err("message error: %s", s);
                membuf_printf(message, "%s", s);
        }
        json_unref(reply);
        return ret;
}

static int wait(membuf_t *message)
{
        r_debug("Waiting 1 sec");
        clock_sleep(2);
        return 0;
}

static int homing(membuf_t *message)
{
        messagelink_t *motors = get_messagelink_motorcontroller();
        json_object_t reply;
        int err;
        
        r_debug("Sending homing");

        if (app_standalone()) {
                clock_sleep(1);
                return 0;
        }
        
        reply = messagelink_send_command_f(motors, "{'command':'homing'}");
        return status_error(reply, message);
}

static int moveat(membuf_t *message, int speed)
{
        messagelink_t *motors = get_messagelink_motorcontroller();
        json_object_t reply;

        r_debug("Sending moveat %d", speed);

        if (app_standalone()) {
                clock_sleep(1);
                return 0;
        }
        
        reply = messagelink_send_command_f(motors, "{'command':'moveat','speed':%d}",
                                           speed);
        return status_error(reply, message);
}

static int move(membuf_t *message, double distance, double speed)
{
        messagelink_t *navigation = get_messagelink_navigation();
        json_object_t reply;

        r_debug("Sending move %f", distance);

        if (app_standalone()) {
                clock_sleep(1);
                return 0;
        }
        
        reply = messagelink_send_command_f(navigation,
                                  "{'command':'move',"
                                  "'distance':%f,"
                                  "'speed':%f}",
                                  distance, speed);
        return status_error(reply, message);
}

int32 _iterator(const char* key, json_object_t value, json_object_t request)
{
        if (rstreq(key, "action"))
                return 0;
        json_object_set(request, key, value);
        return 0;
}

static int hoe(membuf_t *message, const char *method, json_object_t action)
{
        messagelink_t *weeder = get_messagelink_weeder();
        json_object_t reply;

        json_object_t request = json_object_create();;
        json_object_setstr(request, "command", "hoe");
        // Copy the parameters
        json_object_foreach(action, (json_iterator_t) _iterator, request);
        
        r_debug("Sending hoe %s", method);
        
        reply = messagelink_send_command(weeder, request);
        json_unref(request);
        
        return status_error(reply, message);
}

static int start_recording(membuf_t *message)
{
        messagelink_t *recorder = get_messagelink_camera_recorder();
        json_object_t reply;
        
        r_debug("Sending start_recording");

        if (app_standalone()) {
                clock_sleep(1);
                return 0;
        }
        
        reply = messagelink_send_command_f(recorder, "{'command':'start'}");

        int err = status_error(reply, message);
        if (err = 0) recording = 1;
        
        return err;
}
                  
static int stop_recording(membuf_t *message)
{
        messagelink_t *recorder = get_messagelink_camera_recorder();
        json_object_t reply;
        
        r_debug("Sending stop_recording");

        if (app_standalone()) {
                clock_sleep(1);
                return 0;
        }
        
        reply = messagelink_send_command_f(recorder, "{'command':'stop'}");
        int err = status_error(reply, message);
        if (err = 0)
                recording = 0;
        
        return err;
}

static int shutdown_rover(membuf_t *message)
{
        messagelink_t *watchdog = get_messagelink_watchdog();
        json_object_t reply;

        r_debug("Sending shutdown");

        if (app_standalone()) {
                clock_sleep(1);
                return 0;
        }
        
        reply = messagelink_send_command_f(watchdog, "{'command':'shutdown'}");
        return status_error(reply, message);
}

////////////////////////////////////////////////////////

typedef struct _script_t {
        char *name;
        char *display_name;
        json_object_t actions;
} script_t;

static void delete_script(script_t *script);

static script_t *new_script(const char *name,
                            const char *display_name,
                            json_object_t actions)
{
        script_t *script = r_new(script_t);
        script->actions = json_null();
        script->name = r_strdup(name);
        script->display_name = r_strdup(display_name);
        if (script->name == NULL || script->display_name == NULL) {
                delete_script(script);
                return NULL;
        }
        script->actions = actions;
        json_ref(actions);
        return script;
}

static void delete_script(script_t *script)
{
        if (script) {
                if (script->name) r_free(script->name);
                if (script->display_name) r_free(script->display_name);
                json_unref(script->actions);
                r_delete(script);
        }
}

static void delete_script_list(list_t *list)
{
        list_t *l = list;
        script_t *script;
        while (l) {
                script = list_get(l, script_t);
                delete_script(script);
                l = list_next(l);
        }
        delete_list(list);
}

static script_t *find_script(list_t *list, const char *name)
{
        while (list) {
                script_t *script = list_get(list, script_t);
                if (rstreq(name, script->name))
                        return script;
                list = list_next(list);
        }
        return NULL;
}

static int do_action(const char *name, json_object_t action,
                     list_t *environments, membuf_t *message)
{
        int err;

        if (rstreq(name, "homing")) {
                err = homing(message);

        } else if (rstreq(name, "start_recording")) {
                err = start_recording(message);

        } else if (rstreq(name, "wait")) {
                err = wait(message);

        } else if (rstreq(name, "stop_recording")) {
                err = stop_recording(message);

        } else if (rstreq(name, "shutdown")) {
                err = shutdown_rover(message);

        } else if (rstreq(name, "move")) {
                double distance = json_object_getnum(action, "distance");
                if (isnan(distance)) {
                        r_err("Action 'move': invalid distance");
                        return -1;
                }
                double speed = 100.0;
                if (json_object_has(action, "speed")) {
                        speed = json_object_getnum(action, "speed");
                        if (isnan(speed) || speed < -1000 || speed > 1000) {
                                r_err("Action 'move': invalid distance");
                                return -1;
                        }
                }
                err = move(message, distance, speed);
                
        } else if (rstreq(name, "moveat")) {
                double speed = json_object_getnum(action, "speed");
                if (isnan(speed) || speed < -1000 || speed > 1000) {
                        r_err("Action 'moveat': invalid speed");
                        return -1;
                }
                err = moveat(message, (int) speed);
                
        } else if (rstreq(name, "hoe")) {
                const char *method = json_object_getstr(action, "method");
                if (method == NULL) {
                        r_err("Action 'hoe': invalid method");
                        return -1;
                }
                if (!rstreq(method, "quincunx") && !rstreq(method, "boustrophedon")) {
                        r_err("Action 'hoe': invalid method");
                        return -1;
                }
                err = hoe(message, method, action);
        } else {
                r_err("Unknown action: '%s'", name);
                membuf_printf(message, "Unknown action: '%s'", name);
                err = -1;
        }

        /* json_unref(action); */
        return err;
}

static int execute(json_object_t actions, list_t *environments, membuf_t *message)
{
        int err = 0;

        for (int i = 0; i < json_array_length(actions); i++) {
                
                json_object_t action = json_array_get(actions, i);
                
                const char *action_name = json_object_getstr(action, "action");
                if (action_name == NULL) {
                        membuf_printf(message, "script step has no action field");
                        err = -1;
                        break;
                }
                
                status = action_name; 
                r_debug("action: %s", action_name);
                
                do_action(action_name, action, environments, message);
        }

        return err;
}

void _run_script(script_t *script)
{
        r_debug("_run_script");

        membuf_t *message = new_membuf();
        
        mutex_lock(mutex);
        progress = 0;
        status = "idle";
        current_script = script;
        mutex_unlock(mutex);

        list_t *environments = list_prepend(NULL, global_env);

        if (execute(script->actions, environments, message) != 0) {
                membuf_append_zero(message);
                r_err("Script '%s' returned an error: %s",
                        script->name, membuf_data(message));
                moveat(message, 0); // FIXME: raise alert to robot!
        }

        delete_list(environments);
        delete_membuf(message);
                
        mutex_lock(mutex);
        idle = 1;
        progress = 100;
        status = "idle";
        current_script = NULL;
        delete_thread(thread);
        thread = NULL;
        mutex_unlock(mutex);
}

int run_script(script_t *script)
{
        int err = 0;
        r_debug("run_script");
        mutex_lock(mutex);
        if (thread == NULL) {
                r_debug("run_script: creating new thread");
                thread = new_thread((thread_run_t) _run_script, script, 0, 0);
                if (thread == NULL) 
                        err = -1;
        } else {
                err = -2;
        }
        mutex_unlock(mutex);
        return err;
}

static list_t *load_script_file(const char *filename)
{
        int err;
        char errmsg[256];
        json_object_t scripts;
        list_t *list = NULL;

        r_debug("load_script_file %s", filename);
        
        scripts = json_load(filename, &err, errmsg, sizeof(errmsg));
        if (err != 0) {
                r_err("Failed to load scripts file: %s", errmsg);
                r_err("File: '%s'", filename);
                json_unref(scripts);
                return NULL;
        }
        if (!json_isarray(scripts)) {
                r_err("Scripts file doesn't contain an array. File: '%s'", filename);
                json_unref(scripts);
                return NULL;
        }

        for (int i = 0; i < json_array_length(scripts); i++) {
                const char* name;
                const char* display_name;
                json_object_t script;
                json_object_t actions;

                script = json_array_get(scripts, i);
                if (!json_isobject(script)) {
                        r_err("Script is not an object. File: '%s'", filename);
                        json_unref(scripts);
                        delete_script_list(list);
                        return NULL;
                }

                name = json_object_getstr(script, "name");
                display_name = json_object_getstr(script, "display_name");
                actions = json_object_get(script, "script");
                if (name == NULL
                    || display_name == NULL
                    || !json_isarray(actions)) {
                        if (name == NULL) 
                                r_err("Script %i has no name. File: '%s'", i, filename);
                        else if (display_name == NULL) 
                                r_err("Script %i has no disply name. File: '%s'",
                                        i, filename);
                        else r_err("Script %i: script filed is not an array. File: '%s'",
                                     i, filename);
                        json_unref(scripts);
                        delete_script_list(list);
                        return NULL;
                }
                r_debug("Adding script '%s'", name);
                script_t *s = new_script(name, display_name, actions);
                list = list_append(list, s);
        }
        
        json_unref(scripts);
        return list;
}

////////////////////////////////////////////////////////

static int set_server_dir(const char *path)
{
        char initial_path[PATH_MAX];
        char resolved_path[PATH_MAX];
        struct stat statbuf;

        r_debug("set_server_dir: %s", path);

        // FIXME!
        if (path[0] != '/') {
                char cwd[PATH_MAX];
                if (getcwd(cwd, PATH_MAX) == NULL) {
                        char reason[200];
                        strerror_r(errno, reason, 200);
                        r_err("getcwd failed: %s", reason);
                        return -1;
                }
                snprintf(initial_path, PATH_MAX, "%s/%s", cwd, path);
                initial_path[PATH_MAX-1] = 0;
        } else {
                snprintf(initial_path, PATH_MAX, "%s", path);
                initial_path[PATH_MAX-1] = 0;
        }

        if (realpath(initial_path, resolved_path) == NULL) {
                char reason[200];
                strerror_r(errno, reason, 200);
                r_err("realpath failed: %s", reason);
                r_err("path: %s", initial_path);
                return -1;
        }
        if (stat(resolved_path, &statbuf) != 0) {
                char reason[200];
                strerror_r(errno, reason, 200);
                r_err("stat failed: %s", reason);
                return -1;
        }
        if ((statbuf.st_mode & S_IFMT) != S_IFDIR) {
                r_err("Not a directory: %s", resolved_path);
                return -1;
        }
        
        server_dir = r_strdup(resolved_path);
        r_info("Serving files from directory %s", server_dir);
        return 0;
}

static int set_script_path(const char *path)
{
        if (script_path != NULL) {
                r_free(script_path);
                script_path = NULL;
        }
        script_path = r_strdup(path);
        if (script_path == NULL)
                return -1;

        r_info("Script file: %s", path);
        return 0;
}

static int get_configuration()
{
        if (server_dir != NULL && script_path != NULL)
                return 0;
        
        r_debug("trying to configure the interface");

        global_env = client_get("configuration", "");
        if (json_falsy(global_env)) {
                r_err("failed to load the configuration");
                return -1;
        }
        
        json_object_t config = json_object_get(global_env, "interface");

        if (!json_isobject(config)) {
                r_err("failed to load the interface configuration");
                json_unref(config);
                return -1;
        }
        
        const char *html = json_object_getstr(config, "html");
        const char *script_file = json_object_getstr(config, "scripts");
        if (html == NULL || script_file == NULL) {
                r_err("invalid configuration");
                json_unref(config);
                return -1;
        }

        if (set_server_dir(html) != 0) {
                json_unref(config);
                return -1;
        }

        if (set_script_path(script_file) != 0) {
                json_unref(config);
                return -1;
        }
                        
        json_unref(config);
        return 0;
}

static int init()
{
        if (interface_initialized)
                return 0;
        
        if (get_configuration() != 0)
                return -1;
        
        scripts = load_script_file(script_path);
        if (scripts == NULL)
                return -1;
        
        interface_initialized = 1;
        return 0;
}

int interface_init(int argc, char **argv)
{
        r_log_set_writer(broadcast_log, NULL);
        
        mutex = new_mutex();
        if (mutex == NULL)
                return -1;
        
        if (argc == 3) {
                if (set_server_dir(argv[1]) != 0)
                        return -1;
                if (set_script_path(argv[2]) != 0)
                        return -1;
        }
        
        for (int i = 0; i < 10; i++) {
                if (init() == 0)
                        return 0;
                r_err("init failed: attempt %d/10", i);
                clock_sleep(0.2);
        }

        r_err("failed to initialize the interface");        
        return -1;
}

void interface_cleanup()
{
        if (server_dir)
                r_free(server_dir);
        if (script_path)
                r_free(script_path);
        if (scripts) {
                for (list_t *l = scripts; l != NULL; l = list_next(l)) {
                        script_t *script = list_get(l, script_t);
                        delete_script(script);
                }
                delete_list(scripts);
        }
        if (mutex != NULL)
                delete_mutex(mutex);
        json_unref(global_env);
}

/////////////////////////////////////////////////////

static void send_file(const char *path, const char *mimetype,
                      request_t *request, response_t *response)
{
        FILE *fp = fopen(path, "r");
        if (fp == NULL) {
                r_err("Failed to open %s", path);
                response_set_status(response, HTTP_Status_Not_Found);
                return;
        }

        response_set_mimetype(response, mimetype);

        while (1) {
                size_t num;
                char buffer[256];
                num = fread(buffer, 1, 256, fp);
                response_append(response, buffer, num);
                if (feof(fp)) break;
                if (ferror(fp)) {
                        r_err("Failed to read %s", path);
                        fclose(fp);
                        response_set_status(response, HTTP_Status_Internal_Server_Error);
                        return;
                }
        }
        fclose(fp);
}

static int check_path(const char *filename, char *path, int len)
{
        char requested_path[PATH_MAX];
        char resolved_path[PATH_MAX];
        struct stat statbuf;
        
        snprintf(requested_path, PATH_MAX, "%s/%s", server_dir, filename);
        requested_path[PATH_MAX-1] = 0;

        if (realpath(requested_path, resolved_path) == NULL) {
                char reason[200];
                strerror_r(errno, reason, 200);
                r_err("realpath failed: %s", reason);
                r_err("path: %s", requested_path);
                return -1;
        }
        if (strncmp(server_dir, resolved_path, strlen(server_dir)) != 0) {
                r_err("File not in server path: %s", resolved_path);
                return -1;
        }
        if (stat(resolved_path, &statbuf) != 0) {
                char reason[200];
                strerror_r(errno, reason, 200);
                r_err("stat failed: %s", reason);
                return -1;
        }
        if ((statbuf.st_mode & S_IFMT) != S_IFREG) {
                r_err("Not a regular file: %s", resolved_path);
                return -1;
        }
        snprintf(path, len, "%s", resolved_path);
        return 0;
}

void send_local_file(const char *filename, request_t *request, response_t *response)
{
        char path[PATH_MAX];
        
        const char *mimetype = filename_to_mimetype(filename);
        if (mimetype == NULL) {
                response_set_status(response, HTTP_Status_Bad_Request);
                return;
        }
        
        if (check_path(filename, path, PATH_MAX) != 0) {
                response_set_status(response, HTTP_Status_Bad_Request);
                return;
        }
        send_file(path, mimetype, request, response);
}

/////////////////////////////////////////////////////

void interface_local_file(void *data, request_t *request, response_t *response)
{
        if (init() != 0) {
                response_set_status(response, HTTP_Status_Internal_Server_Error);
                return;
        }

	const char *filename = request_uri(request);
        
        if (rstreq(filename, "/"))
                filename = "index.html";
        else if (filename[0] == '/')
                filename++;
        
        send_local_file(filename, request, response);
}

void interface_index(void *data, request_t *request, response_t *response)
{
        if (init() != 0) {
                response_set_status(response, HTTP_Status_Internal_Server_Error);
                return;
        }
        send_local_file("index.html", request, response);
}

void interface_scripts(void *data, request_t *request, response_t *response)
{
        if (init() != 0) {
                response_set_status(response, HTTP_Status_Internal_Server_Error);
                return;
        }

        response_printf(response, "[");
        
        for (list_t *l = scripts; l != NULL; l = list_next(l)) {
                script_t *script = list_get(l, script_t);
                response_printf(response,
                                     "{\"name\":\"%s\",\"display_name\":\"%s\"}",
                                     script->name, script->display_name);
                ;
                if (list_next(l) != NULL) 
                        response_printf(response, ",");
        }

        response_printf(response, "]");
}

void interface_status(void *data, request_t *request, response_t *response)
{
        response_printf(response,
                             "{\"status\": \"%s\", "
                             "\"position\": %f, "
                             "\"progress\": %d, "
                             "\"recording\": %d",
                             status, position, progress, recording);

        if (current_script != NULL) {
                response_printf(response,
                                     ", "
                                     "\"script\": {\"name\":\"%s\", "
                                     "\"display_name\": \"%s\"}}",
                                     current_script->name,
                                     current_script->display_name);
        } else {
                response_printf(response, "}");
        }
}

void interface_execute(void *data, request_t *request, response_t *response)
{
        if (init() != 0) {
                response_set_status(response, HTTP_Status_Internal_Server_Error);
                return;
        }

        const char *arg = request_args(request);
        if (arg == NULL) {
                response_set_status(response, HTTP_Status_Bad_Request);
                return;
        }

        r_debug("checking for script '%s'", arg);

        script_t *script = find_script(scripts, arg);
        if (script == NULL) {
                response_set_status(response, HTTP_Status_Not_Found);
                return;
        }

        int r = run_script(script);
        if (r == -1) {
                response_set_status(response, HTTP_Status_Internal_Server_Error);
                return;
        }

        send_local_file("script.html", request, response);
}
