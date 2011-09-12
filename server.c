/*
Copyright © 2011 Johan Liesén

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <assert.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>
#include <event2/util.h>
#include <jansson.h>
#include <libspotify/api.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include "constants.h"
#include "json.h"

#define HTTP_ERROR 500
#define HTTP_NOTIMPL 501
#define PORT 8080

// Application key
extern const unsigned char g_appkey[];
extern const size_t g_appkey_size;

// Account information
extern const char username[];
extern const char password[];

static int exit_status = EXIT_FAILURE;

// Spotify account information
struct account {
  char *username;
  char *password;
};


struct state {
  sp_session *session;

  struct event_base *event_base;
  struct event *async;
  struct event *timer;
  struct event *sigint;
  struct timeval next_timeout;

  struct evhttp *http;

};

typedef void (*handle_playlist_fn)(sp_playlist *playlist,
                                   struct evhttp_request *request,
                                   void *userdata);

// State of a request as it's threaded through libspotify callbacks
struct playlist_handler {
  sp_playlist_callbacks *playlist_callbacks;
  struct evhttp_request *request;
  handle_playlist_fn callback;
  void *userdata;
};

static void send_reply(struct evhttp_request *request,
                       int code,
                       const char *message,
                       struct evbuffer *body) {

  fprintf(stderr, "Got JSON response, attempting to send reply\n");
  evhttp_add_header(evhttp_request_get_output_headers(request),
                    "Content-type", "application/json; charset=UTF-8");
  bool empty_body = body == NULL;

  if (empty_body)
    body = evbuffer_new();

  fprintf(stderr, "Sending the reply\n");
  evhttp_send_reply(request, code, message, body);

  fprintf(stderr, "Freeing body\n");
  if (empty_body)
    evbuffer_free(body);
  fprintf(stderr, "Finished!\n");
}

// Sends JSON to the client (also `free`s the JSON object)
static void send_reply_json(struct evhttp_request *request,
                            int code,
                            const char *message,
                            json_t *json) {

  fprintf(stderr, "Sending the JSON...\n");
  struct evbuffer *buf = evhttp_request_get_output_buffer(request);
  char *json_str = json_dumps(json, JSON_COMPACT);
  json_decref(json);
  evbuffer_add(buf, json_str, strlen(json_str));
  fprintf(stderr, "Freed JSON\n");
  free(json_str);
  send_reply(request, code, message, buf);
  fprintf(stderr, "JSON reply sent!\n");

}

// Will wrap an error message in a JSON object before sending it
static void send_error(struct evhttp_request *request,
                       int code,
                       const char *message) {
  json_t *error_object = json_object();
  json_object_set_new(error_object, "message", json_string(message));
  send_reply_json(request, code, message, error_object);
}

static void send_error_sp(struct evhttp_request *request,
                          int code,
                          sp_error error) {
  const char *message = sp_error_message(error);
  send_error(request, code, message);
}

static struct playlist_handler *register_playlist_callbacks(
    sp_playlist *playlist,
    struct evhttp_request *request,
    handle_playlist_fn callback,
    sp_playlist_callbacks *playlist_callbacks,
    void *userdata) {
  struct playlist_handler *handler = malloc(sizeof (struct playlist_handler));
  handler->request = request;
  handler->callback = callback;
  handler->playlist_callbacks = playlist_callbacks;
  handler->userdata = userdata;
  sp_playlist_add_callbacks(playlist, handler->playlist_callbacks, handler);
  return handler;
}


static void playlist_state_changed(sp_playlist *playlist, void *userdata) {

  fprintf(stderr, "Playlist state changed\n");
  if (!sp_playlist_is_loaded(playlist))
    return;

}

static sp_playlist_callbacks playlist_state_changed_callbacks = {
  .playlist_state_changed = &playlist_state_changed
};



// HTTP handlers

// Standard response handler
static void not_implemented(sp_playlist *playlist,
                            struct evhttp_request *request,
                            void *userdata) {
  evhttp_send_error(request, HTTP_NOTIMPL, "Not Implemented");
}

// Responds with an entire playlist
static void get_playlist(sp_playlist *playlist,
                         struct evhttp_request *request,
                         void *userdata) {

  fprintf(stderr, "Getting playlist\n");
  json_t *json = json_object();

   fprintf(stderr, "Parsing playlist\n");
  if (playlist_to_json(playlist, json) == NULL) {
    json_decref(json);
    send_error(request, HTTP_ERROR, "");
    return;
  }
  fprintf(stderr, "Created JSON response, Sending...\n");
  send_reply_json(request, HTTP_OK, "OK", json);
}

// Request dispatcher
/**
* comments by hugo:
* It seems this is getting called twice? or is it just http standards?
* Dont think so, heres the call structure
* Handle Request
* Got GET request
* Decoding URI
* Got URI entity
* Seems to be a playlist uri...
* Adding playlist to session
* Found playlist
* Added ref
* Got GET request for playlist
* Trying to load playlist...
* Playlist loaded, sending callback.
* Getting playlist
* Parsing playlist
* Created JSON response, Sending...
* Sending the JSON...
* Freed JSON
* Got JSON response, attempting to send reply
* Sending the reply
* Freeing body
* Finished!
* JSON reply sent! --------> Second call to function
* Handle Request
* Got GET request
* Decoding URI
* Got URI entity <--------- Second call to function
**/
static void handle_request(struct evhttp_request *request,
                            void *userdata) {
  evhttp_connection_set_timeout(request->evcon, 1);
  evhttp_add_header(evhttp_request_get_output_headers(request),
                    "Server", "Spotify API");

  fprintf(stderr, "Handle Request\n");
  // Check request method
  int http_method = evhttp_request_get_command(request);

  switch (http_method) {
    case EVHTTP_REQ_GET:
      fprintf(stderr, "Got GET request\n");
      break;

    default:
      evhttp_send_error(request, HTTP_NOTIMPL, "Not Implemented");
      return;
  }

  fprintf(stderr, "Decoding URI\n");
  char *uri = evhttp_decode_uri(evhttp_request_get_uri(request));

  char *entity = strtok(uri, "/");
  fprintf(stderr, "Got URI entity\n");
  if (entity == NULL) {
    evhttp_send_error(request, HTTP_BADREQUEST, "Bad Request");
    free(uri);
    return;
  }

  // Handle requests to /playlist/<playlist_uri>
  if (strncmp(entity, "playlist", 8) != 0) {
    evhttp_send_error(request, HTTP_BADREQUEST, "Bad Request");
    free(uri);
    return;
  }

  char *playlist_uri = strtok(NULL, "/");

  if (playlist_uri == NULL) {
    send_error(request, HTTP_BADREQUEST, "Bad Request");
    free(uri);
    return;
  }

  fprintf(stderr, "Seems to be a playlist uri...\n");
  sp_link *playlist_link = sp_link_create_from_string(playlist_uri);

  if (playlist_link == NULL) {
    send_error(request, HTTP_NOTFOUND, "Playlist link not found");
    free(uri);
    return;
  }

  if (sp_link_type(playlist_link) != SP_LINKTYPE_PLAYLIST) {
    sp_link_release(playlist_link);
    send_error(request, HTTP_BADREQUEST, "Not a playlist link");
    free(uri);
    return;
  }

  struct state *state = userdata;
  sp_session *session = state->session;

  fprintf(stderr, "Adding playlist to session\n");
  sp_playlist *playlist = sp_playlist_create(session, playlist_link);
  sp_link_release(playlist_link);

  if (playlist == NULL) {
    send_error(request, HTTP_NOTFOUND, "Playlist not found");
    free(uri);
    return;
  }
  fprintf(stderr, "Found playlist\n");
  sp_playlist_add_ref(playlist);
  fprintf(stderr, "Added ref\n");

  free(uri);

  // Default request handler
  handle_playlist_fn request_callback = &not_implemented;
  void *callback_userdata = NULL;

  switch (http_method) {
  case EVHTTP_REQ_GET:
    {
      	fprintf(stderr, "Got GET request for playlist\n");
        // Send entire playlist
        request_callback = &get_playlist;
    }
    break;

  }
  fprintf(stderr, "Trying to load playlist...\n");
  if (sp_playlist_is_loaded(playlist)) {
    fprintf(stderr, "Playlist loaded, sending callback.\n");
    request_callback(playlist, request, callback_userdata);
    return;

  } else {
    // Wait for playlist to load
    fprintf(stderr, "Waiting on playlist...\n");
    register_playlist_callbacks(playlist, request, request_callback,
                                &playlist_state_changed_callbacks,
                                callback_userdata);
  }

}

static void playlistcontainer_loaded(sp_playlistcontainer *pc, void *userdata);

static sp_playlistcontainer_callbacks playlistcontainer_callbacks = {
  .container_loaded = playlistcontainer_loaded
};

static void playlistcontainer_loaded(sp_playlistcontainer *pc, void *userdata) {

   fprintf(stderr, "Rootlist synchronized\n");
}

// Catches SIGINT and exits gracefully
static void sigint_handler(evutil_socket_t socket,
                           short what,
                           void *userdata) {
  fprintf(stderr, "Got signal, Handling...\n");
  struct state *state = userdata;
  sp_session_logout(state->session);
}

static void logged_out(sp_session *session) {
  fprintf(stderr, "Logging out from Spotify\n");
  struct state *state = sp_session_userdata(session);
  event_del(state->async);
  event_del(state->timer);
  event_del(state->sigint);
  fprintf(stderr, "Breaking loop\n");
  event_base_loopbreak(state->event_base);
  //apr_pool_destroy(state->pool);
}


static void logged_in(sp_session *session, sp_error error) {
  if (error != SP_ERROR_OK) {
    fprintf(stderr, "%s\n", sp_error_message(error));
    exit_status = EXIT_FAILURE;
    logged_out(session);
    return;
  }

  fprintf(stderr, "Logged in to Spotify API\n");
  struct state *state = sp_session_userdata(session);
  state->session = session;
  evsignal_add(state->sigint, NULL);
  fprintf(stderr, "Waiting on container...\n");
  sp_playlistcontainer *pc = sp_session_playlistcontainer(session);
  sp_playlistcontainer_add_callbacks(pc, &playlistcontainer_callbacks,
                                     session);
  fprintf(stderr, "Container loaded\n");
  fprintf(stderr, "Got %d playlists in container\n", sp_playlistcontainer_num_playlists(pc));
}

static void process_events(evutil_socket_t socket,
                           short what,
                           void *userdata) {

  fprintf(stderr, "processing_events!\n");
  struct state *state = userdata;
  event_del(state->timer);
  int timeout = 0;

  do {
    sp_session_process_events(state->session, &timeout);
  } while (timeout == 0);

  state->next_timeout.tv_sec = timeout / 1000;
  state->next_timeout.tv_usec = (timeout % 1000) * 1000;
  evtimer_add(state->timer, &state->next_timeout);
}

static void notify_main_thread(sp_session *session) {
  fprintf(stderr, "Notifying main thread...\n");
  struct state *state = sp_session_userdata(session);
  event_active(state->async, 0, 1);
}


int main(int argc, char **argv) {

  struct account account = {
    .username = username,
    .password = password
  };

  // Initialize program state
  struct state *state = malloc(sizeof(struct state));
  fprintf(stderr, "Starting pthreads\n");
  // Initialize libev w/ pthreads
  evthread_use_pthreads();
  fprintf(stderr, "Initializing\n");
  state->event_base = event_base_new();
  fprintf(stderr, "Proccessing events\n");
  state->async = event_new(state->event_base, -1, 0, &process_events, state);
  state->timer = evtimer_new(state->event_base, &process_events, state);
  fprintf(stderr, "Sending signals\n");
  state->sigint = evsignal_new(state->event_base, SIGINT, &sigint_handler, state);



  // Initialize libspotify
  sp_session_callbacks session_callbacks = {
	&logged_in,
	&logged_out,
	NULL, //&metadata_updated,
	NULL, //&connection_error,
	NULL,
	&notify_main_thread,
	NULL,
	NULL,
	NULL, //&log_message,
	NULL, // end_of_track
	NULL, // streaming error
	NULL, // userinfo update
	NULL, // start_playback
	NULL, // stop_playback
	NULL, // get_audio_buffer_stats
	NULL, //offline_status_updated,
};


  sp_session_config session_config = {
    .api_version = SPOTIFY_API_VERSION,
    .application_key = g_appkey,
    .application_key_size = g_appkey_size,
    .cache_location = ".cache",
    .callbacks = &session_callbacks,
    .compress_playlists = false,
    .dont_save_metadata_for_playlists = false,
    .settings_location = ".settings",
    .user_agent = "sphttpd",
    .userdata = state,
  };

  sp_session *session;
  sp_error session_create_error = sp_session_create(&session_config,
                                                    &session);

  if (session_create_error != SP_ERROR_OK)
    return EXIT_FAILURE;

  // Log in to Spotify
  sp_session_login(session, account.username, account.password, false);

  /** by hugo:
  * Lets load this up here instead when the container is loaded
  * Seems to be able to load other users playlists faster this way?
  * It can take serveral seconds if playlist belongs to other user and is the
  * first one that gets parsed.
  */
  fprintf(stderr, "Setting up httpd\n");
  state->http = evhttp_new(state->event_base);
  evhttp_set_timeout(state->http, 10);
  fprintf(stderr, "Setting up handler\n");
  evhttp_set_gencb(state->http, &handle_request, state);

  fprintf(stderr, "Binding Socket\n");
  // TODO(liesen): Make address and port configurable
  if (evhttp_bind_socket(state->http, "0.0.0.0", PORT) == -1) {
    fprintf(stderr, "fail\n");
    sp_session_logout(session);
  }
  fprintf(stderr, "Socket bound\n");
  event_base_dispatch(state->event_base);

  fprintf(stderr, "Freeing async state\n");
  event_free(state->async);
  fprintf(stderr, "Freeing timer state\n");
  event_free(state->timer);
  if (state->http != NULL) evhttp_free(state->http);
  event_base_free(state->event_base);
  free(state);
  return exit_status;
}

