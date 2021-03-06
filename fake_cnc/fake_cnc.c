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
#include "fake_cnc.h"

static mutex_t *mutex = NULL;
static double _x = 0.0;
static double _y = 0.0;
static double _z = 0.0;

static void broadcast_log(void *userdata, const char* s)
{
        messagelink_t *get_messagelink_logger();
        messagelink_t *log = get_messagelink_logger();
        if (log)
                messagelink_send_str(log, s);
}

int fake_cnc_init(int argc, char **argv)
{
        r_log_set_writer(broadcast_log, NULL);
        mutex = new_mutex();
        return 0;
}

void fake_cnc_cleanup()
{
        delete_mutex(mutex);
}

int cnc_onmoveto(void *userdata,
                 messagelink_t *link,
                 json_object_t command,
                 membuf_t *message)
{
	r_debug("cnc_onmoveto");

        const char *r;
        int hasx = json_object_has(command, "x");
        int hasy = json_object_has(command, "y");
        int hasz = json_object_has(command, "z");
        double x = 0.0, y = 0.0, z = 0.0;
        
        if (!hasx && !hasy && !hasz) {
                membuf_printf(message, "No coordinates given");
                return -1;
        }
        if (hasx) x = json_object_getnum(command, "x");
        if (hasy) y = json_object_getnum(command, "y");
        if (hasz) z = json_object_getnum(command, "z");
        if (isnan(x) || isnan(y) || isnan(z)) {
                membuf_printf(message, "Invalid coordinates");
                return -1;
        }

        mutex_lock(mutex);        
        if (hasx) _x = x;
        if (hasy) _y = y;
        if (hasz) _z = z;
	r_debug("position[%.4f,%.4f,%.4f]", _x, _y, _z);
        mutex_unlock(mutex);
        
        return 0;
}

int cnc_ontravel(void *userdata,
                 messagelink_t *link,
                 json_object_t command,
                 membuf_t *message)
{
	r_debug("cnc_ontravel");
        
        json_object_t path = json_object_get(command, "path");
        if (!json_isarray(path)) {
                membuf_printf(message, "Expected an array for the path");
                return -1;
        }

        // Check the path
        for (int i = 0; i < json_array_length(path); i++) {
                json_object_t p = json_array_get(path, i);
                if (!json_isarray(p) || json_array_length(p) < 3) {
                        membuf_printf(message, "Point %d is not a valid", i);
                        return -1;
                }
                for (int j = 0; j < 3; j++) {
                        json_object_t v;
                        v = json_array_get(p, j);
                        if (!json_isnumber(v)) {
                                membuf_printf(message, 
                                              "Coordinate (%d,%d) is not a valid", i, j);
                                return -1;
                        }
                        // TODO: check against CNC dimensions
                }
        }

        mutex_lock(mutex);        
        
        for (int i = 0; i < json_array_length(path); i++) {
                json_object_t p = json_array_get(path, i);
                _x = json_array_getnum(p, 0);
                _y = json_array_getnum(p, 1);
                _z = json_array_getnum(p, 2);
                r_debug("position[%.4f,%.4f,%.4f]", _x, _y, _z);
                clock_sleep(0.2);
        }
        
        mutex_unlock(mutex);        

        return 0;
}

int cnc_onspindle(void *userdata,
                  messagelink_t *link,
                  json_object_t command,
                  membuf_t *message)
{
	r_debug("cnc_onspindle");
        return 0;
}

int cnc_onhoming(void *userdata,
                 messagelink_t *link,
                 json_object_t command,
                 membuf_t *message)
{
	r_debug("cnc_onhoming");
        return 0;
}
