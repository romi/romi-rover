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
#include <stdlib.h>
#include <unistd.h>
#include "webproxy.h"

void broadcast_log(void *userdata, const char* s)
{
        messagelink_t *get_messagelink_logger();
        messagelink_t *log = get_messagelink_logger();
        if (log)
                messagelink_send_str(log, s);
}

int webproxy_init(int argc, char **argv)
{
        r_log_set_writer(broadcast_log, NULL);
        return 0;
}

void webproxy_cleanup()
{
}

/***************************************************************************/

void webproxy_get_service(request_t *request,
                          const char *topic,
                          const char *resource,
                          response_t *response)
{
        int err;

        if (request_args(request) != NULL) {
                char buffer[1024]; 
                rprintf(buffer, sizeof(buffer), "%s?%s", resource, request_args(request));
                err = client_get_data(topic, buffer, &response);
                
        } else
                err = client_get_data(topic, resource, &response);
        
        if (err != 0)
                response_set_status(response, HTTP_Status_Internal_Server_Error);
}

/***************************************************************************/

static int *webproxy_onpart(void *userdata,
                            const unsigned char *data, int len,
                            const char *mimetype,
                            double timestamp)
{
        return 0;
}

int webproxy_stream_onresponse(void *userdata, response_t *response)
{
        return 0;
}

int webproxy_stream_ondata(multipart_parser_t *parser, response_t *response,
                           const char *buf, int len)
{
        multipart_parser_process(parser, response, buf, len);
        return 0;
}

void webproxy_get_stream(request_t *request,
                         const char *topic,
                         const char *resource,
                         response_t *response)
{
        r_debug("webproxy_get_stream: topic %s, resource %s", topic, resource);

        /* multipart_parser_t *parser = NULL; */

        /* parser = new_multipart_parser(NULL, NULL, */
        /*                               (multipart_onpart_t) webproxy_onpart); */

        /* streamerlink_t *link = NULL; */
        /* link = registry_open_streamerlink("webproxy", */
        /*                                   topic, */
        /*                                   webproxy_stream_ondata,  */
        /*                                   webproxy_stream_onresponse, */
        /*                                   parser, */
        /*                                   1); */
        /* if (link == NULL) { */
        /*         response_set_status(response, HTTP_Status_Internal_Server_Error); */
        /*         return; */
        /* } */

}

/***************************************************************************/

void webproxy_onrequest(void *data,
                        request_t *request,
                        response_t *response)
{
	const char *path = request_uri(request);
        list_t* elements = path_break(path);
        list_t* start = elements;

        r_info("%s", path);

        char *s = list_get(start, char);
        if (rstreq(s, "/"))
            start = list_next(start);

        list_t* l_type = start;
        if (l_type == NULL) {
                response_set_status(response, HTTP_Status_Bad_Request);
                return;
        }
        char *type = list_get(l_type, char);
        if (rstreq(type, "coffee")) {
                response_set_status(response, HTTP_Status_Im_A_Teapot);
                return;
        }
        
        list_t* l_topic = list_next(l_type);
        if (l_topic == NULL) {
                response_set_status(response, HTTP_Status_Bad_Request);
                return;
        }
        char *topic = list_get(l_topic, char);
        
        list_t* l_resource = list_next(l_topic);
        if (l_resource == NULL) {
                response_set_status(response, HTTP_Status_Bad_Request);
                return;
        }

        char resource[1024];
        int err = path_glue(l_resource, 1, resource, sizeof(resource));

        int r = 0;
        if (rstreq(type, "service")) {
                webproxy_get_service(request, topic, resource, response);

        } else {
                r_warn("webproxy_onrequest: Bad request: Unknown type '%s'", type);
                response_set_status(response, HTTP_Status_Bad_Request);
        }
        
        path_delete(elements);
}

/***************************************************************************/

static void copy_message_to_client(messagelink_t *link_from_client,
                                   messagelink_t *link_to_hub,
                                   json_object_t message)
{
        if (link_from_client)
                messagelink_send_obj(link_from_client, message);
}

static void copy_message_to_hub(messagelink_t *link_to_hub,
                                messagelink_t *link_from_client,
                                json_object_t message)
{
        if (link_to_hub)
                messagelink_send_obj(link_to_hub, message);
}

static void link_from_client_closed(messagelink_t *link_to_hub,
                                    messagelink_t *link_from_client)
{
        registry_close_messagelink(link_to_hub);
        messagelink_set_userdata(link_from_client, NULL);
}

int webproxy_onconnect(void *userdata, messagehub_t *hub,
                       request_t* request, messagelink_t *link_from_client)
{
        const char *path = request_uri(request);
        
        list_t* elements = path_break(path);
        list_t* start = elements;

        char *s = list_get(start, char);
        if (rstreq(s, "/"))
            start = list_next(start);

        list_t* l_type = start;
        if (l_type == NULL)
                return -1;
        
        char *type = list_get(l_type, char);

        if (!rstreq(type, "messagehub"))
                return -1;
        
        list_t* l_topic = list_next(l_type);
        if (l_topic == NULL)
                return -1;
        
        char *topic = list_get(l_topic, char);
        
        messagelink_t *link_to_hub = NULL;
        link_to_hub = registry_open_messagelink("webproxy",
                                                topic,
                                                (messagelink_onmessage_t) copy_message_to_client,
                                                link_from_client);
        if (link_to_hub == NULL) {
                path_delete(elements);
                return -1;
        }
        
        messagelink_set_userdata(link_from_client, link_to_hub);
        messagelink_set_onmessage(link_from_client,
                                  (messagelink_onmessage_t) copy_message_to_hub);
        messagelink_set_onclose(link_from_client,
                                (messagelink_onclose_t) link_from_client_closed);

        path_delete(elements);
        return 0;
}

