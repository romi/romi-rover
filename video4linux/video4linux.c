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
#include <romi.h>
#include "camera_v4l.h"
#include "video4linux.h"

static const char *device = "/dev/video0";
static camera_t* camera = NULL;
static membuf_t *jpegbuf = NULL;
static int width = 1920;
static int height = 1080;

typedef struct _image_request_t {
        response_t *response;
        condition_t *condition;
        double timestamp;
} image_request_t;

#define REQUEST_BUFFER_SIZE 8

static mutex_t *request_mutex = NULL;
static image_request_t *request_buffer[REQUEST_BUFFER_SIZE];
static int request_buffer_index = 0;

streamer_t *get_streamer_camera();

static void broadcast_log(void *userdata, const char* s)
{
        messagelink_t *get_messagelink_logger();
        messagelink_t *log = get_messagelink_logger();
        if (log)
                messagelink_send_str(log, s);
}

image_request_t *new_image_request(response_t *response)
{
        image_request_t *r = r_new(image_request_t);
        if (r == NULL)
                return NULL;
        r->timestamp = 0.0;
        r->response = response;
        r->condition = new_condition();
        if (r->condition == NULL) {
                r_delete(r);
                return NULL;
        }
        return r;
}

void delete_image_request(image_request_t *r)
{
        if (r) {
                if (r->condition) 
                        delete_condition(r->condition);
                r_delete(r);
        }
}

int add_image_request(image_request_t *r)
{
        int err = 0;
        
        mutex_lock(request_mutex);
        if (request_buffer_index < REQUEST_BUFFER_SIZE) 
                request_buffer[request_buffer_index++] = r;
        else
                err = -1;
        mutex_unlock(request_mutex);

        return err;
}

int wait_image_request(image_request_t *r)
{
        membuf_t *buf = response_body(r->response);
        membuf_lock(buf);
        while (r->timestamp == 0)
                condition_wait(r->condition, membuf_mutex(buf));
        membuf_unlock(buf);
        return 0;
}

int video4linux_init(int argc, char **argv)
{
        r_log_set_writer(broadcast_log, NULL);
        
        if (argc == 2)
                device = argv[1];
        
        request_mutex = new_mutex();
        if (request_mutex == NULL)
                return -1;
        
        jpegbuf = new_membuf();
        if (jpegbuf == NULL || membuf_assure(jpegbuf, 3 * width * height) != 0)
                return -1;
        
        camera = new_camera(device, IO_METHOD_MMAP, width, height);
        if (camera == NULL) {
                r_err("Failed to open the camera");
                return -1;
        }
        return 0;
}

void video4linux_cleanup()
{
        if (camera)
                delete_camera(camera);
        if (request_mutex)
                delete_mutex(request_mutex);
        if (jpegbuf)
                delete_membuf(jpegbuf);
}

void video4linux_broadcast()
{
        streamer_t *streamer = get_streamer_camera();
        int image_requested = 0;
        int stream_requested = 0;

        int error = camera_capture(camera);
        if (error) {
                r_err("Failed to grab the image");
                clock_sleep(0.04);
                return;
        }
        double timestamp = clock_time();
        
        mutex_lock(request_mutex);
        image_requested = (request_buffer_index > 0);
        mutex_unlock(request_mutex);
        
        streamer_lock_clients(streamer);
        stream_requested = streamer_has_clients(streamer);
        streamer_unlock_clients(streamer);
        
        if (image_requested || stream_requested) {
                convert_to_jpeg(camera_getimagebuffer(camera),
                                camera_width(camera),
                                camera_height(camera),
                                90, jpegbuf);
        } else {
                clock_sleep(0.2);
        }
        
        if (stream_requested) {
                //r_debug("timestamp %f", timestamp);
                //r_debug("image length %d", membuf_len(jpegbuf));
                streamer_send_multipart(streamer, membuf_data(jpegbuf),
                                        membuf_len(jpegbuf),
                                        "image/jpeg", timestamp);
        }
        
        if (image_requested) {
                mutex_lock(request_mutex);
                for (int i = 0; i < request_buffer_index; i++) {
                        image_request_t *r = request_buffer[i];
                        membuf_t *buf = response_body(r->response);
                        
                        membuf_lock(buf);
                        membuf_clear(buf);
                        membuf_append(buf, membuf_data(jpegbuf), membuf_len(jpegbuf));
                        r->timestamp = timestamp;
                        condition_signal(r->condition);
                        membuf_unlock(buf);
                }
                request_buffer_index = 0;
                mutex_unlock(request_mutex);
        }
}

void video4linux_still(void *data, request_t *request, response_t *response)
{
        image_request_t *r = new_image_request(response);
        if (r == NULL) {
                response_set_status(response, HTTP_Status_Internal_Server_Error);
                return;
        }
        
        int err = add_image_request(r);
        if (err != 0) {
                response_set_status(response, HTTP_Status_Too_Many_Requests);
                goto cleanup;
        }

        err = wait_image_request(r);
        if (r == NULL) {
                response_set_status(response, HTTP_Status_Internal_Server_Error);
                goto cleanup;
        }

cleanup:
        delete_image_request(r);
}
